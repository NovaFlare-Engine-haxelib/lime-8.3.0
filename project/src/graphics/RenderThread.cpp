#include <graphics/RenderThread.h> 
#include "opengl/OpenGLBindings.h" 
#include <graphics/PerformanceMonitor.h> 
#include <SDL.h> 
#include <sstream> 
#include <stdio.h> 
#include <chrono>
#include <cstdlib>

#ifdef HXCPP_TRACY
#include <hx/TelemetryTracy.h>

#include <TracyClient.cpp>
#endif

namespace lime { 

    std::atomic<int> RenderThread::activePendingFrames(0);
    std::atomic<bool> RenderThread::hasPendingRenderRequest(false);

    RenderThread::RenderThread() : window(nullptr), context(nullptr), workerThread(nullptr), running(false), paused(false), contextHeld(false), swapInterval(1), workerWaiting(false), mainWaiting(false) {
        currentFrame = new Frame();
        currentFrame->reserve(1024);
        
        // Keep only enough reusable frames for the bounded in-flight queue.
        // Typical NovaFlare frames contain roughly 300 commands; reserving
        // 4096 commands across 16 frames retained several MiB permanently.
        for (int i = 0; i < MaxPendingFrames + 1; ++i) {
            Frame* frame = new Frame();
            frame->reserve(1024);
            if (!framePool.push(frame)) {
                delete frame; 
            }
        }
    } 

    RenderThread::~RenderThread() { 
        Stop(); 
        
        Frame* pending = nullptr;
        while (pendingFrames.pop(pending)) {
            if (activePendingFrames.load(std::memory_order_acquire) > 0) {
                activePendingFrames.fetch_sub(1, std::memory_order_acq_rel);
            }
            delete pending;
        }

        if (currentFrame) {
            delete currentFrame;
            currentFrame = nullptr;
        }
        
        Frame* frame = nullptr;
        while (framePool.pop(frame)) {
            delete frame;
        }
    } 

    void RenderThread::Start(SDL_Window* window, SDL_GLContext context) { 
        this->window = window; 
        this->context = context; 
        running = true; 
        workerThread = new std::thread(&RenderThread::Run, this); 
    } 

    void RenderThread::SetContext(SDL_GLContext context) {
        std::lock_guard<std::mutex> lock(mutex);
        this->context = context;
    }

    void RenderThread::Stop() { 
        if (!running) return; 
        
        { 
            std::lock_guard<std::mutex> lock(mutex); 
            running = false; 
            paused = false;
        } 
        condition.notify_all(); 
        
        if (workerThread) { 
            if (workerThread->joinable()) { 
                workerThread->join(); 
            } 
            delete workerThread; 
            workerThread = nullptr; 
        } 
    } 

    void RenderThread::Pause() {
        if (!running || paused) return;
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            paused = true;
        }
        condition.notify_all();
    }

    void RenderThread::Resume() {
        if (!running || !paused) return;
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            paused = false;
        }
        condition.notify_all();
    }

    void RenderThread::RebindContext() {
        PushCommand([this]() {
            if (window && context) {
                if (SDL_GL_MakeCurrent(window, NULL) < 0) {
                    printf("RenderThread::RebindContext: Failed to unbind context: %s\n", SDL_GetError());
                }
                if (SDL_GL_MakeCurrent(window, context) < 0) {
                    printf("RenderThread::RebindContext: Failed to bind context: %s\n", SDL_GetError());
                }
            }
        });
    }

    void RenderThread::Flip(bool forceWait) {
        (void)forceWait;
        if (!running) return;

        if (paused) return;

        if (!currentFrame->empty()) {
            Frame* nextFrame = nullptr;
            
            // A submitted frame is never discarded. If the bounded pool is
            // temporarily exhausted, apply backpressure until the worker
            // returns one.
            if (!framePool.pop(nextFrame)) {
                std::unique_lock<std::mutex> lock(mutex);
                mainWaiting = true;
                condition.wait(lock, [this, &nextFrame] {
                    return framePool.pop(nextFrame) || !running;
                });
                mainWaiting = false;
                if (!running) {
                    if (nextFrame != nullptr) {
                        while (!framePool.push(nextFrame)) {
                            std::this_thread::yield();
                        }
                    }
                    return;
                }
            }

            Frame* submitted = currentFrame;

            currentFrame = nextFrame;
            currentFrame->clear();

            // Publish the in-flight count before the queue item so the worker
            // can never complete a frame before its submission is counted.
            activePendingFrames.fetch_add(1, std::memory_order_acq_rel);
            bool queued = pendingFrames.push(submitted);
            while (!queued) {
                std::unique_lock<std::mutex> lock(mutex);
                mainWaiting = true;
                condition.wait(lock, [this, submitted, &queued] {
                    queued = pendingFrames.push(submitted);
                    return queued || !running;
                });
                mainWaiting = false;
                if (!queued && !running) {
                    activePendingFrames.fetch_sub(
                        1, std::memory_order_acq_rel);
                    submitted->clear();
                    while (!framePool.push(submitted)) {
                        std::this_thread::yield();
                    }
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                condition.notify_all();
            }
        }
    }

    bool RenderThread::IsRenderThread() { 
        return std::this_thread::get_id() == threadId; 
    } 

    void RenderThread::MakeCurrent() {
        if (window && context) {
            if (SDL_GL_MakeCurrent(window, context) < 0) {
                printf("RenderThread::MakeCurrent: Failed to make context current: %s\n", SDL_GetError());
            }
        }
    }

    void RenderThread::SetSwapInterval(int interval) {
        swapInterval = interval;
        if (IsRenderThread()) {
            SDL_GL_SetSwapInterval(interval);
        } else {
            PushCommand([this, interval]() {
                SDL_GL_SetSwapInterval(interval);
            });
        }
    }

    bool RenderThread::WaitForContext(bool held, int timeoutMS) {
        if (!running) return contextHeld.load(std::memory_order_acquire) == held;
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(timeoutMS), [this, held] {
            return !running || contextHeld.load(std::memory_order_acquire) == held;
        });
    }

    void RenderThread::Run() { 
        #ifdef HXCPP_TRACY
        tracy::SetThreadName("Render Thread");
        #endif

        threadId = std::this_thread::get_id(); 
        const char* perfTraceValue = std::getenv("NOVAGC_PERF_TRACE");
        const bool perfTrace = perfTraceValue != nullptr &&
            perfTraceValue[0] != '\0' && perfTraceValue[0] != '0';
        auto perfWindowStarted = std::chrono::steady_clock::now();
        std::uint64_t perfFrames = 0;
        std::uint64_t perfCommands = 0;
        std::uint64_t perfExecutionMicros = 0;
        std::uint64_t perfMaximumExecutionMicros = 0;

        // Make context current on this thread 
        if (window && context) { 
            if (SDL_GL_MakeCurrent(window, context) < 0) {
                printf("RenderThread::Run: Initial context bind failed: %s\n", SDL_GetError());
            } else {
                contextHeld = true;
                    std::lock_guard<std::mutex> lock(mutex);
                    condition.notify_all();
            }
        } 

        while (running) { 
            #ifdef HXCPP_TRACY
            ZoneScopedN("Render Loop");
            #endif

            if (paused) {
                
                if (window && context) {
                    if (SDL_GL_MakeCurrent(window, NULL) < 0) {
                        printf("RenderThread::Run: Pause context unbind failed: %s\n", SDL_GetError());
                    }
                    contextHeld = false;
                    std::lock_guard<std::mutex> lock(mutex);
                    condition.notify_all();
                }

                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this] { return !paused || !running; });
                }

                if (!running) break;

                // When resumed, re-bind the context to ensure we target the correct surface
                // This is crucial for Android where the surface might have been recreated
                if (window && context) {
                    if (SDL_GL_MakeCurrent(window, context) < 0) {
                        printf("RenderThread::Run: Resume context bind failed: %s\n", SDL_GetError());
                    } else {
                        contextHeld = true;
                        SDL_GL_SetSwapInterval(swapInterval);
                        std::lock_guard<std::mutex> lock(mutex);
                        condition.notify_all();
                    }
                }
            }
            
            Frame* frame = nullptr;

            pendingFrames.pop(frame);

            if (frame == nullptr) {
                std::unique_lock<std::mutex> lock(mutex);
                workerWaiting = true;
                if (pendingFrames.empty() && running && !paused && !hasPendingRenderRequest) {
                    condition.wait(lock, [this] {
                        return !pendingFrames.empty() || !running || paused || hasPendingRenderRequest;
                    });
                }
                workerWaiting = false;

                if (!running) break;
                if (paused) continue;
                
                pendingFrames.pop(frame);
            }
            
            if (frame) {
                if (!frame->empty()) {
                    const std::size_t commandCount = frame->size();
                    const auto executionStarted =
                        std::chrono::steady_clock::now();
                    for (auto& command : *frame) {
                        if (command) {
                            command();
                        }
                    }
                    const std::uint64_t executionMicros =
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() -
                                    executionStarted).count());
                    frame->clear();

                    if (perfTrace) {
                        ++perfFrames;
                        perfCommands += commandCount;
                        perfExecutionMicros += executionMicros;
                        if (executionMicros > perfMaximumExecutionMicros)
                            perfMaximumExecutionMicros = executionMicros;
                        const auto now = std::chrono::steady_clock::now();
                        if (now - perfWindowStarted >=
                            std::chrono::seconds(1)) {
                            const double averageCommands = perfFrames == 0
                                ? 0.0
                                : static_cast<double>(perfCommands) /
                                    static_cast<double>(perfFrames);
                            const double averageExecutionMs = perfFrames == 0
                                ? 0.0
                                : static_cast<double>(perfExecutionMicros) /
                                    static_cast<double>(perfFrames) / 1000.0;
                            std::printf(
                                "perf:RenderThread frames=%llu commands=%llu "
                                "avg_commands=%.2f exec_avg_ms=%.3f "
                                "exec_max_ms=%.3f\n",
                                static_cast<unsigned long long>(perfFrames),
                                static_cast<unsigned long long>(perfCommands),
                                averageCommands, averageExecutionMs,
                                static_cast<double>(
                                    perfMaximumExecutionMicros) / 1000.0);
                            std::fflush(stdout);
                            perfWindowStarted = now;
                            perfFrames = 0;
                            perfCommands = 0;
                            perfExecutionMicros = 0;
                            perfMaximumExecutionMicros = 0;
                        }
                    }

                    #ifdef HXCPP_TRACY
                    FrameMarkNamed("RenderLoop");
                    #endif
                }

                activePendingFrames.fetch_sub(1, std::memory_order_acq_rel);
                
                while (!framePool.push(frame)) {
                    std::unique_lock<std::mutex> lock(mutex);
                    workerWaiting = true;
                    condition.wait(lock, [this, frame] {
                        return framePool.push(frame) || !running;
                    });
                    workerWaiting = false;
                }
                
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    condition.notify_all();
                }
            }
            
            if (hasPendingRenderRequest) {
                SDL_Event event;
                SDL_UserEvent userevent;
                userevent.type = SDL_USEREVENT;
                userevent.code = 1; 
                userevent.data1 = NULL;
                userevent.data2 = NULL;
                event.type = SDL_USEREVENT;
                event.user = userevent;
                
                if (SDL_PushEvent(&event) == 1) {
                    hasPendingRenderRequest = false;
                } else {
                    std::this_thread::yield();
                }
            }
        } 

        // Cleanup 
        if (window && context) { 
            if (SDL_GL_MakeCurrent(window, NULL) < 0) {
                 printf("RenderThread::Run: Cleanup context unbind failed: %s\n", SDL_GetError());
            }
            contextHeld = false;
            std::lock_guard<std::mutex> lock(mutex);
            condition.notify_all();
        } 
    } 

} 

#include <graphics/RenderThread.h> 
#include "opengl/OpenGLBindings.h" 
#include <graphics/PerformanceMonitor.h> 
#include <SDL.h> 
#include <sstream> 
#include <stdio.h> 

#ifdef HXCPP_TRACY
#include <hx/TelemetryTracy.h>

#include <TracyClient.cpp>
#endif

namespace lime { 

    std::atomic<int> RenderThread::activePendingFrames(0);
    std::atomic<bool> RenderThread::hasPendingRenderRequest(false);

    RenderThread::RenderThread() : window(nullptr), context(nullptr), workerThread(nullptr), running(false), paused(false), contextHeld(false), swapInterval(1), workerWaiting(false), mainWaiting(false) {
        currentFrame = new Frame();
        currentFrame->reserve(4096);
        
        // Pre-allocate pool
        for (int i = 0; i < 3; ++i) {
            Frame* frame = new Frame();
            frame->reserve(4096);
            if (!framePool.push(frame)) {
                delete frame; 
            }
        }
    } 

    RenderThread::~RenderThread() { 
        Stop(); 
        
        if (currentFrame) {
            delete currentFrame;
            currentFrame = nullptr;
        }
        
        Frame* frame = nullptr;
        while (frameQueue.pop(frame)) {
            delete frame;
        }
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

    void RenderThread::PushCommand(std::function<void()> command) { 
        // No lock needed for SPSC (Main Thread only)
        currentFrame->push_back(std::move(command));
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

    void RenderThread::Flip() {
        if (!running) return;

        if (!currentFrame->empty()) {
            Frame* nextFrame = nullptr;
            
            // Wait for a free frame from pool
            if (!framePool.pop(nextFrame)) {
                std::unique_lock<std::mutex> lock(mutex);
                mainWaiting = true;
                condition.wait(lock, [this, &nextFrame] {
                    return framePool.pop(nextFrame) || !running;
                });
                mainWaiting = false;
                if (!running) return;
            }
            
            // Push filled frame to queue
            if (!frameQueue.push(currentFrame)) {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    if (!running) return true;
                    return frameQueue.push(currentFrame);
                });
                
                if (!running) {
                    framePool.push(nextFrame);
                    return;
                }
            }
            
            activePendingFrames++;
            
            // Swap
            currentFrame = nextFrame;
            currentFrame->clear();
            
            // Wake up worker
            if (workerWaiting) {
                std::lock_guard<std::mutex> lock(mutex);
                condition.notify_one(); 
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

    void RenderThread::Run() { 
        #ifdef HXCPP_TRACY
        tracy::SetThreadName("Render Thread");
        #endif

        threadId = std::this_thread::get_id(); 

        // Make context current on this thread 
        if (window && context) { 
            if (SDL_GL_MakeCurrent(window, context) < 0) {
                printf("RenderThread::Run: Initial context bind failed: %s\n", SDL_GetError());
            } else {
                contextHeld = true;
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
                    }
                }
            }
            
            Frame* frame = nullptr;

            // Optimistic lock-free pop
            bool gotFrame = frameQueue.pop(frame);
            
            if (!gotFrame) {
                 // Empty, wait using CV
                 std::unique_lock<std::mutex> lock(mutex);
                 // Check activePendingFrames as a robust fallback for "not empty"
                 workerWaiting = true;
                 if (frameQueue.empty() && running && !paused && !hasPendingRenderRequest) {
                     condition.wait(lock, [this] { 
                         return !frameQueue.empty() || !running || paused || hasPendingRenderRequest; 
                     });
                 }
                 workerWaiting = false;
                 
                 if (!running) break;
                 if (paused) continue;
                 
                 gotFrame = frameQueue.pop(frame);
            }
            
            if (gotFrame && frame) {
                if (!frame->empty()) {
                    for (auto& command : *frame) {
                        if (command) {
                            command();
                        }
                    }
                    frame->clear();
                    
                    activePendingFrames--;

                    #ifdef HXCPP_TRACY
                    FrameMarkNamed("RenderLoop"); 
                    #endif
                }
                
                while (!framePool.push(frame)) {
                    std::unique_lock<std::mutex> lock(mutex);
                    workerWaiting = true;
                    condition.wait(lock, [this, frame] {
                        return framePool.push(frame) || !running;
                    });
                    workerWaiting = false;
                }
                if (mainWaiting) {
                    std::lock_guard<std::mutex> lock(mutex);
                    condition.notify_one();
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
        } 
    } 

} 

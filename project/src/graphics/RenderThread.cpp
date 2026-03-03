#include <graphics/RenderThread.h> 
#include "opengl/OpenGLBindings.h" 
#include <graphics/PerformanceMonitor.h> 
#include <SDL.h> 
#include <sstream> 

#ifdef HXCPP_TRACY
#include <hx/TelemetryTracy.h>

#include <TracyClient.cpp>
#endif

namespace lime { 

    std::atomic<int> RenderThread::activePendingFrames(0);
    std::atomic<bool> RenderThread::hasPendingRenderRequest(false);

    RenderThread::RenderThread() : window(nullptr), context(nullptr), workerThread(nullptr), running(false), paused(false), contextHeld(false), swapInterval(1) {
        currentFrame = new Frame();
        currentFrame->reserve(4096);
        
        // Pre-allocate pool
        // Size 32 queue, capacity is 31.
        for (int i = 0; i < 31; ++i) {
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
                SDL_GL_MakeCurrent(window, NULL);
                SDL_GL_MakeCurrent(window, context);
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
                condition.wait(lock, [this, &nextFrame] {
                    return framePool.pop(nextFrame) || !running;
                });
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
            {
                std::lock_guard<std::mutex> lock(mutex);
                condition.notify_one(); // notify_one is enough and more efficient
            }
        }
    }

    bool RenderThread::IsRenderThread() { 
        return std::this_thread::get_id() == threadId; 
    } 

    void RenderThread::MakeCurrent() {
        if (window && context) {
            SDL_GL_MakeCurrent(window, context);
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
            SDL_GL_MakeCurrent(window, context); 
            contextHeld = true;
        } 

        while (running) { 
            #ifdef HXCPP_TRACY
            ZoneScopedN("Render Loop");
            #endif

            if (paused) {
                
                if (window && context) {
                    SDL_GL_MakeCurrent(window, NULL);
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
                    SDL_GL_MakeCurrent(window, context);
                    contextHeld = true;
                    SDL_GL_SetSwapInterval(swapInterval);
                }
            }
            
            Frame* frame = nullptr;

            // Optimistic lock-free pop
            bool gotFrame = frameQueue.pop(frame);
            
            if (!gotFrame) {
                 // Empty, wait using CV
                 std::unique_lock<std::mutex> lock(mutex);
                 // Check activePendingFrames as a robust fallback for "not empty"
                 if (frameQueue.empty() && running && !paused && !hasPendingRenderRequest) {
                     condition.wait(lock, [this] { 
                         return !frameQueue.empty() || !running || paused || hasPendingRenderRequest; 
                     });
                 }
                 
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
                    condition.wait(lock, [this, frame] {
                        return framePool.push(frame) || !running;
                    });
                }
                {
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } 

        // Cleanup 
        if (window && context) { 
            SDL_GL_MakeCurrent(window, NULL); 
            contextHeld = false;
        } 
    } 

} 

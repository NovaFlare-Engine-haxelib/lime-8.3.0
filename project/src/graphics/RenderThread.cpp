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

    RenderThread::RenderThread() : window(nullptr), context(nullptr), workerThread(nullptr), running(false), paused(false) {
        currentFrame = new Frame();
        currentFrame->reserve(4096);
        
        // Pre-allocate pool
        // Size 16 queue, capacity is 15.
        for (int i = 0; i < 16; ++i) {
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
            // If the application logic (SDLApplication.cpp) is working correctly,
            // activePendingFrames should throttle the main thread before we run out of pool frames.
            // However, if we do run out, we must wait or allocate new ones.
            // Since we are in a Lock-Free design, allocating new ones is tricky without a lock on the pool.
            // But since this is SPSC, we are the only consumer of the pool.
            
            while (!framePool.pop(nextFrame)) {
                if (!running) return;
                // If pool is empty, it means the render thread is falling behind.
                // We can yield to let it catch up.
                std::this_thread::yield();
            }
            
            // Push filled frame to queue
            // Since we have a pool, the queue should ideally never be full if pool size >= queue size.
            // But if it is full (shouldn't happen with correct logic), we wait.
            while (!frameQueue.push(currentFrame)) {
                if (!running) {
                    framePool.push(nextFrame);
                    return;
                }
                std::this_thread::yield();
            }
            
            activePendingFrames++;
            
            // Swap
            currentFrame = nextFrame;
            currentFrame->clear();
            
            // Wake up worker
            condition.notify_all();
        }
    }

    bool RenderThread::IsRenderThread() { 
        return std::this_thread::get_id() == threadId; 
    } 

    void RenderThread::Run() { 
        #ifdef HXCPP_TRACY
        tracy::SetThreadName("Render Thread");
        #endif

        threadId = std::this_thread::get_id(); 

        // Make context current on this thread 
        if (window && context) { 
            SDL_GL_MakeCurrent(window, context); 
        } 

        while (running) { 
            #ifdef HXCPP_TRACY
            ZoneScopedN("Render Loop");
            #endif

            if (paused) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this] { return !paused || !running; });
                }

                if (!running) break;

                if (window && context) {
                    SDL_GL_MakeCurrent(window, context);
                }
            }
            
            Frame* frame = nullptr;

            // Optimistic lock-free pop
            if (frameQueue.pop(frame)) {
                 // Got work
            } else {
                 // Empty, wait using CV
                 std::unique_lock<std::mutex> lock(mutex);
                 // Check activePendingFrames as a robust fallback for "not empty"
                 if (frameQueue.empty() && running && !paused && activePendingFrames == 0) {
                     condition.wait(lock, [this] { 
                         return !frameQueue.empty() || !running || paused || activePendingFrames > 0; 
                     });
                 }
                 
                 if (!running) break;
                 if (paused) continue;
                 
                 if (!frameQueue.pop(frame)) continue;
            }
            
            if (frame) {
                if (!frame->empty()) {
                    for (auto& command : *frame) {
                        if (command) {
                            command();
                        }
                    }
                    frame->clear();
                    
                    activePendingFrames--;

                    if (hasPendingRenderRequest) {
                        hasPendingRenderRequest = false;
                        
                        SDL_Event event;
                        SDL_UserEvent userevent;
                        userevent.type = SDL_USEREVENT;
                        userevent.code = 1; 
                        userevent.data1 = NULL;
                        userevent.data2 = NULL;
                        event.type = SDL_USEREVENT;
                        event.user = userevent;
                        
                        SDL_PushEvent(&event);
                    }

                    #ifdef HXCPP_TRACY
                    FrameMarkNamed("RenderLoop"); 
                    #endif
                }
                
                while (!framePool.push(frame)) {
                    std::this_thread::yield();
                }
            }
        } 

        // Cleanup 
        if (window && context) { 
            SDL_GL_MakeCurrent(window, NULL); 
        } 
    } 

} 

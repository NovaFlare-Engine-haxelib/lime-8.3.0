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

    RenderThread::RenderThread() : window(nullptr), context(nullptr), workerThread(nullptr), running(false), maxPendingFrames(1) {
        currentFrame.reserve(4096);
    } 

    RenderThread::~RenderThread() { 
        Stop(); 
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
        std::lock_guard<std::mutex> lock(mutex); 
        currentFrame.push_back(std::move(command));
    } 

    void RenderThread::Flip() {
        std::unique_lock<std::mutex> lock(mutex);
        
        if (!running) return;

        if (!currentFrame.empty()) {
            frameQueue.push_back(std::move(currentFrame));
            activePendingFrames++;
            
            // Prepare next frame
            currentFrame = std::vector<std::function<void()>>();
            currentFrame.reserve(4096);
            
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
            
            std::vector<std::function<void()>> frame;

            { 
                std::unique_lock<std::mutex> lock(mutex); 
                condition.wait(lock, [this] { return !frameQueue.empty() || !running; }); 
                
                if (!running && frameQueue.empty()) break; 
                
                if (!frameQueue.empty()) {
                    frame = std::move(frameQueue.front());
                    frameQueue.pop_front();
                    
                    // Notify main thread (Flip) that space is available
                    condition.notify_all(); 
                }
            } 
            
            // Execute all commands in the frame
            if (!frame.empty()) {
                for (auto& command : frame) {
                    if (command) {
                        command();
                    }
                }
                frame.clear();
                
                activePendingFrames--;

                if (hasPendingRenderRequest) {
                    hasPendingRenderRequest = false;
                    
                    SDL_Event event;
                    SDL_UserEvent userevent;
                    userevent.type = SDL_USEREVENT;
                    userevent.code = 1; // 1 = Render Event code in SDLApplication
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
        } 

        // Cleanup 
        if (window && context) { 
            SDL_GL_MakeCurrent(window, NULL); 
        } 
    } 

} 

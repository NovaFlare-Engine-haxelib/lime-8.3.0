#ifndef LIME_GRAPHICS_RENDER_THREAD_H
#define LIME_GRAPHICS_RENDER_THREAD_H

#include <SDL.h>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <deque>

namespace lime {

    class RenderThread {
    public:
        RenderThread();
        ~RenderThread();

        void Start(SDL_Window* window, SDL_GLContext context);
        void Stop();
        
        void PushCommand(std::function<void()> command);
        void Flip();
        
        template<typename T>
        T RunCommandAndWait(std::function<T()> command) {
            if (IsRenderThread()) {
                return command();
            }
            //printf("[RenderThread] RunCommandAndWait<T>: Pushing command...\n"); fflush(stdout);
            std::promise<T> promise;
            std::future<T> future = promise.get_future();
            PushCommand([&]() {
                promise.set_value(command());
            });
            //printf("[RenderThread] RunCommandAndWait<T>: Calling Flip...\n"); fflush(stdout);
            Flip();
            //printf("[RenderThread] RunCommandAndWait<T>: Waiting for future...\n"); fflush(stdout);
            T result = future.get();
            //printf("[RenderThread] RunCommandAndWait<T>: Done.\n"); fflush(stdout);
            return result;
        }

        template<>
        void RunCommandAndWait<void>(std::function<void()> command) {
             if (IsRenderThread()) {
                command();
                return;
            }
            //printf("[RenderThread] RunCommandAndWait: Pushing command...\n"); fflush(stdout);
            std::promise<void> promise;
            std::future<void> future = promise.get_future();
            PushCommand([&]() {
                command();
                promise.set_value();
            });
            //printf("[RenderThread] RunCommandAndWait: Calling Flip...\n"); fflush(stdout);
            Flip();
            //printf("[RenderThread] RunCommandAndWait: Waiting for future...\n"); fflush(stdout);
            future.get();
            //printf("[RenderThread] RunCommandAndWait: Done.\n"); fflush(stdout);
        }

        bool IsRenderThread();
        bool IsRunning() { return running; }
        
        static std::atomic<int> activePendingFrames;
        static std::atomic<bool> hasPendingRenderRequest;
    private:
        void Run();

        SDL_Window* window;
        SDL_GLContext context;
        std::thread* workerThread;
        std::atomic<bool> running;
        
        std::condition_variable condition;
        std::mutex mutex;
        
        std::deque<std::vector<std::function<void()>>> frameQueue;
        std::vector<std::function<void()>> currentFrame;
        int maxPendingFrames;
        
        std::thread::id threadId;
    };

}

#endif

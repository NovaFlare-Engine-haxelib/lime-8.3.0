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
#include <array>

namespace lime {

    template<typename T, size_t Size>
    class LockFreeQueue {
    public:
        LockFreeQueue() : head(0), tail(0) {}
        
        bool push(const T& value) {
            size_t t = tail.load(std::memory_order_relaxed);
            size_t next = (t + 1) % Size;
            if (next == head.load(std::memory_order_acquire)) return false;
            buffer[t] = value;
            tail.store(next, std::memory_order_release);
            return true;
        }

        bool pop(T& value) {
            size_t h = head.load(std::memory_order_relaxed);
            if (h == tail.load(std::memory_order_acquire)) return false;
            value = std::move(buffer[h]);
            head.store((h + 1) % Size, std::memory_order_release);
            return true;
        }
        
        bool empty() const {
            return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_acquire);
        }

    private:
        std::array<T, Size> buffer;
        std::atomic<size_t> head;
        std::atomic<size_t> tail;
    };

    class RenderThread {
    public:
        RenderThread();
        ~RenderThread();

        void Start(SDL_Window* window, SDL_GLContext context);
        void Stop();
        void Pause();
        void Resume();
        void RebindContext();
        
        void PushCommand(std::function<void()> command);
        void Flip();
        
        template<typename T>
        T RunCommandAndWait(std::function<T()> command) {
            if (IsRenderThread()) {
                return command();
            }
            std::promise<T> promise;
            std::future<T> future = promise.get_future();
            PushCommand([&]() {
                promise.set_value(command());
            });
            Flip();
            return future.get();
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
        std::atomic<bool> paused;
        
        std::condition_variable condition;
        std::mutex mutex;
        
        // Use pointers to vectors to avoid copying and enable lightweight swapping
        using Frame = std::vector<std::function<void()>>;
        
        LockFreeQueue<Frame*, 16> frameQueue;
        LockFreeQueue<Frame*, 16> framePool;
        Frame* currentFrame;
        
        std::thread::id threadId;
    };

    template<>
    inline void RenderThread::RunCommandAndWait<void>(std::function<void()> command) {
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

}

#endif

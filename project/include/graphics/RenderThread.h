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
        void SetContext(SDL_GLContext context);
        
        void PushCommand(std::function<void()> command);
        void Flip(bool forceWait = false);
        
        template<typename T>
        T RunCommandAndWait(std::function<T()> command) {
            if (IsRenderThread()) {
                return command();
            }
            std::promise<T> promise;
            std::future<T> future = promise.get_future();
            PushCommand([&]() {
                try {
                    promise.set_value(command());
                } catch (...) {
                    promise.set_exception(std::current_exception());
                }
            });
            Flip(true);
            return future.get();
        }

        bool IsRenderThread();
        bool IsRunning() { return running; }
        void MakeCurrent();
        void SetSwapInterval(int interval);
        
        static std::atomic<int> activePendingFrames;
        static std::atomic<bool> hasPendingRenderRequest;
        std::atomic<bool> contextHeld;
        
        // Flags for conditional notify to reduce lock contention
        std::atomic<bool> workerWaiting;
        std::atomic<bool> mainWaiting;

    private:
        void Run();

        SDL_Window* window;
        SDL_GLContext context;
        std::thread* workerThread;
        std::atomic<bool> running;
        std::atomic<bool> paused;
        std::atomic<int> swapInterval;
        
        std::condition_variable condition;
        std::mutex mutex;
        
        // Use pointers to vectors to avoid copying and enable lightweight swapping
        using Frame = std::vector<std::function<void()>>;
        
        LockFreeQueue<Frame*, 16> framePool;
        std::atomic<Frame*> pendingFrame;
        Frame* currentFrame;
        
        std::thread::id threadId;
    };

    template<>
    inline void RenderThread::RunCommandAndWait<void>(std::function<void()> command) {
         if (IsRenderThread()) {
            command();
            return;
        }
        std::promise<void> promise;
        std::future<void> future = promise.get_future();
        PushCommand([&]() {
            try {
                command();
                promise.set_value();
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });
        Flip(true);
        future.get();
    }

}

#endif

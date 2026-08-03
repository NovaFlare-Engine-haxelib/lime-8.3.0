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
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace lime {

    // libstdc++'s std::function stores only a very small callable inline.
    // OpenGL command lambdas commonly capture several values, so the old
    // queue performed a native heap allocation and free for nearly every GL
    // call (hundreds of thousands per second at high frame rates). This
    // move-only command keeps ordinary captures in the reusable Frame vector.
    class RenderCommand {
    public:
        static constexpr std::size_t InlineBytes = 64;

        RenderCommand() noexcept = default;

        template<typename Callable,
            typename std::enable_if<!std::is_same<
                typename std::decay<Callable>::type,
                RenderCommand>::value, int>::type = 0>
        explicit RenderCommand(Callable&& callable) {
            using Function = typename std::decay<Callable>::type;
            constexpr bool fitsInline = sizeof(Function) <= InlineBytes &&
                alignof(Function) <= alignof(std::max_align_t) &&
                std::is_nothrow_move_constructible<Function>::value;

            invoke = [](void* value) {
                (*static_cast<Function*>(value))();
            };
            initialize<Function>(std::forward<Callable>(callable),
                std::integral_constant<bool, fitsInline>());
        }

        RenderCommand(const RenderCommand&) = delete;
        RenderCommand& operator=(const RenderCommand&) = delete;

        RenderCommand(RenderCommand&& other) noexcept {
            moveFrom(other);
        }

        RenderCommand& operator=(RenderCommand&& other) noexcept {
            if (this != &other) {
                reset();
                moveFrom(other);
            }
            return *this;
        }

        ~RenderCommand() {
            reset();
        }

        explicit operator bool() const noexcept { return invoke != nullptr; }

        void operator()() {
            invoke(object);
        }

    private:
        using Invoke = void (*)(void*);
        using Destroy = void (*)(void*);
        using MoveInline = void (*)(void*, void*);

        template<typename Function, typename Callable>
        void initialize(Callable&& callable, std::true_type) {
            ::new (static_cast<void*>(storage))
                Function(std::forward<Callable>(callable));
            object = storage;
            destroy = [](void* value) noexcept {
                static_cast<Function*>(value)->~Function();
            };
            moveInline = [](void* destination, void* source) noexcept {
                Function* input = static_cast<Function*>(source);
                ::new (destination) Function(std::move(*input));
                input->~Function();
            };
            inlineObject = true;
        }

        template<typename Function, typename Callable>
        void initialize(Callable&& callable, std::false_type) {
            object = new Function(std::forward<Callable>(callable));
            destroy = [](void* value) noexcept {
                delete static_cast<Function*>(value);
            };
        }

        void reset() noexcept {
            if (destroy != nullptr && object != nullptr)
                destroy(object);
            object = nullptr;
            invoke = nullptr;
            destroy = nullptr;
            moveInline = nullptr;
            inlineObject = false;
        }

        void moveFrom(RenderCommand& other) noexcept {
            invoke = other.invoke;
            destroy = other.destroy;
            moveInline = other.moveInline;
            inlineObject = other.inlineObject;
            if (other.inlineObject) {
                moveInline(storage, other.object);
                object = storage;
            } else {
                object = other.object;
            }
            other.object = nullptr;
            other.invoke = nullptr;
            other.destroy = nullptr;
            other.moveInline = nullptr;
            other.inlineObject = false;
        }

        alignas(std::max_align_t) unsigned char storage[InlineBytes];
        void* object = nullptr;
        Invoke invoke = nullptr;
        Destroy destroy = nullptr;
        MoveInline moveInline = nullptr;
        bool inlineObject = false;
    };

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
        
        template<typename Callable>
        void PushCommand(Callable&& command) {
            // Render submission is SPSC: only the stage thread appends to the
            // current frame, and only the worker consumes submitted frames.
            currentFrame->emplace_back(std::forward<Callable>(command));
        }
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
        bool WaitForContext(bool held, int timeoutMS);
        
        static constexpr int MaxPendingFrames = 2;
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
        using Frame = std::vector<RenderCommand>;
        
        LockFreeQueue<Frame*, 16> framePool;
        LockFreeQueue<Frame*, 16> pendingFrames;
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

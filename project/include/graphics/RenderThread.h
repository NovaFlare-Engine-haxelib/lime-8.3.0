#ifndef LIME_GRAPHICS_RENDER_THREAD_H
#define LIME_GRAPHICS_RENDER_THREAD_H

#include <SDL.h>
#include <graphics/FrameTimeline.h>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <vector>
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
        struct Submission {
            Submission(bool wasAccepted = false, std::uint64_t batchId = 0)
                : accepted(wasAccepted), batch(batchId) {}
            bool accepted;
            std::uint64_t batch;
        };

        RenderThread();
        ~RenderThread();

        void Start(SDL_Window* initialWindow, SDL_GLContext initialContext, int initialSwapInterval = 1);
        void Stop();
        bool PauseAndReleaseContext(int timeoutMS);
        bool RequestContextRelease();
        bool ResumeAndAcquireContext(int timeoutMS);
        bool CanAcquireContext();
        bool SetContext(SDL_GLContext newContext);
        void SetSubmissionEnabled(bool enabled);
        bool RecordViewport(int x, int y, int width, int height);
        bool RecordActiveTexture(bool textureZero);
        
        template<typename Callable>
        bool PushCommand(Callable&& command) {
            if (!IsApplicationThread() || !CanSubmit()) return false;
            currentFrame->commands.emplace_back(std::forward<Callable>(command));
            return true;
        }
        Submission Flip();
        void BeginTimelinePhase() {
#ifdef HX_WINDOWS
            if (IsApplicationThread() && FrameTimeline::CurrentWork() && !currentFrame->commands.empty())
                currentFrame->mixedTimelinePhase = true;
#endif
        }
        
        template<typename T>
        T RunCommandAndWait(const char* operation, std::function<T()> command) {
            return ExecuteCommandAndWait(operation, std::move(command), [] { return T(); });
        }

        template<typename T>
        T RunCommandAndWaitOr(const char* operation, std::function<T()> command, T failureValue) {
            return ExecuteCommandAndWait(operation, std::move(command), [&failureValue] { return std::move(failureValue); });
        }

        bool IsRenderThread() const;
        bool IsRunning() const { return running; }
#ifdef ANDROID
        void WaitForPendingFrames(std::chrono::nanoseconds timeout);
#endif
        bool MakeCurrent();
        bool SetSwapInterval(int interval, bool useWorker = false);
        bool ApplySwapInterval();
        bool SetCurrentSwapInterval(int interval);
        int ConfiguredSwapInterval() const { return swapInterval.load(); }
        int CurrentSwapInterval() const { return currentSwapInterval.load(); }
        std::uint64_t SwapIntervalRevision() const { return swapIntervalRevision.load(std::memory_order_relaxed); }
        bool WaitForContext(bool held, int timeoutMS);

        static constexpr int MaxPendingFrames = 2;
        static std::atomic<int> activePendingFrames;

    private:
        template<typename T, typename OnUnavailable>
        T ExecuteCommandAndWait(const char* operation, std::function<T()> command, OnUnavailable onUnavailable) {
            if (IsRenderThread()) return command();
            if (!IsApplicationThread() || !CanSubmit()) return onUnavailable();

            FrameTimeline::Span wait(FrameTimeline::SyncEvent(), FrameTimeline::CurrentWork(), operation);
            // Dropping an unstarted batch destroys the task and wakes its waiter.
            std::packaged_task<T()> task(std::move(command));
            auto future = task.get_future();
            currentFrame->commands.emplace_back(std::move(task));
            const Submission submission = SubmitFrame(true);
            if (!submission.accepted) {
                currentFrame->commands.pop_back();
                return onUnavailable();
            }

            wait.SetBatch(submission.batch);
            try {
                return future.get();
            } catch (const std::future_error& error) {
                if (error.code() != std::make_error_code(std::future_errc::broken_promise)) throw;
                return onUnavailable();
            }
        }

        enum class ContextState { Released, Acquiring, Held, Releasing };
        using Deadline = std::chrono::steady_clock::time_point;

        void Run();
        bool IsApplicationThread() const;
        bool IsContextCurrent() const;
        bool CanSubmit(bool draining = false) const;
        bool WorkerContextIs(bool held) const;
        bool RequestContextRelease(Deadline deadline);
        bool WaitForWorkerContext(std::unique_lock<std::mutex>& lock, bool held, Deadline deadline);
        void ChangeWorkerContext(std::unique_lock<std::mutex>& lock, bool acquire);
        Submission SubmitFrame(bool synchronous, Deadline deadline = (Deadline::max)(), bool draining = false);

        SDL_Window* window;
        SDL_GLContext context;
        std::thread workerThread;
        const std::thread::id applicationThread;
        std::atomic<bool> running{false};
        std::atomic<bool> submissionEnabled{true};
        std::atomic<bool> contextRequested{false};
        std::atomic<ContextState> contextState{ContextState::Released};
        bool contextError = false;
        std::atomic<int> swapInterval;
        std::atomic<int> currentSwapInterval{0};
        std::atomic<std::uint64_t> swapIntervalRevision{0};
        
        std::condition_variable condition;
        std::mutex mutex;
        std::mutex workerMutex;
        
        // Use pointers to vectors to avoid copying and enable lightweight swapping
        struct Frame {
            std::vector<RenderCommand> commands;
            std::array<int, 4> viewport{};
            bool viewportRecorded = false;
            bool textureZeroRecorded = false;

            void ResetRecordedGLState() {
                viewportRecorded = false;
                textureZeroRecorded = false;
            }
#ifdef HX_WINDOWS
            std::uint64_t timelineId = 0;
            std::uint64_t timelineBatch = 0;
            std::uint64_t submittedAt = 0;
            int queueDepth = 0;
            bool synchronization = false;
            bool timelineUpdate = false;
            bool mixedTimelinePhase = false;
#endif
#ifdef HX_ANDROID
            std::uint64_t androidCycle = 0;
            bool androidFinalSubmission = false;
#endif
        };
        
        LockFreeQueue<Frame*, 16> framePool;
        LockFreeQueue<Frame*, 16> pendingFrames;
        Frame* currentFrame;

        Frame* WaitForFrame();
        void ReturnFrame(Frame* frame);
    };

}

#endif

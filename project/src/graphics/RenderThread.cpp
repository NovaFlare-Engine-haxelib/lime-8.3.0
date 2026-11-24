#include <graphics/AndroidPerformanceHints.h>
#include <graphics/RenderThread.h>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#ifdef HXCPP_TRACY
#include <hx/TelemetryTracy.h>

#include <TracyClient.cpp>
#endif

namespace lime {

    std::atomic<int> RenderThread::activePendingFrames(0);
    static thread_local RenderThread* currentRenderThread = nullptr;

    RenderThread::RenderThread()
        : window(nullptr), context(nullptr), applicationThread(std::this_thread::get_id()), swapInterval(1) {
        currentFrame = new Frame();
        currentFrame->commands.reserve(1024);

        // Keep only enough reusable frames for the bounded in-flight queue.
        for (int i = 0; i < MaxPendingFrames + 1; ++i) {
            Frame* frame = new Frame();
            frame->commands.reserve(1024);
            framePool.push(frame);
        }
    }

    RenderThread::~RenderThread() {
        Stop();
        delete currentFrame;

        Frame* frame = nullptr;
        while (framePool.pop(frame)) delete frame;
    }

    void RenderThread::Start(SDL_Window* initialWindow, SDL_GLContext initialContext, int initialSwapInterval) {
        std::lock_guard<std::mutex> workerLock(workerMutex);
        if (!IsApplicationThread() || workerThread.joinable()) return;

        std::lock_guard<std::mutex> lock(mutex);
        window = initialWindow;
        context = initialContext;
        swapInterval = initialSwapInterval;
        currentFrame->commands.clear();
        currentFrame->ResetRecordedGLState();
        contextError = false;
        contextRequested = true;
        submissionEnabled = true;
        running = true;
        try {
            workerThread = std::thread(&RenderThread::Run, this);
        } catch (const std::system_error& error) {
            running = false;
            contextRequested = false;
            std::fprintf(stderr, "RenderThread::Start: Failed to start render thread: %s\n", error.what());
        }
    }

    bool RenderThread::SetContext(SDL_GLContext newContext) {
        if (!IsApplicationThread()) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!WorkerContextIs(false)) return false;
        context = newContext;
        currentFrame->ResetRecordedGLState();
        return true;
    }

    void RenderThread::Stop() {
        if (IsRenderThread()) return;
        std::lock_guard<std::mutex> workerLock(workerMutex);
        if (!workerThread.joinable()) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
            contextRequested = false;
            Frame* frame = nullptr;
            while (pendingFrames.pop(frame)) {
                frame->commands.clear();
                framePool.push(frame);
                activePendingFrames.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        condition.notify_all();
        workerThread.join();

        {
            std::lock_guard<std::mutex> lock(mutex);
            contextState = ContextState::Released;
            contextError = false;
        }
        condition.notify_all();

        if (IsApplicationThread()) {
            currentFrame->commands.clear();
            currentFrame->ResetRecordedGLState();
        }
    }

    bool RenderThread::PauseAndReleaseContext(int timeoutMS) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS);
        if (!RequestContextRelease(deadline)) return false;

        std::unique_lock<std::mutex> lock(mutex);
        if (WaitForWorkerContext(lock, false, deadline)) return true;

        if (running) contextRequested = true;
        contextError = false;
        condition.notify_all();
        return false;
    }

    bool RenderThread::RequestContextRelease() {
        return RequestContextRelease(std::chrono::steady_clock::now());
    }

    bool RenderThread::RequestContextRelease(Deadline deadline) {
        if (!IsApplicationThread()) return false;
        if (CanAcquireContext()) return true;
        if (!currentFrame->commands.empty() && !SubmitFrame(true, deadline, true).accepted) return false;

        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return WorkerContextIs(false);
        if (contextRequested || contextError) {
            contextError = false;
            contextRequested = false;
            condition.notify_all();
        }
        currentFrame->ResetRecordedGLState();
        return true;
    }

    bool RenderThread::ResumeAndAcquireContext(int timeoutMS) {
        if (!IsApplicationThread()) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS);
        std::unique_lock<std::mutex> lock(mutex);
        if (!running) return false;
        contextError = false;
        contextRequested = true;
        condition.notify_all();
        if (WaitForWorkerContext(lock, true, deadline)) return true;

        contextRequested = false;
        contextError = false;
        condition.notify_all();
        return false;
    }

    bool RenderThread::CanAcquireContext() {
        std::lock_guard<std::mutex> lock(mutex);
        return WorkerContextIs(false);
    }

    void RenderThread::SetSubmissionEnabled(bool enabled) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            submissionEnabled = enabled;
        }
        condition.notify_all();
    }

    bool RenderThread::RecordViewport(int x, int y, int width, int height) {
        if (!IsApplicationThread() || !CanSubmit()) return false;
        if (width < 0 || height < 0) {
            currentFrame->viewportRecorded = false;
            return true;
        }
        const std::array<int, 4> viewport{{x, y, width, height}};
        if (currentFrame->viewportRecorded && currentFrame->viewport == viewport) return false;
        currentFrame->viewport = viewport;
        currentFrame->viewportRecorded = true;
        return true;
    }

    bool RenderThread::RecordActiveTexture(bool textureZero) {
        if (!IsApplicationThread() || !CanSubmit()) return false;
        if (textureZero && currentFrame->textureZeroRecorded) return false;
        currentFrame->textureZeroRecorded = textureZero;
        return true;
    }

    RenderThread::Submission RenderThread::Flip() {
        if (!IsApplicationThread()) return {};
        return SubmitFrame(false);
    }

    RenderThread::Submission RenderThread::SubmitFrame(bool synchronous, Deadline deadline, bool draining) {
#if !defined(HX_WINDOWS) && !defined(HX_ANDROID)
        (void)synchronous;
#endif
        std::unique_lock<std::mutex> lock(mutex);
        if (!CanSubmit(draining)) return {};
        if (currentFrame->commands.empty()) return {true, 0};

        std::uint64_t batch = 0;
#ifdef HX_WINDOWS
        const auto timelineId = FrameTimeline::CurrentWork();
        if (timelineId) batch = FrameTimeline::NextBatch();
#endif
        Frame* nextFrame = nullptr;
        if (!framePool.pop(nextFrame)) {
            FrameTimeline::Span wait("pool_wait", FrameTimeline::CurrentWork());
            wait.SetBatch(batch);
            auto available = [this, &nextFrame, draining] {
                return !CanSubmit(draining) || framePool.pop(nextFrame);
            };
            if (deadline == (Deadline::max)()) {
                condition.wait(lock, available);
            } else if (!condition.wait_until(lock, deadline, available)) {
                return {};
            }
            if (!nextFrame) return {};
        }

        Frame* submitted = currentFrame;
#ifdef HX_WINDOWS
        submitted->timelineId = timelineId;
        submitted->timelineUpdate = FrameTimeline::IsUpdate();
        submitted->synchronization = synchronous;
        submitted->timelineBatch = batch;
        submitted->queueDepth = activePendingFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
        submitted->submittedAt = submitted->timelineId ? FrameTimeline::Now() : 0;
#else
        activePendingFrames.fetch_add(1, std::memory_order_acq_rel);
#endif
#ifdef HX_ANDROID
        submitted->androidCycle = AndroidPerformanceHints::CurrentCycle();
        submitted->androidFinalSubmission = !synchronous;
#endif
        if (!pendingFrames.push(submitted)) {
            activePendingFrames.fetch_sub(1, std::memory_order_acq_rel);
            framePool.push(nextFrame);
            return {};
        }

        currentFrame = nextFrame;
        currentFrame->ResetRecordedGLState();
#ifdef HX_WINDOWS
        currentFrame->mixedTimelinePhase = false;
#endif
        condition.notify_all();
        return {true, batch};
    }

    bool RenderThread::IsRenderThread() const {
        return currentRenderThread == this;
    }

    bool RenderThread::IsApplicationThread() const {
        return std::this_thread::get_id() == applicationThread;
    }

    bool RenderThread::MakeCurrent() {
        if (!IsApplicationThread()) return false;
        if (!CanAcquireContext() || !window || !context) return false;
        if (SDL_GL_MakeCurrent(window, context) != 0) {
            std::fprintf(stderr, "RenderThread::MakeCurrent: Failed to make context current: %s\n", SDL_GetError());
            return false;
        }
        ApplySwapInterval();
        return true;
    }

    bool RenderThread::SetSwapInterval(int interval, bool useWorker) {
        if (!IsApplicationThread() && !IsRenderThread()) return false;
        if (IsContextCurrent()) return SetCurrentSwapInterval(interval);
        if (useWorker && CanSubmit())
            return RunCommandAndWait<bool>("SDL_GL_SetSwapInterval", [this, interval]() { return SetCurrentSwapInterval(interval); });
        return false;
    }

    bool RenderThread::IsContextCurrent() const {
        return window && context && SDL_GL_GetCurrentWindow() == window && SDL_GL_GetCurrentContext() == context;
    }

    bool RenderThread::ApplySwapInterval() {
        return IsContextCurrent() && SetCurrentSwapInterval(ConfiguredSwapInterval());
    }

    bool RenderThread::SetCurrentSwapInterval(int interval) {
        const bool applied = SDL_GL_SetSwapInterval(interval) == 0;
        if (applied) swapInterval = interval;
        currentSwapInterval = SDL_GL_GetSwapInterval();
        swapIntervalRevision.fetch_add(1, std::memory_order_relaxed);
        return applied;
    }

    bool RenderThread::CanSubmit(bool draining) const {
        return running && contextRequested && contextState == ContextState::Held &&
            (draining || submissionEnabled);
    }

    bool RenderThread::WorkerContextIs(bool held) const {
        return held ? running && contextRequested && contextState == ContextState::Held :
            !contextRequested && contextState == ContextState::Released;
    }

    bool RenderThread::WaitForWorkerContext(std::unique_lock<std::mutex>& lock, bool held, Deadline deadline) {
        condition.wait_until(lock, deadline, [this, held] {
            return WorkerContextIs(held) || contextError || !running;
        });
        return WorkerContextIs(held);
    }

    bool RenderThread::WaitForContext(bool held, int timeoutMS) {
        std::unique_lock<std::mutex> lock(mutex);
        return WaitForWorkerContext(lock, held,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS));
    }

    void RenderThread::ChangeWorkerContext(std::unique_lock<std::mutex>& lock, bool acquire) {
        const auto targetContext = acquire ? context : nullptr;
        contextState = acquire ? ContextState::Acquiring : ContextState::Releasing;
        lock.unlock();

        const bool changed = (!acquire || targetContext) && window && SDL_GL_MakeCurrent(window, targetContext) == 0;
        if (!changed) {
            std::fprintf(stderr, "RenderThread::Run: Failed to %s context: %s\n", acquire ? "bind" : "release", SDL_GetError());
        }
        const bool contextHeld = IsContextCurrent();
        if (acquire && contextHeld) ApplySwapInterval();

        lock.lock();
        contextState = contextHeld ? ContextState::Held : ContextState::Released;
        contextError = contextHeld != acquire && contextRequested == acquire;
        condition.notify_all();
    }

    RenderThread::Frame* RenderThread::WaitForFrame() {
        std::unique_lock<std::mutex> lock(mutex);
        while (running) {
            const bool contextHeld = contextState == ContextState::Held;
            Frame* frame = nullptr;
            if (contextHeld && pendingFrames.pop(frame)) return frame;
            if (!contextError && contextHeld != contextRequested) {
                ChangeWorkerContext(lock, !contextHeld);
                continue;
            }
            condition.wait(lock);
        }
        return nullptr;
    }

    void RenderThread::ReturnFrame(Frame* frame) {
        frame->commands.clear();
        std::lock_guard<std::mutex> lock(mutex);
        framePool.push(frame);
        activePendingFrames.fetch_sub(1, std::memory_order_acq_rel);
        condition.notify_all();
    }

    void RenderThread::Run() {
#ifdef HXCPP_TRACY
        tracy::SetThreadName("Render Thread");
#endif

        AndroidPerformanceHints::RegisterWorker(true);
        currentRenderThread = this;
        const char* perfTraceValue = std::getenv("NOVAGC_PERF_TRACE");
        const bool perfTrace = perfTraceValue != nullptr &&
            perfTraceValue[0] != '\0' && perfTraceValue[0] != '0';
        auto perfWindowStarted = perfTrace ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point();
        std::uint64_t perfFrames = 0;
        std::uint64_t perfCommands = 0;
        std::uint64_t perfExecutionMicros = 0;
        std::uint64_t perfMaximumExecutionMicros = 0;

        while (Frame* frame = WaitForFrame()) {
#ifdef HXCPP_TRACY
            ZoneScopedN("Render Loop");
#endif
            const std::size_t commandCount = frame->commands.size();
            const auto executionStarted = perfTrace ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point();
#ifdef HX_WINDOWS
            const auto timelineId = frame->timelineId;
            const auto dequeuedAt = timelineId ? FrameTimeline::Now() : 0;
            if (!frame->timelineUpdate) FrameTimeline::BeginGpu(timelineId);
            const auto driverStarted = timelineId ? FrameTimeline::Now() : 0;
#endif
#ifdef HX_ANDROID
            AndroidPerformanceHints::BeginWorkerBatch(frame->androidCycle);
#endif
            for (auto& command : frame->commands) {
                if (command) command();
            }
#ifdef HX_WINDOWS
            if (timelineId) {
                const auto driverEnded = FrameTimeline::Now();
                const char* detail = frame->synchronization ? "sync" : "submission";
                const char* phase = frame->timelineUpdate ? "update" : "render";
                if (frame->mixedTimelinePhase)
                    phase = frame->timelineUpdate ? "update_mixed" : "render_mixed";
                FrameTimeline::Write({"queue", timelineId, frame->submittedAt, dequeuedAt,
                    static_cast<std::uint64_t>(frame->queueDepth), commandCount, detail, frame->timelineBatch, phase});
                FrameTimeline::Write({"driver_batch", timelineId, driverStarted, driverEnded,
                    commandCount, 0, detail, frame->timelineBatch, phase});
            }
#endif
#ifdef HX_ANDROID
            AndroidPerformanceHints::EndWorkerBatch(frame->androidFinalSubmission);
#endif
            if (perfTrace) {
                const auto now = std::chrono::steady_clock::now();
                const auto executionMicros = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - executionStarted).count());
                ++perfFrames;
                perfCommands += commandCount;
                perfExecutionMicros += executionMicros;
                if (executionMicros > perfMaximumExecutionMicros)
                    perfMaximumExecutionMicros = executionMicros;
                if (now - perfWindowStarted >= std::chrono::seconds(1)) {
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
            ReturnFrame(frame);
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (contextState == ContextState::Held) ChangeWorkerContext(lock, false);
        }
        currentRenderThread = nullptr;
        AndroidPerformanceHints::RegisterWorker(false);
    }

}

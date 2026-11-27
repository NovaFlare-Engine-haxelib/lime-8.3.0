#include <graphics/AndroidPerformanceHints.h>

#ifdef HX_ANDROID
#include <SDL.h>
#include <android/log.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace lime {
namespace AndroidPerformanceHints {
namespace {
    constexpr std::size_t History = 16;
    constexpr std::int64_t NanosPerSecond = 1000000000;
    constexpr unsigned Enabled = 1;
    constexpr unsigned Diagnostic = 2;
    constexpr unsigned RequestFlags = Enabled | Diagnostic;
    constexpr unsigned RequestStep = RequestFlags + 1;
    std::atomic<unsigned> requestedState{0};
    std::atomic<int> workerId{0};
    std::atomic<bool> measuring{false};

    std::int64_t Now() {
        timespec value{};
        if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
        return value.tv_sec * NanosPerSecond + value.tv_nsec;
    }

    int ThreadId() {
        return static_cast<int>(syscall(SYS_gettid));
    }

    struct WorkerFrame {
        std::int64_t swapBegan = 0;
        std::int64_t swapEnded = 0;
        std::uint64_t cycle = 0;
    };

    struct Work {
        std::uint64_t cycle = 0;
        std::int64_t started = 0;
        std::int64_t swapBegan = 0;
        std::int64_t swapEnded = 0;
    };

    struct GameFrame {
        std::int64_t started = 0;
        std::int64_t producerEnded = 0;
        std::int64_t updates = 0;
        std::int64_t target = 0;
        std::uint64_t cycle = 0;
    };

    struct JavaSession {
        JNIEnv* env = nullptr;
        jclass bridge = nullptr;
        jmethodID begin = nullptr;
        jmethodID update = nullptr;
        jmethodID close = nullptr;

        bool Load();
        bool Start(int worker, std::int64_t target);
        bool Report(std::int64_t work, std::int64_t target);
        void Stop();
        bool Failed();
    };

    bool JavaSession::Failed() {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        __android_log_write(ANDROID_LOG_WARN, "NovaFlareADPF", "JNI failure");
        return true;
    }

    bool JavaSession::Load() {
        env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return false;
        if (bridge) return begin && update && close;
        jclass local = env->FindClass("org/haxe/lime/FramePerformanceHints");
        if (Failed() || !local) return false;
        bridge = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (Failed() || !bridge) return false;
        begin = env->GetStaticMethodID(bridge, "start", "(Landroid/content/Context;IIJ)Z");
        if (!Failed() && begin) update = env->GetStaticMethodID(bridge, "report", "(JJ)Z");
        if (!Failed() && update) close = env->GetStaticMethodID(bridge, "stop", "()V");
        if (!Failed() && begin && update && close) return true;
        env->DeleteGlobalRef(bridge);
        bridge = nullptr;
        begin = update = close = nullptr;
        return false;
    }

    bool JavaSession::Start(int worker, std::int64_t target) {
        if (!Load()) return false;
        jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (Failed()) {
            if (activity) env->DeleteLocalRef(activity);
            return false;
        }
        if (!activity) return false;
        const jboolean opened = env->CallStaticBooleanMethod(bridge, begin, activity,
            static_cast<jint>(ThreadId()), static_cast<jint>(worker), static_cast<jlong>(target));
        env->DeleteLocalRef(activity);
        return !Failed() && opened;
    }

    bool JavaSession::Report(std::int64_t work, std::int64_t target) {
        const jboolean reported = env->CallStaticBooleanMethod(bridge, update,
            static_cast<jlong>(work), static_cast<jlong>(target));
        return !Failed() && reported;
    }

    void JavaSession::Stop() {
        env->CallStaticVoidMethod(bridge, close);
        Failed();
    }

    class FrameSession {
    public:
        void Begin(double periodMs, bool threaded);
        void Finish();
        void Foreground(bool foreground);

        Work work;
        std::int64_t startedUpdate = 0;
        std::int64_t pendingUpdate = 0;

    private:
        bool ApplyRequest();
        void Send(const GameFrame& frame, std::int64_t swapBegan, std::int64_t priorSwapIdle = 0);
        void Stop();
        void ReadWorkerFrames();

        JavaSession java;
        std::array<GameFrame, History> gameFrames{};
        std::uint64_t cycle = 0;
        std::uint64_t consumed = 0;
        std::int64_t target = 0;
        unsigned request = 0;
        int sessionWorker = 0;
        int refresh = 60;
        bool foreground = true;
        bool attemptFailed = false;
        bool running = false;
    };

    FrameSession game;
    std::array<WorkerFrame, History> workerFrames;
    std::atomic<std::uint64_t> latestWorkerCycle{0};
    std::mutex workerFramesMutex;
    thread_local Work worker;
    thread_local bool workerThread = false;

    bool FrameSession::ApplyRequest() {
        const unsigned next = requestedState.load(std::memory_order_acquire);
        if (next == request) return false;
        Stop();
        request = next;
        attemptFailed = false;
        const char* message = (request & Diagnostic)
            ? ((request & Enabled) ? "enabled by diagnostic setting" : "disabled by diagnostic setting")
            : ((request & Enabled) ? "enabled by preference" : "disabled by preference");
        __android_log_write(ANDROID_LOG_INFO, "NovaFlareADPF", message);
        return true;
    }

    void FrameSession::Begin(double periodMs, bool threaded) {
        ApplyRequest();
        if (!(request & Enabled) || !foreground || attemptFailed) return;
        const int currentWorker = threaded ? workerId.load(std::memory_order_acquire) : 0;
        if (threaded && !currentWorker) return;
        if (running && currentWorker != sessionWorker) Stop();
        if (!running || cycle % 120 == 0) {
            SDL_DisplayMode mode{};
            if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0)
                refresh = mode.refresh_rate;
        }

        const auto displayPeriod = NanosPerSecond / refresh;
        const auto capPeriod = static_cast<std::int64_t>(periodMs * 1000000.0);
        target = std::max(displayPeriod, capPeriod);
        if (!running) {
            running = java.Start(currentWorker, target);
            if (!running) {
                attemptFailed = true;
                return;
            }
            sessionWorker = currentWorker;
            measuring.store(true, std::memory_order_release);
        }

        work = {};
        work.cycle = ++cycle;
        work.started = Now();
    }

    void FrameSession::Stop() {
        if (!running) return;
        measuring.store(false, std::memory_order_release);
        java.Stop();
        running = false;
        work = {};
        startedUpdate = 0;
        pendingUpdate = 0;
        consumed = cycle;
    }

    void FrameSession::Foreground(bool visible) {
        if (visible && !foreground) attemptFailed = false;
        foreground = visible;
        if (!visible) Stop();
    }

    void FrameSession::Send(const GameFrame& frame, std::int64_t swapBegan, std::int64_t priorSwapIdle) {
        if (!frame.started || swapBegan < frame.started) return;
        const auto duration = frame.updates + swapBegan - frame.started - priorSwapIdle;
        if (duration > 0 && !java.Report(duration, frame.target)) {
            Stop();
            attemptFailed = true;
        }
    }

    void FrameSession::ReadWorkerFrames() {
        const auto completed = latestWorkerCycle.load(std::memory_order_acquire);
        if (completed <= consumed) return;
        const auto earliest = completed >= History ? completed - History + 1 : 1;
        for (auto sample = std::max(consumed + 1, earliest); running && sample <= completed; ++sample) {
            consumed = sample;
            const auto& frame = gameFrames[sample % History];
            if (frame.cycle != sample) continue;
            WorkerFrame rendered;
            std::int64_t priorSwapIdle = 0;
            {
                std::lock_guard<std::mutex> lock(workerFramesMutex);
                rendered = workerFrames[sample % History];
                if (rendered.cycle != sample) continue;
                // Deduct a preceding swap only when it blocked the worker
                // after this frame's producer had finished submitting work.
                for (const auto& previous : workerFrames) {
                    if (!previous.cycle || previous.cycle >= sample) continue;
                    const auto start = std::max(previous.swapBegan, frame.producerEnded);
                    const auto end = std::min(previous.swapEnded, rendered.swapBegan);
                    if (end > start) priorSwapIdle += end - start;
                }
            }
            Send(frame, rendered.swapBegan, priorSwapIdle);
        }
    }

    void FrameSession::Finish() {
        if (!work.cycle || ApplyRequest()) return;
        GameFrame frame;
        frame.started = work.started;
        frame.producerEnded = Now();
        frame.updates = pendingUpdate;
        frame.target = target;
        frame.cycle = work.cycle;
        if (sessionWorker) {
            gameFrames[work.cycle % History] = frame;
            ReadWorkerFrames();
        } else if (work.swapBegan) {
            Send(frame, work.swapBegan);
        }
        work = {};
        pendingUpdate = 0;
    }
}

static void SetEnabled(bool enabled, bool diagnostic) {
    const unsigned flags = (enabled ? Enabled : 0) | (diagnostic ? Diagnostic : 0);
    auto current = requestedState.load(std::memory_order_relaxed);
    for (;;) {
        if (!diagnostic && (current & Diagnostic)) return;
        if ((current & RequestFlags) == flags) return;
        // Preserve an off/on change even when both requests land before a frame.
        const auto next = ((current & ~RequestFlags) + RequestStep) | flags;
        if (requestedState.compare_exchange_weak(current, next, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

void BeginUpdate() {
    if (measuring.load(std::memory_order_relaxed)) game.startedUpdate = Now();
}

void EndUpdate() {
    if (!game.startedUpdate) return;
    game.pendingUpdate += std::max<std::int64_t>(0, Now() - game.startedUpdate);
    game.startedUpdate = 0;
}

void BeginRender(double periodMs, bool threaded) { game.Begin(periodMs, threaded); }
void EndRender() { game.Finish(); }
std::uint64_t CurrentCycle() { return game.work.cycle; }
void SetForeground(bool foreground) { game.Foreground(foreground); }

void RegisterWorker(bool running) {
    workerThread = running;
    workerId.store(running ? ThreadId() : 0, std::memory_order_release);
}

void BeginWorkerBatch(std::uint64_t cycle) {
    worker = {};
    if (cycle && measuring.load(std::memory_order_relaxed)) worker.cycle = cycle;
}

void EndWorkerBatch(bool finalSubmission) {
    if (!worker.cycle || !finalSubmission || !worker.swapBegan || !measuring.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(workerFramesMutex);
        auto& rendered = workerFrames[worker.cycle % History];
        rendered.swapBegan = worker.swapBegan;
        rendered.swapEnded = worker.swapEnded;
        rendered.cycle = worker.cycle;
    }
    latestWorkerCycle.store(worker.cycle, std::memory_order_release);
}

void BeforeSwap() {
    auto& work = workerThread ? worker : game.work;
    if (work.cycle && measuring.load(std::memory_order_relaxed)) work.swapBegan = Now();
}

void AfterSwap() {
    if (workerThread && worker.swapBegan && measuring.load(std::memory_order_relaxed)) worker.swapEnded = Now();
}
}
}

extern "C" JNIEXPORT void JNICALL Java_org_haxe_lime_FramePerformanceHints_nativeSetEnabled(
    JNIEnv*, jclass, jboolean enabled, jboolean diagnostic) {
    lime::AndroidPerformanceHints::SetEnabled(enabled == JNI_TRUE, diagnostic == JNI_TRUE);
}
#endif

#include <graphics/FrameTimeline.h>

#ifdef HX_WINDOWS
#include <SDL.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace lime {
namespace FrameTimeline {
namespace {

    using TraceFile = std::unique_ptr<FILE, decltype(&std::fclose)>;

    class TraceWriter {
    public:
        explicit TraceWriter(TraceFile file) : file(std::move(file)) {
            pending.reserve(MaxPending);
            writing.reserve(MaxPending);
        }

        void Enqueue(const char* bytes, std::size_t size) {
            try {
                std::string chunk(bytes, size);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping) return;
                    if (pending.size() >= MaxPending) {
                        ++dropped;
                        return;
                    }
                    pending.push_back(std::move(chunk));
                }
                ready.notify_one();
            } catch (const std::bad_alloc&) {
                ++dropped;
            }
        }

        void Finish() {
            std::unique_lock<std::mutex> lock(mutex);
            stopping = true;
            ready.notify_one();
            finished.wait(lock, [this] { return closed; });
        }

        void Run() {
            std::uint64_t writeFailures = 0;
            while (TakePending()) {
                for (const auto& chunk : writing) {
                    if (std::fwrite(chunk.data(), 1, chunk.size(), file.get()) != chunk.size())
                        ++writeFailures;
                }
                writing.clear();
            }
            if (std::fflush(file.get()) != 0) ++writeFailures;
            const auto tick = SDL_GetPerformanceCounter();
            std::fprintf(file.get(), "trace_end,0,%llu,%llu,%llu,%llu,async,0,\n",
                static_cast<unsigned long long>(tick), static_cast<unsigned long long>(tick),
                static_cast<unsigned long long>(dropped.load()), static_cast<unsigned long long>(writeFailures));
            file.reset();
            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
            }
            finished.notify_all();
        }

    private:
        bool TakePending() {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [this] { return stopping || !pending.empty(); });
            if (pending.empty()) return false;
            pending.swap(writing);
            return true;
        }

        static constexpr std::size_t MaxPending = 512;
        TraceFile file;
        std::mutex mutex;
        std::condition_variable ready;
        std::condition_variable finished;
        std::vector<std::string> pending;
        std::vector<std::string> writing;
        std::atomic<std::uint64_t> dropped{0};
        bool stopping = false;
        bool closed = false;
    };

    std::shared_ptr<TraceWriter> OpenTimeline() {
        wchar_t path[32768];
        DWORD length = GetEnvironmentVariableW(L"NOVAFLARE_FRAME_TIMELINE", path, 32768);
        if (!length || length >= 32768) return nullptr;
        TraceFile file(_wfopen(path, L"wb"), std::fclose);
        if (!file) return nullptr;
        std::fputs("event,frame,begin_qpc,end_qpc,value,auxiliary,detail,batch,phase\n", file.get());
        std::fprintf(file.get(), "clock,0,0,0,%llu,0,qpc_frequency,0,\n",
            static_cast<unsigned long long>(SDL_GetPerformanceFrequency()));
        std::fputs("trace_writer,0,0,0,1,0,async,0,\n", file.get());
        try {
            auto writer = std::make_shared<TraceWriter>(std::move(file));
            // The writer owns the file until normal application shutdown drains it.
            std::thread([writer] { writer->Run(); }).detach();
            return writer;
        } catch (const std::system_error&) {
            return nullptr;
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }

    TraceWriter* Timeline() {
        static const auto writer = OpenTimeline();
        return writer.get();
    }

    class TraceBuffer {
    public:
        ~TraceBuffer() { Flush(); }

        void Append(const Event& event, TraceWriter* writer) {
            output = writer;
            if (bytes.size() - size < 512) Flush();
            const int count = std::snprintf(bytes.data() + size, bytes.size() - size,
                "%s,%llu,%llu,%llu,%llu,%llu,%s,%llu,%s\n", event.name,
                static_cast<unsigned long long>(event.frame),
                static_cast<unsigned long long>(event.begin),
                static_cast<unsigned long long>(event.end),
                static_cast<unsigned long long>(event.value),
                static_cast<unsigned long long>(event.auxiliary), event.detail ? event.detail : "",
                static_cast<unsigned long long>(event.batch), event.phase ? event.phase : "");
            if (count > 0 && static_cast<std::size_t>(count) < bytes.size() - size)
                size += static_cast<std::size_t>(count);
        }

        void Flush() {
            if (!size) return;
            output->Enqueue(bytes.data(), size);
            size = 0;
        }

    private:
        TraceWriter* output = nullptr;
        std::array<char, 16384> bytes{};
        std::size_t size = 0;
    };

    thread_local TraceBuffer traceBuffer;

    struct DispatchState {
        std::uint64_t update = 0;
        bool updating = false;
        std::uint64_t sequence = 0;
        std::uint64_t batchSequence = 0;
        std::uint64_t render = 0;
        std::uint64_t renderStart = 0;
    };

    thread_local DispatchState dispatch;

    class GpuQueries {
    public:
        void Begin(std::uint64_t frame);
        void End(std::uint64_t frame);

    private:
        struct Query {
            unsigned int begin = 0;
            unsigned int end = 0;
            std::uint64_t frame = 0;
            bool pending = false;
        };

        bool Load();
        void ReadAvailable();

        // SDL returns OpenGL entry points with the platform's GL calling convention.
        using CreateQueries = void (APIENTRY*)(int, unsigned int*);
        using QueryCounter = void (APIENTRY*)(unsigned int, unsigned int);
        using QueryParameter = void (APIENTRY*)(unsigned int, unsigned int, int*);
        using QueryResult = void (APIENTRY*)(unsigned int, unsigned int, std::uint64_t*);
        static constexpr unsigned int Timestamp = 0x8E28;
        static constexpr unsigned int CounterBits = 0x8864;
        static constexpr unsigned int Result = 0x8866;
        static constexpr unsigned int ResultAvailable = 0x8867;

        SDL_GLContext context = nullptr;
        std::array<Query, 16> queries{};
        Query* active = nullptr;
        std::uint64_t currentFrame = 0;
        QueryCounter counter = nullptr;
        QueryParameter queryParameter = nullptr;
        QueryResult queryResult = nullptr;
    };

    bool GpuQueries::Load() {
        auto profile = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
        if (profile & SDL_GL_CONTEXT_PROFILE_ES) return false;
        int major = 0;
        int minor = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        if (major < 4 && (major < 3 || minor < 3) &&
            !SDL_GL_ExtensionSupported("GL_ARB_timer_query")) return false;

        const auto create = reinterpret_cast<CreateQueries>(SDL_GL_GetProcAddress("glGenQueries"));
        const auto getParameter = reinterpret_cast<QueryParameter>(SDL_GL_GetProcAddress("glGetQueryiv"));
        counter = reinterpret_cast<QueryCounter>(SDL_GL_GetProcAddress("glQueryCounter"));
        queryParameter = reinterpret_cast<QueryParameter>(SDL_GL_GetProcAddress("glGetQueryObjectiv"));
        queryResult = reinterpret_cast<QueryResult>(SDL_GL_GetProcAddress("glGetQueryObjectui64v"));
        if (!create || !getParameter || !counter || !queryParameter || !queryResult)
            return false;

        int counterBits = 0;
        getParameter(Timestamp, CounterBits, &counterBits);
        if (counterBits <= 0) return false;
        std::array<unsigned int, 32> ids{};
        create(static_cast<int>(ids.size()), ids.data());
        for (std::size_t index = 0; index < queries.size(); ++index) {
            queries[index].begin = ids[index * 2];
            queries[index].end = ids[index * 2 + 1];
        }
        return queries.front().begin != 0;
    }

    void GpuQueries::ReadAvailable() {
        for (Query& query : queries) {
            if (!query.pending) continue;
            int available = 0;
            queryParameter(query.end, ResultAvailable, &available);
            if (!available) continue;
            std::uint64_t begin = 0;
            std::uint64_t end = 0;
            queryResult(query.begin, Result, &begin);
            queryResult(query.end, Result, &end);
            if (end >= begin) {
                const auto read = Now();
                Write({"gpu_elapsed", query.frame, read, read, end - begin});
            }
            query.pending = false;
        }
    }

    void GpuQueries::Begin(std::uint64_t frame) {
        if (frame == currentFrame) return;
        currentFrame = frame;
        SDL_GLContext currentContext = SDL_GL_GetCurrentContext();
        if (context != currentContext) {
            context = currentContext;
            active = nullptr;
            queries = {};
            const bool supported = context && Load();
            const auto now = Now();
            Write({"gpu_support", frame, now, now, supported ? 1u : 0u});
        }
        if (!queries.front().begin) return;

        active = nullptr;
        ReadAvailable();
        for (Query& query : queries) {
            if (!query.pending && query.begin && query.end) {
                active = &query;
                counter(query.begin, Timestamp);
                return;
            }
        }
        const auto now = Now();
        Write({"gpu_query_backlog", frame, now, now});
    }

    void GpuQueries::End(std::uint64_t frame) {
        if (frame != currentFrame || !active) return;
        counter(active->end, Timestamp);
        active->frame = frame;
        active->pending = true;
        active = nullptr;
    }

    thread_local GpuQueries gpu;
}

std::uint64_t Now() {
    return Timeline() ? SDL_GetPerformanceCounter() : 0;
}

void Write(const Event& event) {
    if (auto* writer = Timeline()) traceBuffer.Append(event, writer);
}

void Flush() {
    traceBuffer.Flush();
    if (auto* writer = Timeline()) writer->Finish();
}

std::uint64_t CurrentRender() { return dispatch.render; }

std::uint64_t CurrentUpdate() { return dispatch.update; }

void BeginUpdate() {
    if (!Timeline()) return;
    ++dispatch.update;
    dispatch.updating = true;
}

void EndUpdate() { dispatch.updating = false; }

std::uint64_t CurrentWork() {
    return dispatch.updating ? dispatch.update : dispatch.render;
}

std::uint64_t NextBatch() { return ++dispatch.batchSequence; }

bool IsUpdate() { return dispatch.updating; }

const char* SyncEvent() {
    return dispatch.updating ? "update_gl_sync" : "gl_sync";
}

void BeginRender() {
    if (!Timeline()) return;
    dispatch.render = ++dispatch.sequence;
    dispatch.renderStart = Now();
}

void EndCommands(bool threaded) {
    if (!dispatch.render) return;
    Write({"render_commands", dispatch.render, dispatch.renderStart, Now(),
        dispatch.update, threaded ? 1u : 0u});
}

void EndRender() {
    if (!dispatch.render) return;
    Write({"render_dispatch", dispatch.render, dispatch.renderStart, Now(), dispatch.update});
    dispatch.render = 0;
}

void RenderBlocked(int depth) {
    const auto now = Now();
    if (now) Write({"queue_full", 0, now, now, static_cast<std::uint64_t>(depth), dispatch.update});
}

void BeginGpu(std::uint64_t frame) {
    if (frame) gpu.Begin(frame);
}

void EndGpu(std::uint64_t frame) {
    if (frame) gpu.End(frame);
}

void WindowGeometry(SDL_Window* window, std::uint64_t frame) {
    const auto tick = Now();
    if (!tick || !window) return;

    int width = 0;
    int height = 0;
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    const char* mode = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN ? "fullscreen" : "windowed";
    if (width > 0 && height > 0)
        Write({"window_size", frame, tick, tick, static_cast<std::uint64_t>(width),
            static_cast<std::uint64_t>(height), mode});
    if (drawableWidth > 0 && drawableHeight > 0)
        Write({"drawable_size", frame, tick, tick, static_cast<std::uint64_t>(drawableWidth),
            static_cast<std::uint64_t>(drawableHeight), mode});

    const int display = SDL_GetWindowDisplayIndex(window);
    float horizontal = 0;
    float vertical = 0;
    if (display >= 0 && SDL_GetDisplayDPI(display, nullptr, &horizontal, &vertical) == 0 &&
        horizontal > 0 && vertical > 0)
        Write({"display_dpi_milli", frame, tick, tick, static_cast<std::uint64_t>(horizontal * 1000.0 + .5),
            static_cast<std::uint64_t>(vertical * 1000.0 + .5), mode});
}
}
}
#endif

#ifndef LIME_GRAPHICS_FRAME_TIMELINE_H
#define LIME_GRAPHICS_FRAME_TIMELINE_H

#include <cstdint>

struct SDL_Window;

namespace lime {
namespace FrameTimeline {

    struct Event {
        const char* name;
        std::uint64_t frame;
        std::uint64_t begin;
        std::uint64_t end;
        std::uint64_t value;
        std::uint64_t auxiliary;
        const char* detail;
        std::uint64_t batch;
        const char* phase;
    };

#ifdef HX_WINDOWS
    std::uint64_t Now();
    void Write(const Event& event);
    std::uint64_t CurrentRender();
    void BeginUpdate();
    void EndUpdate();
    std::uint64_t CurrentWork();
    std::uint64_t NextBatch();
    bool IsUpdate();
    const char* SyncEvent();
    void Flush();
    void BeginRender();
    void EndCommands(bool threaded);
    void EndRender();
    void RenderBlocked(int depth);
    void BeginGpu(std::uint64_t frame);
    void EndGpu(std::uint64_t frame);
    void WindowGeometry(SDL_Window* window, std::uint64_t frame = 0);
#else
    inline std::uint64_t Now() { return 0; }
    inline void Write(const Event&) {}
    inline std::uint64_t CurrentRender() { return 0; }
    inline void BeginUpdate() {}
    inline void EndUpdate() {}
    inline std::uint64_t CurrentWork() { return 0; }
    inline std::uint64_t NextBatch() { return 0; }
    inline bool IsUpdate() { return false; }
    inline const char* SyncEvent() { return ""; }
    inline void Flush() {}
    inline void BeginRender() {}
    inline void EndCommands(bool) {}
    inline void EndRender() {}
    inline void RenderBlocked(int) {}
    inline void BeginGpu(std::uint64_t) {}
    inline void EndGpu(std::uint64_t) {}
    inline void WindowGeometry(SDL_Window*, std::uint64_t = 0) {}
#endif

    class Span {
    public:
        explicit Span(const char* name, const char* detail = "")
            : Span(name, CurrentRender(), detail) {}
        Span(const char* name, std::uint64_t frame, const char* detail = "")
            : event{name, frame, frame ? Now() : 0, 0, 0, 0, detail, 0, IsUpdate() ? "update" : "render"} {}
        void SetBatch(std::uint64_t batch) { event.batch = batch; }
        ~Span() {
            if (!event.begin) return;
            event.end = Now();
            Write(event);
        }

    private:
        Event event;
    };

#ifdef HX_WINDOWS
    std::uint64_t CurrentUpdate();
#else
    inline std::uint64_t CurrentUpdate() { return 0; }
#endif
}
}

#endif

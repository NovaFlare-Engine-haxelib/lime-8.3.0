#ifndef LIME_ANDROID_PERFORMANCE_HINTS_H
#define LIME_ANDROID_PERFORMANCE_HINTS_H

#include <cstdint>

namespace lime {
namespace AndroidPerformanceHints {
#ifdef HX_ANDROID
    void BeginUpdate();
    void EndUpdate();
    void BeginRender(double periodMs, bool threaded);
    void EndRender();
    std::uint64_t CurrentCycle();
    void BeforeSwap();
    void AfterSwap();
    void RegisterWorker(bool running);
    void BeginWorkerBatch(std::uint64_t cycle);
    void EndWorkerBatch(bool finalSubmission);
    void SetForeground(bool foreground);
#else
    inline void BeginUpdate() {}
    inline void EndUpdate() {}
    inline void BeginRender(double, bool) {}
    inline void EndRender() {}
    inline std::uint64_t CurrentCycle() { return 0; }
    inline void BeforeSwap() {}
    inline void AfterSwap() {}
    inline void RegisterWorker(bool) {}
    inline void BeginWorkerBatch(std::uint64_t) {}
    inline void EndWorkerBatch(bool) {}
    inline void SetForeground(bool) {}
#endif
}
}

#endif

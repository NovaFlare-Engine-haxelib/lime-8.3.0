#ifndef LIME_GRAPHICS_PERFORMANCE_MONITOR_H
#define LIME_GRAPHICS_PERFORMANCE_MONITOR_H

#include <atomic>
#include <chrono>

namespace lime {

    class PerformanceMonitor {
    public:
        static PerformanceMonitor& Instance();

        void StartFrame();
        void EndFrame();
        
        void RecordDrawCall();
        void RecordBufferSwap();
        
        double GetAverageFrameTime() const;
        int GetFPS() const;
        int GetDrawCalls() const;

    private:
        PerformanceMonitor();
        
        std::atomic<int> drawCalls;
        std::atomic<int> frameCount;
        
        std::chrono::high_resolution_clock::time_point startTime;
        std::chrono::high_resolution_clock::time_point lastFrameTime;
        
        double accumulatedFrameTime;
        double averageFrameTime;
        int fps;
    };

}

#endif

#include <graphics/PerformanceMonitor.h>

namespace lime {

    PerformanceMonitor& PerformanceMonitor::Instance() {
        static PerformanceMonitor instance;
        return instance;
    }

    PerformanceMonitor::PerformanceMonitor() 
        : drawCalls(0), frameCount(0), accumulatedFrameTime(0.0), averageFrameTime(0.0), fps(0) {
        startTime = std::chrono::high_resolution_clock::now();
        lastFrameTime = startTime;
    }

    void PerformanceMonitor::StartFrame() {
        drawCalls = 0;
    }

    void PerformanceMonitor::EndFrame() {
        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> frameDuration = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        accumulatedFrameTime += frameDuration.count();
        frameCount++;

        if (accumulatedFrameTime >= 1000.0) {
            fps = frameCount;
            averageFrameTime = accumulatedFrameTime / frameCount;
            
            accumulatedFrameTime = 0.0;
            frameCount = 0;
        }
    }

    void PerformanceMonitor::RecordDrawCall() {
        drawCalls++;
    }

    void PerformanceMonitor::RecordBufferSwap() {
        // Can be used to measure swap latency if needed
    }

    double PerformanceMonitor::GetAverageFrameTime() const {
        return averageFrameTime;
    }

    int PerformanceMonitor::GetFPS() const {
        return fps;
    }

    int PerformanceMonitor::GetDrawCalls() const {
        return drawCalls.load();
    }

}

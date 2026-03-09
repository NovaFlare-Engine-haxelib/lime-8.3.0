#include <system/BackendThread.h>
#include <stdio.h>
#include <chrono>

#ifdef HXCPP_TRACY
#include <hx/TelemetryTracy.h>
#endif

namespace lime {

    BackendThread* BackendThread::instance = nullptr;

    BackendThread::BackendThread() : workerThread(nullptr), running(false) {
        
    }

    BackendThread::~BackendThread() {
        Stop();
        if (instance == this) {
            instance = nullptr;
        }
    }

    BackendThread* BackendThread::GetInstance() {
        if (!instance) {
            instance = new BackendThread();
        }
        return instance;
    }

    void BackendThread::Start() {
        if (running) return;
        
        running = true;
        workerThread = new std::thread(&BackendThread::Run, this);
    }

    void BackendThread::Stop() {
        if (!running) return;
        
        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
        }
        condition.notify_all();
        
        if (workerThread) {
            if (workerThread->joinable()) {
                workerThread->join();
            }
            delete workerThread;
            workerThread = nullptr;
        }
    }

    void BackendThread::Push(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            taskQueue.push_back(task);
        }
        condition.notify_one();
    }

    void BackendThread::Run() {
        #ifdef HXCPP_TRACY
        tracy::SetThreadName("Backend Thread");
        #endif

        while (running) {
            #ifdef HXCPP_TRACY
            ZoneScopedN("Backend Loop");
            #endif

            std::function<void()> task;
            
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    return !taskQueue.empty() || !running;
                });
                
                if (!running && taskQueue.empty()) {
                    break;
                }
                
                if (!taskQueue.empty()) {
                    task = taskQueue.front();
                    taskQueue.pop_front();
                }
            }
            
            if (task) {
                task();
            }
        }
    }

}

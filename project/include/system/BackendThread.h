#ifndef LIME_SYSTEM_BACKEND_THREAD_H
#define LIME_SYSTEM_BACKEND_THREAD_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>

namespace lime {

    class BackendThread {
    
    public:
        
        BackendThread();
        ~BackendThread();
        
        static BackendThread* GetInstance();
        
        void Start();
        void Stop();
        void Push(std::function<void()> task);
        
    private:
        
        void Run();
        
        static BackendThread* instance;
        std::deque<std::function<void()>> taskQueue;
        std::mutex mutex;
        std::condition_variable condition;
        std::thread* workerThread;
        bool running;
        
    };

}

#endif

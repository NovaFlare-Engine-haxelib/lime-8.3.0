#ifndef LIME_GRAPHICS_SHARED_BUFFER_MANAGER_H
#define LIME_GRAPHICS_SHARED_BUFFER_MANAGER_H

#include <vector>
#include <mutex>
#include <memory>
#include <map>
#include <atomic>

namespace lime {

    // Represents a chunk of memory that can be shared between threads
    struct SharedBuffer {
        unsigned char* data;
        size_t size;
        size_t capacity;
        std::atomic<int> refCount;
        
        SharedBuffer(size_t cap);
        ~SharedBuffer();
        
        void AddRef();
        void Release();
    };

    class SharedBufferManager {
    public:
        static SharedBufferManager& Instance();

        // Acquire a buffer from the pool or create a new one
        SharedBuffer* AcquireBuffer(size_t minSize);
        
        // Internal use: return buffer to pool when refCount drops to 0
        void RecycleBuffer(SharedBuffer* buffer);

    private:
        SharedBufferManager();
        ~SharedBufferManager();
        
        std::mutex mutex;
        std::vector<SharedBuffer*> bufferPool;
        
        // Prevent copying
        SharedBufferManager(const SharedBufferManager&) = delete;
        SharedBufferManager& operator=(const SharedBufferManager&) = delete;
    };

}

#endif

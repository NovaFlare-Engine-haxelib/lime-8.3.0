#include <graphics/SharedBufferManager.h>
#include <algorithm>

namespace lime {

    SharedBuffer::SharedBuffer(size_t cap) : size(0), capacity(cap), refCount(0) {
        data = new unsigned char[capacity];
    }

    SharedBuffer::~SharedBuffer() {
        if (data) {
            delete[] data;
            data = nullptr;
        }
    }

    void SharedBuffer::AddRef() {
        refCount++;
    }

    void SharedBuffer::Release() {
        if (--refCount == 0) {
            SharedBufferManager::Instance().RecycleBuffer(this);
        }
    }

    // ------------------------------------------------------------------------

    SharedBufferManager::SharedBufferManager() {}

    SharedBufferManager::~SharedBufferManager() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto buffer : bufferPool) {
            delete buffer;
        }
        bufferPool.clear();
    }

    SharedBufferManager& SharedBufferManager::Instance() {
        static SharedBufferManager instance;
        return instance;
    }

    SharedBuffer* SharedBufferManager::AcquireBuffer(size_t minSize) {
        std::lock_guard<std::mutex> lock(mutex);
        
        // Find a suitable buffer in the pool
        for (auto it = bufferPool.begin(); it != bufferPool.end(); ++it) {
            if ((*it)->capacity >= minSize) {
                SharedBuffer* buffer = *it;
                bufferPool.erase(it);
                buffer->size = 0; // Reset size
                buffer->AddRef(); // Initial ref
                return buffer;
            }
        }
        
        // No suitable buffer found, create new one
        SharedBuffer* newBuffer = new SharedBuffer(minSize > 1024 ? minSize : 1024); // Minimum 1KB
        newBuffer->AddRef();
        return newBuffer;
    }

    void SharedBufferManager::RecycleBuffer(SharedBuffer* buffer) {
        std::lock_guard<std::mutex> lock(mutex);
        bufferPool.push_back(buffer);
    }

}

#ifndef LIME_GRAPHICS_DOUBLE_BUFFER_H
#define LIME_GRAPHICS_DOUBLE_BUFFER_H

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <utility>

namespace lime {

    template <typename T>
    class DoubleBuffer {
    public:
        DoubleBuffer() {
            readBuffer = new T();
            writeBuffer = new T();
            isNewDataAvailable = false;
        }

        ~DoubleBuffer() {
            delete readBuffer;
            delete writeBuffer;
        }

        // Get the buffer for writing (Main Thread)
        // The caller is responsible for resetting/clearing the buffer if needed
        T* LockWrite() {
            return writeBuffer;
        }

        // Submit the write buffer (Main Thread)
        // This makes the current write buffer the new read buffer
        void UnlockWrite() {
            std::lock_guard<std::mutex> lock(mutex);
            std::swap(readBuffer, writeBuffer);
            isNewDataAvailable = true;
            condition.notify_one();
        }

        // Check if there is a new frame available (Render Thread)
        bool HasNewData() {
            return isNewDataAvailable;
        }

        // Get the buffer for reading (Render Thread)
        // Returns the current read buffer.
        T* LockRead() {
            std::lock_guard<std::mutex> lock(mutex);
            return readBuffer;
        }
        
        // Signal that reading is finished (Render Thread)
        void UnlockRead() {
            // Optional: Logic to signal back to main thread if we need strict sync
        }

        // Blocking wait for new data (Render Thread)
        T* WaitAndLockRead() {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] { return isNewDataAvailable.load(); });
            // Once we consume it, we can flag it as read, 
            // but for a continuous render loop, we might just want to read whatever is there.
            // If we want to ensure we process each frame exactly once, we'd set isNewDataAvailable = false here.
            isNewDataAvailable = false;
            return readBuffer;
        }

    private:
        T* readBuffer;
        T* writeBuffer;
        
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> isNewDataAvailable;
    };

}

#endif

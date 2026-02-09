#ifndef LIME_GRAPHICS_READ_ONLY_VIEW_H
#define LIME_GRAPHICS_READ_ONLY_VIEW_H

#include <graphics/ImageBuffer.h>
#include <utils/Bytes.h>

namespace lime {

    // Interface for Read-Only GPU Resources
    class IReadOnlyView {
    public:
        virtual ~IReadOnlyView() {}
    };

    // Read-Only View for Texture Data
    class ReadOnlyTextureView : public IReadOnlyView {
    public:
        ReadOnlyTextureView(ImageBuffer* imageBuffer) {
            this->width = imageBuffer->width;
            this->height = imageBuffer->height;
            this->bitsPerPixel = imageBuffer->bitsPerPixel;
            // Note: We might want to copy data or keep a shared reference
            // For now, let's assume we copy the metadata and hold a reference to the data if safe
        }
        
        int GetWidth() const { return width; }
        int GetHeight() const { return height; }
        int GetBitsPerPixel() const { return bitsPerPixel; }

    private:
        int width;
        int height;
        int bitsPerPixel;
    };

    // Read-Only View for Vertex Buffers
    class ReadOnlyBufferView : public IReadOnlyView {
    public:
        ReadOnlyBufferView(unsigned char* data, int length) : data(data), length(length) {}
        
        const unsigned char* GetData() const { return data; }
        int GetLength() const { return length; }

    private:
        unsigned char* data;
        int length;
    };

}

#endif

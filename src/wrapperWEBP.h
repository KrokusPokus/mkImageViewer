#ifndef WEBPWRAPPER_H
#define WEBPWRAPPER_H

#include <cstddef>

class WebpReaderWriter
{
public:
    // Returns data in 4 byte BGRA
    static void* ReadImage(int& width,   // width of the image loaded.
        int& height,  // height of the image loaded.
        int& bpp,     // BYTES (not bits) PER PIXEL.
        bool& has_animation,     // if the image is animated
        int& frame_count, // number of frames
        int& frame_time, // frame duration in milliseconds
        void*& exif, // Pointer to Exif data (must be freed by caller)
        bool& outOfMemory, // set to true when no memory to read image
        const void* buffer, // memory address containing webp compressed data.
        std::size_t sizebytes); // size of webp compressed data

    static void DeleteCache();

private:
    struct webp_cache;
    static webp_cache cache;
};


#endif // WEBPWRAPPER_H

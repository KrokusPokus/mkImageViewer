#ifndef AVIFWRAPPER_H
#define AVIFWRAPPER_H

#include <cstddef>

// AVIF reader based on libavif. Plain C++20, no Qt, no Win32.
//
// Threading: the decoder state (an animation that is read frame by frame) is process wide.
// All calls must come from one thread at a time (the image loading thread).
class AvifReader
{
public:
    // Reads one frame of the AVIF image.
    // 'buffer' is the complete file. It is only needed for the first call of an image: the data is copied into the
    // cache, the following calls for the same file may pass nullptr/0. Call DeleteCache() when switching to another file.
    //
    // Returns the pixels as 4 byte BGRA (nullptr on error), allocated with new[]: the caller releases it with delete[].
    // On error the cache is released.
    static void* ReadImage(int& width,            // width of the image loaded.
        int& height,                              // height of the image loaded.
        int& bpp,                                 // BYTES (not bits) PER PIXEL.
        bool& has_animation,                      // if the image is animated
        int frame_index,                          // index of frame
        int& frame_count,                         // number of frames
        int& frame_time,                          // frame duration in milliseconds
        void*& exif_chunk,                        // JPEG APP1 segment with the Exif data or nullptr (malloc(), must be freed by the caller)
        bool& outOfMemory,                        // set to true when there is no memory to read the image
        const void* buffer,                       // avif compressed data, or nullptr while an animation is cached
        std::size_t sizebytes);                   // size of avif compressed data

    static void DeleteCache();
};

#endif // AVIFWRAPPER_H

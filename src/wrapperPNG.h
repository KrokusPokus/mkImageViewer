#ifndef PNGWRAPPER_H
#define PNGWRAPPER_H

#include <cstddef>


// PNG / APNG reader based on libpng. Plain C++20, no Qt.
//
// Threading: the decoder state (an APNG animation that is read frame by frame) is process wide.
// All calls must come from one thread at a time (the image loading thread).
class PngReader
{
public:
    // Reads the next frame of the PNG (for an APNG every call returns the next frame, after the last one it starts again).
    // 'buffer' is the complete PNG file. For an animation the first call copies it: the following calls for the same
    // file may pass nullptr/0 (the cached copy is used), a different file drops the cached animation.
    //
    // Returns the pixels as 4 byte BGRA (nullptr on error), allocated with malloc(): the caller releases it with free().
    // On error the cache is released. 'outOfMemory' is only ever set to true, never reset.
    static void* ReadImage(int& width,            // width of the image loaded.
                           int& height,                              // height of the image loaded.
                           int& bpp,                                 // BYTES (not bits) PER PIXEL.
                           bool& has_animation,                      // if the image is animated
                           int& frame_count,                         // number of frames
                           int& frame_time,                          // frame duration in milliseconds
                           void*& exif_chunk,                        // JPEG APP1 segment with the Exif data or nullptr (malloc(), must be freed by the caller)
                           bool& outOfMemory,                        // set to true when there is no memory (or the image is too large) to read the image
                           const void* buffer,                       // png compressed data, or nullptr while an animation is cached
                           std::size_t sizebytes);                   // size of png compressed data

    static void DeleteCache();

    // Returns true if the PNG can not be decoded by QImageReader/QImage (APNG, or 32 bit size above INT_MAX)
    static bool MustUseLibpng(const void* buffer, std::size_t sizebytes);

    // Get EXIF block as JPEG APP1 segment (malloc(), must be freed by the caller), nullptr if there is none
    static void* GetEXIFBlock(const void* buffer, std::size_t sizebytes);
};

#endif // PNGWRAPPER_H

#ifndef HEIFWRAPPER_H
#define HEIFWRAPPER_H

#include <cstddef>

class HeifReader
{
public:
    // Returns data in the form 4-byte BGRA
    static void * ReadImage(int &width,   // width of the image loaded.
                         int &height,  // height of the image loaded.
                         int &bpp,     // BYTES (not bits) PER PIXEL.
                         int &frame_count, // number of top-level images
                         void* &exif_chunk, // Pointer to Exif data (must be freed by caller)
                         bool &outOfMemory, // set to true when no memory to read image
                         int frame_index, // index of requested frame
                         const void *buffer, // memory address containing heic compressed data.
                         std::size_t sizebytes); // size of heic compressed data.

    // Returns the Exif data of the primary image as a JPEG APP1 segment (allocated with malloc(), the caller frees it)
    // or nullptr. Only the container is parsed, nothing is decoded.
    static void* GetEXIFBlock(const void* buffer, std::size_t sizebytes);
};


#endif // HEIFWRAPPER_H

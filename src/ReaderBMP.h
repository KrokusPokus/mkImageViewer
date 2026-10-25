#ifndef READERBMP_H
#define READERBMP_H

#include "ImageProcessingTypes.h"

class CJPEGImage;

// Simple reader for windows bitmap files (.bmp)
class CReaderBMP
{
public:
    static void* ReadImage(int& width,   // width of the image loaded.
                 int& height,  // height of the image loaded.
                 int& bpp,     // BYTES (not bits) PER PIXEL.
                 bool& outOfMemory, // set to true when no memory to read image
                 const void* buffer, // memory address containing bmp data.
                 std::size_t sizebytes); // size of data.

private:
    CReaderBMP(void);
};

#endif // READERBMP_H

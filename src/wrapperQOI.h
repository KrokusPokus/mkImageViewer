#ifndef QOIWRAPPER_H
#define QOIWRAPPER_H

#include <cstddef>

class QoiReaderWriter
{
public:
    // Returns data in 3-byte BGR (padded to 4-byte boundary) or 4-byte BGRA
    static void* ReadImage(int& width,   // width of the image loaded.
        int& height,  // height of the image loaded.
        int& bpp,     // BYTES (not bits) PER PIXEL.
        bool& outOfMemory, // set to true when no memory to read image
        const void* buffer, // memory address containing qoi compressed data.
        std::size_t sizebytes); // size of qoi compressed data.
};


#endif // QOIWRAPPER_H

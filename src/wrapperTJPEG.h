#ifndef TJPEGWRAPPER_H
#define TJPEGWRAPPER_H

#include "libjpeg-turbo/include/turbojpeg.h" // for enum TJSAMP

#include <cstddef>

class TurboJpeg
{
public:
    // Returns data in the form BGRBGR**********BGR000 where the zeros are padding to 4 byte boundary
    static void * ReadImage(int &width,   // width of the image loaded.
                         int &height,  // height of the image loaded.
                         int &bpp,     // BYTES (not bits) PER PIXEL.
                         TJSAMP &chromoSubsampling, // chromo subsampling of image
                         bool &outOfMemory, // set to true when no memory to read image
                         const void *buffer, // memory address containing jpeg compressed data.
                         std::size_t sizebytes); // size of jpeg compressed data.
};


#endif // TJPEGWRAPPER_H

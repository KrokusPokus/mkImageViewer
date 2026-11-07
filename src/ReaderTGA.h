#ifndef READERTGA_H
#define READERTGA_H

#include "ImageProcessingTypes.h"

#include <cstddef>

class CBaseImage;

// Simple reader for .tga files
class CReaderTGA
{
public:
    // Geändert von LPCTSTR strFileName zu const void* pBuffer, std::size_t bufsize
    static void* ReadImage(int& width, int& height, int& targetChannels, bool& bOutOfMemory, const void* pBuffer, std::size_t bufsize);
    static bool IsAlphaChannelValid(int width, int height, uint32* pImageData);

private:
    CReaderTGA(void);
};


#endif // READERTGA_H

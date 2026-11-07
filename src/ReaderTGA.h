#ifndef READERTGA_H
#define READERTGA_H

#include <cstddef>
#include <cstdint>

class CBaseImage;

// Simple reader for .tga files
class CReaderTGA
{
public:
    static void* ReadImage(int& width, int& height, int& targetChannels, bool& bOutOfMemory, const void* pBuffer, std::size_t bufsize);
    static bool IsAlphaChannelValid(int width, int height, uint32_t* pImageData);

private:
    CReaderTGA();
};


#endif // READERTGA_H

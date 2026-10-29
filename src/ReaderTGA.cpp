#include "ReaderTGA.h"
#include "BaseImage.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>

// The TGA reader has been adapted and extended from the TGA reader used in an example of the BOINC project
// http://www.filewatcher.com/p/boinc-server-maker_7.0.27+dfsg-5_armhf.deb.5191030/usr/share/doc/boinc-server-maker/examples/tgalib.h.html
// The reader supports 8, 16, 24 and 32 bit TGAs

// The following types are supported
#define TGA_INDEXED         1        // This tells us it's a indexed 8 bit file
#define TGA_RGB             2        // This tells us it's a normal RGB (really BGR) file
#define TGA_MONO            3        // This tells us it's a monochrome file
#define TGA_RLE_INDEXED     9        // This tells us it's a indexed 8 bit Run-Length Encoded (RLE) file
#define TGA_RLE_RGB         10       // This tells us that the targa is Run-Length Encoded (RLE) RGB file
#define TGA_RLE_MONO        11       // This tells us that the targa is Run-Length Encoded (RLE) monochrome file

// Checks if all alpha values in a 32 bit image are zero. If yes the alpha channel should be ignored as the image
// would just be black.
bool CReaderTGA::IsAlphaChannelValid(int width, int height, uint32_t* pImageData)
{
    uint32_t pixel = 0;
    const std::size_t numPixels = static_cast<std::size_t>(width) * height; // can exceed 2^31
    for (std::size_t i = 0; i < numPixels; i++)
    {
        pixel = pixel | (*pImageData & 0xFF000000);
        pImageData++;
    }
    return pixel != 0;
}

void* CReaderTGA::ReadImage(int& width, int& height, int& targetChannels, bool& bOutOfMemory,
                               const void* pBuffer, std::size_t bufsize)
{
    bOutOfMemory = false;

    if (pBuffer == nullptr || bufsize < 18) {
        return nullptr;
    }

    const char* pBuf = static_cast<const char*>(pBuffer);

    uint8_t length = *reinterpret_cast<const uint8_t*>(pBuf); pBuf += 1;

    // Jump over one byte
    pBuf += 1;

    // Read in the imageType (RLE, RGB, etc...)
    uint8_t imageType = *reinterpret_cast<const uint8_t*>(pBuf); pBuf += 1;

    bool isIndexed = imageType == TGA_INDEXED || imageType == TGA_RLE_INDEXED;

    // Read in palette info
    uint16_t colormapStart = *reinterpret_cast<const uint16_t*>(pBuf); pBuf += sizeof(uint16_t);
    uint16_t colormapLen = *reinterpret_cast<const uint16_t*>(pBuf); pBuf += sizeof(uint16_t);
    uint8_t colormapBits = *reinterpret_cast<const uint8_t*>(pBuf); pBuf += 1;

    // Skip past general information we don't care about
    pBuf += 4;

    // Read the width, height and bits per pixel (16, 24 or 32)
    width = *reinterpret_cast<const uint16_t*>(pBuf); pBuf += sizeof(uint16_t);
    height = *reinterpret_cast<const uint16_t*>(pBuf); pBuf += sizeof(uint16_t);
    uint8_t bits = *reinterpret_cast<const uint8_t*>(pBuf); pBuf += 1;
    uint8_t attributes = *reinterpret_cast<const uint8_t*>(pBuf); pBuf += 1;

    bool flipVertically = ((attributes >> 5) & 1) == 0;

    // check preconditions:
    // - image size valid
    // - supported bits per pixel - only 8, 15, 16, 24 and 32 bits supported
    // - no strange image attributes
    // - supported image types - only RGB, mono and indexed
    // - color map format must be 256 RGB entries
    if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION ||
        (bits != 8 && bits != 16 && bits != 15 && bits != 24 && bits != 32) ||
        (imageType != TGA_INDEXED && imageType != TGA_RGB && imageType != TGA_MONO &&
         imageType != TGA_RLE_INDEXED && imageType != TGA_RLE_RGB && imageType != TGA_RLE_MONO) ||
        ((imageType == TGA_MONO || imageType == TGA_RLE_MONO) && bits != 8) ||
        (isIndexed && (colormapStart != 0 || colormapLen != 256 || colormapBits != 24)))
    {
        return nullptr;
    }

    // check memory footprint
    targetChannels = (bits == 32) ? 4 : 3;
    int targetStride = Helpers::DoPadding(width * targetChannels, 4);

    if ((double)width * height > MAX_IMAGE_PIXELS)
    {
        bOutOfMemory = true;
        return nullptr;
    }
    // 64 bit: width and height can be up to 65535 each, so the product overflows 32 bit (the buffer would be
    // allocated far too small and the decoding below would write beyond it)
    const std::size_t numberOfBytesRequired = static_cast<std::size_t>(targetStride) * height;

    // The file must be able to hold the pixel data at all, before memory is allocated for it: uncompressed every
    // pixel needs its bytes, with RLE one packet (1 byte + 1 pixel) covers at most 128 pixels.
    const std::size_t numPixelsTotal = static_cast<std::size_t>(width) * height;
    const std::size_t bytesPerSourcePixel = (bits + 7) / 8;
    const std::size_t minDataBytes = (imageType == TGA_RLE_INDEXED || imageType == TGA_RLE_RGB || imageType == TGA_RLE_MONO)
        ? (numPixelsTotal + 127) / 128 * (1 + bytesPerSourcePixel)
        : numPixelsTotal * bytesPerSourcePixel;
    if (bufsize < 18 + static_cast<std::size_t>(length) + (isIndexed ? 768 : 0) + minDataBytes)
    {
        return nullptr;
    }

    // Allocate memory which will hold our image data
    uint8_t* pImageData = new(std::nothrow) uint8_t[numberOfBytesRequired];
    if (pImageData == nullptr)
    {
        bOutOfMemory = true;
        return nullptr;
    }

    // Now we move the pointer to the pixel data
    pBuf = static_cast<const char*>(pBuffer) + 18 + length;
    if (static_cast<std::size_t>(pBuf - static_cast<const char*>(pBuffer)) > bufsize)
    {
        delete[] pImageData;
        return nullptr;
    }

    // read the color map
    uint8_t palette[768];
    if (isIndexed)
    {
        if (static_cast<std::size_t>((pBuf + 768) - static_cast<const char*>(pBuffer)) > bufsize)
        {
            delete[] pImageData;
            return nullptr;
        }
        memcpy(palette, pBuf, 768);
        pBuf += 768;
    }

    uint8_t* pImage = pImageData;

    if (imageType == TGA_MONO)
    {
        // this is always 8 bpp (checked above)
        for (int y = 0; y < height; y++)
        {
            uint8_t* pLine = pImage;
            for (int x = 0; x < width; x++)
            {
                if (static_cast<std::size_t>(pBuf - static_cast<const char*>(pBuffer)) + 1 > bufsize)
                {
                    delete[] pImageData;
                    return nullptr;
                }
                uint8_t grey = *reinterpret_cast<const uint8_t*>(pBuf);
                pBuf += 1;
                *pLine++ = grey;
                *pLine++ = grey;
                *pLine++ = grey;
            }
            pImage += targetStride;
        }
    }
    else if (imageType == TGA_INDEXED)
    {
        for (int y = 0; y < height; y++)
        {
            uint8_t* pLine = pImage;
            for (int x = 0; x < width; x++)
            {
                if (static_cast<std::size_t>(pBuf - static_cast<const char*>(pBuffer)) + 1 > bufsize)
                {
                    delete[] pImageData;
                    return nullptr;
                }
                uint8_t index = *reinterpret_cast<const uint8_t*>(pBuf);
                pBuf += 1;
                *pLine++ = palette[index * 3];
                *pLine++ = palette[index * 3 + 1];
                *pLine++ = palette[index * 3 + 2];
            }
            pImage += targetStride;
        }
    }
    else if (imageType == TGA_RGB)
    {
        // Check if the image is a 24 or 32-bit image
        if (bits == 24 || bits == 32)
        {
            // Calculate the channels (3 or 4)
            // Next, we calculate the stride and allocate enough memory for one pixel line.
            int channels = bits / 8;
            int stride = channels * width;

            // Load in all the pixel data line by line
            for (int y = 0; y < height; y++)
            {
                if (static_cast<std::size_t>((pBuf + stride) - static_cast<const char*>(pBuffer)) > bufsize)
                {
                    delete[] pImageData;
                    return nullptr;
                }
                memcpy(pImage, pBuf, stride);
                pBuf += stride;
                pImage += targetStride;
            }
        }
        // Check if the image is a 16 bit image (RGB stored in 1 unsigned short)
        else if (bits == 16 || bits == 15)
        {
            uint16_t pixel = 0;
            int r = 0, g = 0, b = 0;

            // Since we convert 16-bit images to 24 bit, we hardcode the channels to 3.
            // We then calculate the stride and allocate memory for the pixels.
            int channels = 3;
            int stride = channels * width;

            // Load in all the pixel data pixel by pixel
            for (int y = 0; y < height; y++)
            {
                uint8_t* pLine = pImage;
                for (int i = 0; i < width; i++)
                {
                    if (static_cast<std::size_t>((pBuf + sizeof(uint16_t)) - static_cast<const char*>(pBuffer)) > bufsize)
                    {
                        delete[] pImageData;
                        return nullptr;
                    }
                    // Read in the current pixel
                    pixel = *reinterpret_cast<const uint16_t*>(pBuf);
                    pBuf += sizeof(uint16_t);

                    // To convert a 16-bit pixel into an R, G, B, we need to
                    // do some masking and such to isolate each color value.
                    // 0x1f = 11111 in binary, so since 5 bits are reserved in
                    // each unsigned short for the R, G and B, we bit shift and mask
                    // to find each value.  We then bit shift up by 3 to get the full color.
                    b = (pixel & 0x1f) << 3;
                    g = ((pixel >> 5) & 0x1f) << 3;
                    r = ((pixel >> 10) & 0x1f) << 3;

                    *pLine++ = b;
                    *pLine++ = g;
                    *pLine++ = r;
                }
                pImage += targetStride;
            }
        }
    }
    // Else, it must be Run-Length Encoded (RLE)
    else
    {
        // First, let me explain real quickly what RLE is.
        // For further information, check out Paul Bourke's intro article at:
        // http://astronomy.swin.edu.au/~pbourke/dataformats/rle/
        //
        // Anyway, we know that RLE is a basic type compression.  It takes
        // colors that are next to each other and then shrinks that info down
        // into the color and a integer that tells how much of that color is used.
        // For instance:
        // aaaaabbcccccccc would turn into a5b2c8
        // Well, that's fine and dandy and all, but how is it done with RGB colors?
        // Simple, you read in an color count (rleID), and if that number is less than 128,
        // it does NOT have any optimization for those colors, so we just read the next
        // pixels normally.  Say, the color count was 28, we read in 28 colors like normal.
        // If the color count is over 128, that means that the next color is optimized and
        // we want to read in the same pixel color for a count of (colorCount - 127).
        // It's 127 because we add 1 to the color count, as you'll notice in the code.

        // Create some variables to hold the rleID, current colors read, channels, & stride.
        uint8_t rleID = 0;
        std::size_t colorsRead = 0; // byte offset in the target, can exceed 2^31
        int channels = bits / 8;
        int x = 0;
        int padding = targetStride - targetChannels * width;
        std::size_t i = 0;

        uint8_t colors[4];
        uint8_t* pColors = colors;
        pImage = pImageData;

        // Load in all the pixel data
        const std::size_t numPixels = static_cast<std::size_t>(width) * height;
        while (i < numPixels)
        {
            if (static_cast<std::size_t>(pBuf - static_cast<const char*>(pBuffer)) + 1 > bufsize)
            {
                delete[] pImageData;
                return nullptr;
            }
            // Read in the current color count + 1
            rleID = *reinterpret_cast<const uint8_t*>(pBuf);
            pBuf += 1;

            // Check if we don't have an encoded string of colors
            bool useSameColor;
            if (rleID < 128)
            {
                // Increase the count by 1
                rleID++;
                useSameColor = false;
            }
            // Else, let's read in a string of the same character
            else
            {
                // Minus the 128 ID + 1 (127) to get the color count that needs to be read
                rleID -= 127;
                useSameColor = true;

                if (static_cast<std::size_t>((pBuf + channels) - static_cast<const char*>(pBuffer)) > bufsize)
                {
                    delete[] pImageData;
                    return nullptr;
                }
                // Read in the current color, which is the same for a while
                memcpy(pColors, pBuf, channels);
                pBuf += channels;
            }

            // Go through and read all the unique colors found
            while (rleID && i < numPixels)
            {
                if (!useSameColor)
                {
                    if (static_cast<std::size_t>((pBuf + channels) - static_cast<const char*>(pBuffer)) > bufsize)
                    {
                        delete[] pImageData;
                        return nullptr;
                    }
                    // Read in the current color
                    memcpy(pColors, pBuf, channels);
                    pBuf += channels;
                }

                if (bits == 32)
                {
                    *reinterpret_cast<uint32_t*>(pImage + colorsRead) = *reinterpret_cast<uint32_t*>(pColors);
                }
                else
                {
                    if (bits == 8)
                    {
                        if (isIndexed)
                        {
                            int index = pColors[0] * 3;
                            pImage[colorsRead + 0] = palette[index];
                            pImage[colorsRead + 1] = palette[index + 1];
                            pImage[colorsRead + 2] = palette[index + 2];
                        } else {
                            pImage[colorsRead + 0] = pColors[0];
                            pImage[colorsRead + 1] = pColors[0];
                            pImage[colorsRead + 2] = pColors[0];
                        }
                    } else {
                        pImage[colorsRead + 0] = pColors[0];
                        pImage[colorsRead + 1] = pColors[1];
                        pImage[colorsRead + 2] = pColors[2];
                    }

                    // do padding target lines when 24 bpp
                    x++;
                    if (x == width)
                    {
                        x = 0;
                        colorsRead += padding;
                    }
                }

                // Increase the current pixels read, decrease the amount
                // of pixels left, and increase the starting index for the next pixel.
                i++;
                rleID--;
                colorsRead += targetChannels;
            }

        } // end of RLE pixel loop
    }

    // If image needs to be flipped, do this inplace
    if (flipVertically)
    {
        CBasicProcessing::MirrorVInplace(width, height, targetStride, pImageData);
    }

    return pImageData;
}

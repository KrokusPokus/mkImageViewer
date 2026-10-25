#include "ReaderBMP.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"

//////////////////////////////////////////////////////////////////////////////////
// BITMAP reading

struct BMHEADER {
    unsigned short int type;                 /* Magic identifier            */
    unsigned int size;                       /* File size in bytes          */
    unsigned short int reserved1, reserved2;
    unsigned int offset;                     /* Offset to image data, bytes */
};

struct BMINFOHEADER {
    unsigned int size;               /* Header size in bytes      */
    int width,height;                /* Width and height of image */
    unsigned short int planes;       /* Number of color planes    */
    unsigned short int bits;         /* Bits per pixel            */
    unsigned int compression;        /* Compression type          */
    unsigned int imagesize;          /* Image size in bytes       */
    int xresolution,yresolution;     /* Pixels per meter          */
    unsigned int ncolors;            /* Number of colors          */
    unsigned int importantcolors;    /* Important colors          */
};

void* CReaderBMP::ReadImage(int& width, int& height, int& bpp, bool& bOutOfMemory, const void* pBuffer, std::size_t bufsize)
{
    BMHEADER header;
    BMINFOHEADER infoheader;

    bOutOfMemory = false;

    int off = 0, amtRead = 0;
    char* pBuf = (char*)pBuffer;
    /* Read the header */
    amtRead += 14;
    if (bufsize < amtRead) return NULL;
    header.type = *((uint16*)pBuf); pBuf = pBuf + sizeof(uint16);
    header.size = *((uint32*)pBuf); pBuf += sizeof(uint32);
    header.reserved1 = *((uint16*)pBuf); pBuf += sizeof(uint16);
    header.reserved1 = *((uint16*)pBuf); pBuf += sizeof(uint16);
    header.offset = *((uint32*)pBuf); pBuf += sizeof(uint32);

    /* Read and check the information header */
    int size = sizeof(BMINFOHEADER);
    amtRead += size;
    if (bufsize < amtRead) return NULL;
    memcpy(&infoheader, pBuf, size);
    //pBuf += size;

    /* Only 24 and 32 bpp */
    if (infoheader.bits != 24 && infoheader.bits != 32 && infoheader.bits != 8) {
        return NULL;
    }
    /* Not too big files */
    if (infoheader.width > MAX_IMAGE_DIMENSION || infoheader.width <= 0 || abs(infoheader.height) > MAX_IMAGE_DIMENSION) {
        return NULL;
    }
    if ((double)infoheader.width * abs(infoheader.height) > MAX_IMAGE_PIXELS) {
        bOutOfMemory = true;
        return NULL;
    }

    // read palette for 8 bpp DIBs
    uint8 palette[4 * 256];
    if (infoheader.bits == 8) {
        off = infoheader.size + 14;
        if (bufsize < off) return NULL;
        pBuf = ((char*)pBuffer) + off;
        size = 256 * 4;
        amtRead = off + size;
        if (bufsize < amtRead) return NULL;
        memcpy(palette, pBuf, size);
        //pBuf += size;
    }

    /* Seek to the start of the image data */
    amtRead = off = header.offset;
    if (bufsize < off) return NULL;
    pBuf = ((char*)pBuffer) + off;

    // DIBs are normally stored flipped vertically (meaning they are stored bottom-up)
    bool bFlipped;
    if (infoheader.height < 0) {
        infoheader.height = -infoheader.height;
        bFlipped = false;
    }
    else {
        bFlipped = true;
    }

    int bytesPerPixel = infoheader.bits / 8;
    int paddedWidth = Helpers::DoPadding(infoheader.width * bytesPerPixel, 4);
    int fileSizeBytes = infoheader.height * paddedWidth;
    if (fileSizeBytes <= 0 || fileSizeBytes > MAX_BMP_FILE_SIZE) {
        bOutOfMemory = fileSizeBytes > MAX_BMP_FILE_SIZE;
        return NULL; // corrupt or manipulated header
    }

    uint8* pDest = new(std::nothrow) uint8[fileSizeBytes];
    if (pDest == NULL) {
        bOutOfMemory = true;
        return NULL;
    }
    uint8* pStart = bFlipped ? pDest + paddedWidth * (infoheader.height - 1) : pDest;
    for (int nLine = 0; nLine < infoheader.height; nLine++) {
        amtRead += paddedWidth;
        if (bufsize < amtRead)
        {
            delete[] pDest;
            return NULL;
        }
        memcpy(pStart, pBuf, paddedWidth);
        pBuf += paddedWidth;
        pStart = pStart + (bFlipped ? -paddedWidth : paddedWidth);
    }

    // Convert 8 bpp DIBs
    if (infoheader.bits == 8) {
        uint8* pTemp = (uint8*)CBasicProcessing::Convert8bppTo32bppDIB(infoheader.width, infoheader.height, pDest, palette);
        delete[] pDest;
        pDest = pTemp;
        infoheader.bits = 32;
    }

    width = infoheader.width;
    height = infoheader.height;
    bpp = (infoheader.bits / 8);

    return pDest;
}

#ifndef XMMIMAGE_H
#define XMMIMAGE_H

#include "ImageProcessingTypes.h"

#include <cstddef>

// Represents an image with line interleaving, optimal for SIMD processing. Each pixel has one float per channel
// (linear light, 0..4095), channel order is B, G, R. Lines are padded to a multiple of 'padding' pixels (4 for SSE,
// 8 for AVX), optionally more to avoid cache aliasing (see Init()). x stands for padding:
// BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBxxx
// GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGxxx
// RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRxxx
// BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBxxx
// GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGxxx
// RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRxxx
class CXMMImage
{
public:
    // padding is in pixels (not bytes)
    CXMMImage(int nWidth, int nHeight, int padding);
    CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding); // padding is in pixels (not bytes), width is always padded, height only when bPadHeight is true
    // As above. With bAvoidCacheAliasing, the line size is additionally chosen such that the rows read together by
    // ApplyFilter_AVX/SSE map to different cache sets (see Init()). Then GetPaddedWidth() can exceed the padded width.
    CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding, bool bAvoidCacheAliasing);
    // convert from section of 24 or 32 bpp DIB, from first to (and including) last column and row
    // padding is in pixels(not bytes), the line size avoids cache aliasing (as above)
    CXMMImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, const void* pDIB, int nChannels, int padding);
    ~CXMMImage(void);

    // Pointer to the (page aligned) memory of the image
//    void * AlignedPtr() { return m_pMemory; }
    void * AlignedPtr() const { return m_pMemory; }

    // Converts GetWidth() pixels of a 24 or 32 bpp DIB line (pSrcLine points to the first pixel to convert) into
    // line nLine of this image (sRGB 8 bit -> linear 12 bit, as float). The padding columns read by the filter
    // (up to the next multiple of 'padding') are set to zero.
    void ConvertLine(int nLine, const uint8* pSrcLine, int nChannels, int padding);
    // Same as ConvertLine(), AVX2 implementation (same results)
    void ConvertLine_AVX(int nLine, const uint8* pSrcLine, int nChannels, int padding);

    // Pointer to the first channel of line nLine, the other two channels follow at GetPaddedWidth() floats distance
    float* GetLine(int nLine) const { return (float*)m_pMemory + (size_t)nLine * 3 * m_nPaddedWidth; }

    // Geometry
    int GetWidth() const { return m_nWidth; }
    int GetHeight() const { return m_nHeight; }
    int GetPaddedWidth() const { return m_nPaddedWidth; }
    int GetPaddedHeight() const { return m_nPaddedHeight; }

private:
    int GetLineSize() const { return m_nPaddedWidth*4; }    // [GF] i16 -> f32
    int GetMemSize() const { return GetLineSize()*3*m_nPaddedHeight; }
    void Init(int nWidth, int nHeight, bool bPadHeight, int padding, bool bAvoidCacheAliasing = false);
    // Converts the pixels nFirst to nEnd - 1 of a DIB line, pDst: first channel of the target line
    void ConvertPixels(float* pDst, const uint8* pSrc, int nChannels, int nFirst, int nEnd) const;
    // Zeroes the padding columns of a line read by the filter (up to the next multiple of 'padding')
    void ZeroPaddingColumns(float* pDst, int padding) const;

    void* m_pMemory;
    int m_nWidth, m_nHeight;
    int m_nPaddedWidth; // in pixels
    int m_nPaddedHeight; // in pixels
};

#endif // XMMIMAGE_H

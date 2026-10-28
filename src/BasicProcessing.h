#ifndef BASICPROCESSING_H
#define BASICPROCESSING_H

#include "ImageProcessingTypes.h"
#include "WTLCompat.h"

// Basic image processing methods processing the image pixel data
class CBasicProcessing
{
public:

    // SIMD architecture register width
    enum SIMDArchitecture
    {
        MMX, // 64 bit
        SSE, // 128 bit
        AVX2 // 256 bit
    };

    // Note for all methods: The caller gets ownership of the returned image and is responsible to delete 
    // this pointer when no longer used.
    
    // Note for all methods: If there is not enough memory to allocate a new image, all methods return a null pointer
    // No exception is thrown in this case.

    // Note on DIBs: The rows of DIBs are padded to the next 4 byte boundary. For 32 bpp DIBs this is
    // implicitly true (no padding needed), for other types the padding must be considered.
    
    // Note: The output format of the JPEG lib is 24 bit BGR format. Padding is to 4 byte boundaries, as DIBs.

    // Convert a 8 bpp DIB with color palette to a 32 bpp BGRA DIB
    // The palette must contain 256 entries with format BGR0 (32 bits)
    static void* Convert8bppTo32bppDIB(int nWidth, int nHeight, const void* pDIBPixels, const uint8* pPalette);

    // Convert a 32 bpp BGRA DIB to a 24 bpp BGR DIB, flip vertically if requested.
    // Note that in contrast to the other methods, the target memory must be preallocated by the caller.
    // Consider row padding of the DIB to 4 byte boundary when allocating memory for the target DIB
    static void Convert32bppTo24bppDIB(int nWidth, int nHeight, void* pDIBTarget, const void* pDIBSource, bool bFlip);

    // Convert from a single channel image (8 bpp) to a 4 channel image (32 bpp DIB, BGRA).
    // The source image is row-padded to 4 byte boundary
    // No palette is given, the resulting image is black/white.
    static void* Convert1To4Channels(int nWidth, int nHeight, const void* pPixels);

    // Convert from a 3 channel image (24 bpp, BGR) to a 4 channel image (32 bpp DIB, BGRA)
    static void* Convert3To4Channels(int nWidth, int nHeight, const void* pIJLPixels);

    // Copy rectangular pixel block from source to target 32 bpp bitmap. The target bitmap is allocated
    // if the 'pTarget' parameter is NULL. Note that size of source and target rect must match.
    static void* CopyRect32bpp(void* pTarget, const void* pSource,  CSize targetSize, CRect targetRect,
        CSize sourceSize, CRect sourceRect);

    // Clockwise rotation of a 32 bit DIB. The rotation angle must be 90, 180 or 270 degrees, in all other
    // cases the return value is NULL
    static void* Rotate32bpp(int nWidth, int nHeight, const void* pDIBPixels, int nRotationAngleCW);

    // Mirror 32 bit DIB
    static void* Mirror32bpp(int nWidth, int nHeight, const void* pDIBPixels, bool bHorizontally);

    // Mirror 32 bit DIB horizontally
    static void* MirrorH32bpp(int nWidth, int nHeight, const void* pDIBPixels);

    // Mirror 32 bit DIB vertically
    static void* MirrorV32bpp(int nWidth, int nHeight, const void* pDIBPixels);

    // Mirror 32 bit DIB vertically inplace
    static void MirrorVInplace(int nWidth, int nHeight, int nStride, void* pDIBPixels);

    // Crop specified 32 bpp image. Returns NULL if cropRect is not fully inside source image.
    static void* Crop32bpp(int nWidth, int nHeight, const void* pDIBPixels, CRect cropRect);

    // Create a lookup-table for brightness and contrast correction.
    // dContrastEnh is in [-0.5 .. 0.5], dGamma in [0.1 .. 10]
    // The lookup table contains 256 uint8 entries (single channel LUT)
    static uint8* CreateSingleChannelLUT(double dContrastEnh, double dGamma);

    // Creates lookup tables for color saturation correction.
    // dSaturation must be between 0 and 2
    //  0: Grayscale image
    //  1: Image unmodified
    //  2: Image strongly saturated
    // Creates 6 * 256 int32 entries for the matrix elements of the saturation matrix. The elements are in 8.24 fixed point format.
    static int32* CreateColorSaturationLUTs(double dSaturation);

    // Apply a three channel LUT (memory layout: 256 uint8 values for B channel, followed by 256 uint8 values for G channel,
    // followed by 256 uint8 values for R channel, totally 3*256 bytes) to a 32 bpp BGRA DIB.
    // Notice that the A channel is not processed and set to fixed value 0xFF.
    static void* Apply3ChannelLUT32bpp(int nWidth, int nHeight, const void* pDIBPixels, const uint8* pLUT);

    // Apply the specified saturation LUTs (as returned by CreateColorSaturationLUTs() method) and then a 
    // three channel LUT (256*B, 256*G, 256*R, see above for details) to a 32 bpp BGRA DIB
    // Notice that the A channel is not processed and set to fixed value 0xFF.
    static void* ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight, const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT);

    // The following methods take into account that the original image with size (w, h) - denoted as 'sourceSize' -
    // is zoomed by a factor x, thus resulting in a (virtual) image size of (w * x, h * x) - denoted as 'fullTargetSize'.
    // Actually displayed is only a part of this virtual image, using a cropping rectangle with top, left
    // coordinates (xo, yo) - denoted as 'fullTargetOffset' - and a cropping rectangle size (wc, hc) - denoted
    // as 'clippedTargetSize'.

    // Resize 32 or 24 bpp BGR(A) image using point sampling (i.e. no interpolation).
    // Point sampling is fast but produces a lot of aliasing artifacts.
    // Notice that the A channel is kept unchanged for 32 bpp images.
    // Notice that the returned image is always 32 bpp!
    // fullTargetSize: Virtual size of target image (unclipped).
    // fullTargetOffset: Offset for start of clipping window (in the region given by fullTargetSize)
    // clippedTargetSize: Size of clipped window - returned DIB has this size
    // sourceSize: Size of source image
    // pPixels: Source image
    // nChannels: Number of channels (bytes) in source image, must be 3 or 4
    // Returns a 32 bpp BGRA DIB of size 'clippedTargetSize'
    static void* PointSample(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, 
        CSize sourceSize, const void* pPixels, int nChannels);

    // High quality downsampling of 32 or 24 bpp BGR(A) image to target size, using a set of down-sampling kernels.
    // Notice that the A channel is not processed and set to fixed value 0xFF.
    // Notice that the returned image is always 32 bpp!
    // eFilter: Filter to apply. Note that the filter type can only be one of the downsampling filter types.
    // See PointSample() for other parameters
    // Returns a 32 bpp BGRA DIB of size 'clippedTargetSize'
    static void* SampleDown_HQ(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
        CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter);

    // Same as above, SIMD (AVX2/SSE/MMX) implementation. Returns NULL if memory is exhausted.
    // Notice that the A channel is not processed. The AVX2 implementation sets it to 0xFF, with SSE/MMX it is undefined.
    // Notice that the returned image is always 32 bpp!
    static void* SampleDown_HQ_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
        CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, SIMDArchitecture simd);

    // High quality upsampling of 32 or 24 bpp BGR(A) image using bicubic interpolation.
    // Notice that the A channel is not processed and set to fixed value 0xFF.
    // Notice that the returned image is always 32 bpp!
    // See PointSample() for parameters
    static void* SampleUp_HQ(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
        CSize sourceSize, const void* pPixels, int nChannels);

    // Same as above, SIMD (AVX2/SSE/MMX) implementation. Returns NULL if memory is exhausted.
    // Notice that the A channel is not processed. The AVX2 implementation sets it to 0xFF, with SSE/MMX it is undefined.
    // Notice that the returned image is always 32 bpp!
    static void* SampleUp_HQ_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
        CSize sourceSize, const void* pPixels, int nChannels, SIMDArchitecture simd);

private:
    CBasicProcessing(void);
};

#endif // BASICPROCESSING_H

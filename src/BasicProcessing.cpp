#include "BasicProcessing.h"

#include "Helpers.h"
#include "LookupTables.h"    // needed for LinRGB12_sRGB8[4096]
#include "ProcessingThreadPool.h"
#include "ResizeFilter.h"
#include "XMMImage.h"

#include <immintrin.h> // Beinhaltet alle SSE/AVX-Intrinsics und _mm_round_ps

#include <cstdint>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <new>

#include <unistd.h> // sysconf() for the cache sizes

#define ALPHA_OPAQUE 0xFF000000

// Maximal number of threads for rotating and mirroring the original image (Rotate24bpp/32bpp, Mirror24bpp/32bpp).
// These operations only copy pixels, their speed is limited by the memory bandwidth, not by the computation:
// a single core cannot keep enough memory accesses in flight, a few cores saturate the bandwidth.
// Measured on i7-10700K, dual channel DDR4 (copy bandwidth ~29 GB/s), 40000x30591, 3 channels:
//   rotate 180: 1 thread 910 ms, 2 threads 434 ms, 4 threads 248 ms, 8 threads 259 ms
//   rotate 90:  1 thread 1538 ms, 2 threads 868 ms, 4 threads 595 ms, 8 threads 655 ms
// With 4 threads the bandwidth is saturated, more threads do not help (90 degrees even got slower).
// May be raised for systems with considerably higher memory bandwidth (more memory channels, faster memory).
constexpr int MAX_THREADS_COPY_OPERATIONS = 4;

/////////////////////////////////////////////////////////////////////////////////////////////
// Processing images stripwise on thread pool
/////////////////////////////////////////////////////////////////////////////////////////////

static void* SampleDown_HQ_MMX_SSE_Core(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pIJLPixels, int nChannels, EFilterType eFilter, bool bSSE, uint8_t* pTarget);

static bool SampleSlice_HQ_AVX(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, uint8_t* pTarget);

static void* SampleUp_HQ_MMX_SSE_Core(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pIJLPixels, int nChannels, bool bSSE,
    uint8_t* pTarget);

//---------------------------------------------------------------------------------------------

// Sets FTZ (flush to zero) and DAZ (denormals are zero) for the current thread while in scope and restores
// the previous state afterwards. Denormal floats would trigger slow microcode assists.
class CDenormalsOff {
public:
    CDenormalsOff() : m_nOldCSR(_mm_getcsr()) { _mm_setcsr(m_nOldCSR | 0x8040); }
    ~CDenormalsOff() { _mm_setcsr(m_nOldCSR); }
private:
    unsigned int m_nOldCSR;
};

// Request for upsampling or downsampling
class CRequestUpDownSampling : public CProcessingRequest {
public:
    CRequestUpDownSampling(const void* pSourcePixels, DSize sourceSize, void* pTargetPixels,
        DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
        int nChannels, EFilterType eFilter, CBasicProcessing::SIMDArchitecture simd)
        : CProcessingRequest(pSourcePixels, sourceSize, pTargetPixels, fullTargetSize, fullTargetOffset, clippedTargetSize) {
        Channels = nChannels;
        Filter = eFilter;
        SIMD = simd;
        //StripPadding = (simd == CBasicProcessing::AVX2) ? 16 : 8; // important to set for AVX
        // For linear processing, we use double the data size, so can process only half as many pixels at once, so one strip can only be half the height of than in SSE.
        // (All slices must have a height dividable by 'StripPadding', except the last one)
        StripPadding = (simd == CBasicProcessing::AVX2) ? 8 : 4;
    }

    // AVX: the whole slice in one pass (source lines are converted only once, see SampleSlice_HQ_AVX)
    virtual bool ProcessSlice(int offsetY, int sizeY) {
        if (SIMD != CBasicProcessing::AVX2) {
            // the SSE/MMX path is kept unchanged and processes the slice strip by strip
            return CProcessingRequest::ProcessSlice(offsetY, sizeY);
        }
        CDenormalsOff denormalsOff;
        return SampleSlice_HQ_AVX(FullTargetSize,
            DPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
            DSize(ClippedTargetSize.cx, sizeY),
            SourceSize, SourcePixels,
            Channels, Filter,
            (uint8_t*)TargetPixels + (size_t)ClippedTargetSize.cx * 4 * offsetY);
    }

    // SSE/MMX only
    virtual bool ProcessStrip(int offsetY, int sizeY) {
        CDenormalsOff denormalsOff;
        if (Filter == Filter_Upsampling_Bicubic)
            return nullptr != SampleUp_HQ_MMX_SSE_Core(FullTargetSize,
                DPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                DSize(ClippedTargetSize.cx, sizeY),
                SourceSize, SourcePixels,
                Channels, SIMD == CBasicProcessing::SSE,
                (uint8_t*)TargetPixels + (size_t)ClippedTargetSize.cx * 4 * offsetY);
        else
            return nullptr != SampleDown_HQ_MMX_SSE_Core(FullTargetSize,
                DPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                DSize(ClippedTargetSize.cx, sizeY),
                SourceSize, SourcePixels,
                Channels,
                Filter, SIMD == CBasicProcessing::SSE,
                (uint8_t*)TargetPixels + (size_t)ClippedTargetSize.cx * 4 * offsetY);
    }

    int Channels;
    EFilterType Filter;
    CBasicProcessing::SIMDArchitecture SIMD;
};

/////////////////////////////////////////////////////////////////////////////////////////////
// LUT creation for saturation, contrast and brightness and application of LUT
/////////////////////////////////////////////////////////////////////////////////////////////

int32_t* CBasicProcessing::CreateColorSaturationLUTs(double dSaturation) {
    const double cdScaler = 1 << 16;
    int32_t* pLUTs = new int32_t[6 * 256];
    for (int i = 0; i < 256; i++) {
        pLUTs[i] = Helpers::RoundToInt(i * (0.299 + 0.701 * dSaturation) * cdScaler);
        pLUTs[i + 256] = Helpers::RoundToInt(i * 0.587 * (1.0 - dSaturation) * cdScaler);
        pLUTs[i + 512] = Helpers::RoundToInt(i * 0.114 * (1.0 - dSaturation) * cdScaler);
        pLUTs[i + 768] = Helpers::RoundToInt(i * 0.299 * (1.0 - dSaturation) * cdScaler);
        pLUTs[i + 1024] = Helpers::RoundToInt(i * (0.587 + 0.413 * dSaturation) * cdScaler);
        pLUTs[i + 1280] = Helpers::RoundToInt(i * (0.114 + 0.886 * dSaturation) * cdScaler);
    }

    return pLUTs;
}

void* CBasicProcessing::Apply3ChannelLUT32bpp(int nWidth, int nHeight, const void* pBitmapPixels, const uint8_t* pLUT) {
    if (pBitmapPixels == nullptr || pLUT == nullptr) {
        return nullptr;
    }

    uint32_t* pTarget = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pTarget == nullptr) return nullptr;
    const uint32_t* pSrc = (uint32_t*)pBitmapPixels;
    uint32_t* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            uint32_t nSrcPixels = *pSrc;
            *pTgt = pLUT[nSrcPixels & 0xFF] + pLUT[256 + ((nSrcPixels >> 8) & 0xFF)] * 256 + 
                pLUT[512 + ((nSrcPixels >> 16) & 0xFF)] * 65536 + ALPHA_OPAQUE;
            pTgt++; pSrc++;
        }
    }
    return pTarget;
}

void* CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight, const void* pBitmapPixels, const int32_t* pSatLUTs, const uint8_t* pLUT) {
    if (pBitmapPixels == nullptr || pSatLUTs == nullptr || pLUT == nullptr) {
        return nullptr;
    }

    const int cnScaler = 1 << 16;
    const int cnMax = 255 * cnScaler;
    uint32_t* pTarget = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pTarget == nullptr) return nullptr;
    const uint32_t* pSrc = (uint32_t*)pBitmapPixels;
    uint32_t* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            uint32_t nSrcPixels = *pSrc;
            int32_t nSrcBlue = nSrcPixels & 0xFF;
            int32_t nSrcGreen = (nSrcPixels >> 8) & 0xFF;
            int32_t nSrcRed = (nSrcPixels >> 16) & 0xFF;
            int32_t nRed = pSatLUTs[nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
            int32_t nGreen = pSatLUTs[768 + nSrcRed] + pSatLUTs[1024 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
            int32_t nBlue = pSatLUTs[768 + nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[1280 + nSrcBlue];
            *pTgt = pLUT[std::max(0, std::min(cnMax, nBlue)) >> 16] + pLUT[256 + (std::max(0, std::min(cnMax, nGreen)) >> 16)] * 256 +
                pLUT[512 + (std::max(0, std::min(cnMax, nRed)) >> 16)] * 65536 + ALPHA_OPAQUE;
            pTgt++; pSrc++;
        }
    }
    return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Conversion and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

// Rotate a block of a 32 bit bitmap from source to target by 90 or 270 degrees
static void RotateBlock32bpp(const uint32_t* pSrc, uint32_t* pTgt, int nWidth, int nHeight,
                             int nXStart, int nYStart, int nBlockWidth, int nBlockHeight, bool bCW) {
    int nIncTargetLine = nHeight;
    int nIncSource = nWidth - nBlockWidth;
    const uint32_t* pSource = pSrc + (size_t)nWidth * nYStart + nXStart;
    uint32_t* pTarget = bCW ? pTgt + (size_t)nIncTargetLine * nXStart + (nHeight - 1 - nYStart) :
        pTgt + (size_t)nIncTargetLine * (nWidth - 1 - nXStart) + nYStart;
    uint32_t* pStartYPtr = pTarget;
    if (!bCW) nIncTargetLine = -nIncTargetLine;
    int nIncStartYPtr = bCW ? -1 : +1;

    for (uint32_t i = 0; i < nBlockHeight; i++) {
        for (uint32_t j = 0; j < nBlockWidth; j++) {
            *pTarget = *pSource;
            pTarget += nIncTargetLine;
            pSource++;
        }
        pStartYPtr += nIncStartYPtr;
        pTarget = pStartYPtr;
        pSource += nIncSource;
    }
}

// Rotating and mirroring only copy pixels: the work is split in ranges of source lines [nFirstLine, nEndLine),
// which are processed in parallel (ProcessLinesParallel). Each source pixel has exactly one target pixel, so the
// ranges never write to the same memory.

// Request processing ranges of image lines with a function, on at most MAX_THREADS_COPY_OPERATIONS threads
class CRequestCopyLines : public CProcessingRequest {
public:
    CRequestCopyLines(int nWidth, int nHeight, const std::function<void(int, int)>& processLines)
        : CProcessingRequest(nullptr, DSize(nWidth, nHeight), nullptr, DSize(nWidth, nHeight), DPoint(0, 0), DSize(nWidth, nHeight)),
          m_processLines(processLines) {
        StripPadding = 32; // block size of the rotation by 90 degrees
        MaxThreads = MAX_THREADS_COPY_OPERATIONS;
    }

    virtual bool ProcessSlice(int offsetY, int sizeY) {
        m_processLines(offsetY, offsetY + sizeY);
        return true;
    }

    virtual bool ProcessStrip(int offsetY, int sizeY) {
        return ProcessSlice(offsetY, sizeY);
    }

private:
    std::function<void(int, int)> m_processLines;
};

// Calls processLines(nFirstLine, nEndLine) for ranges covering the lines 0 to nHeight - 1, in parallel
static void ProcessLinesParallel(int nWidth, int nHeight, const std::function<void(int, int)>& processLines) {
    CRequestCopyLines request(nWidth, nHeight, processLines);
    CProcessingThreadPool::This().Process(&request);
}

void* CBasicProcessing::Rotate32bpp(int nWidth, int nHeight, const void* pBitmapPixels, int nRotationAngleCW) {
    if (pBitmapPixels == nullptr) {
        return nullptr;
    }
    if (nRotationAngleCW != 90 && nRotationAngleCW != 180 && nRotationAngleCW != 270) {
        return nullptr; // not allowed
    }

    uint32_t* pTarget = new(std::nothrow) uint32_t[nHeight * nWidth];
    if (pTarget == nullptr) return nullptr;
    const uint32_t* pSource = (uint32_t*)pBitmapPixels;

    if (nRotationAngleCW == 180) {
        ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
            for (int i = nFirstLine; i < nEndLine; i++) {
                uint32_t* pTgt = pTarget + (size_t)nWidth*(nHeight - 1 - i) + (nWidth - 1);
                const uint32_t* pSrc = pSource + (size_t)nWidth*i;
                for (int j = 0; j < nWidth; j++) {
                    *pTgt = *pSrc;
                    pTgt -= 1;
                    pSrc += 1;
                }
            }
        });
        return pTarget;
    }

    const int cnBlockSize = 32;
    ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
        for (int nY = nFirstLine; nY < nEndLine; nY += cnBlockSize) {
            for (int nX = 0; nX < nWidth; nX += cnBlockSize) {
                RotateBlock32bpp(pSource, pTarget, nWidth, nHeight,
                    nX, nY,
                    std::min(cnBlockSize, nWidth - nX),
                    std::min(cnBlockSize, nEndLine - nY), nRotationAngleCW == 90);
            }
        }
    });

    return pTarget;
}

void* CBasicProcessing::Mirror32bpp(int nWidth, int nHeight, const void* pBitmapPixels, bool bHorizontally) {
    return bHorizontally ? CBasicProcessing::MirrorH32bpp(nWidth, nHeight, pBitmapPixels) :
        CBasicProcessing::MirrorV32bpp(nWidth, nHeight, pBitmapPixels);
}

void* CBasicProcessing::MirrorH32bpp(int nWidth, int nHeight, const void* pBitmapPixels) {
    uint32_t* pTarget = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pTarget == nullptr) return nullptr;
    ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
        for (int j = nFirstLine; j < nEndLine; j++) {
            uint32_t* pTgt = pTarget + (size_t)j * nWidth;
            const uint32_t* pSource = (uint32_t*)pBitmapPixels + (size_t)(j + 1) * nWidth - 1;
            for (int i = 0; i < nWidth; i++) {
                *pTgt = *pSource;
                pTgt++; pSource--;
            }
        }
    });
    return pTarget;
}

void* CBasicProcessing::MirrorV32bpp(int nWidth, int nHeight, const void* pBitmapPixels) {
    uint32_t* pTarget = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pTarget == nullptr) return nullptr;
    ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
        for (int j = nFirstLine; j < nEndLine; j++) {
            memcpy(pTarget + (size_t)j * nWidth, (uint32_t*)pBitmapPixels + (size_t)(nHeight - 1 - j) * nWidth,
                   (size_t)nWidth * sizeof(uint32_t));
        }
    });
    return pTarget;
}

// 24 bpp (3 channels) versions of the rotation and mirroring above. Lines are padded to 4 bytes.

static inline void CopyPixel24bpp(uint8_t* pTarget, const uint8_t* pSource) {
    memcpy(pTarget, pSource, 3);
}

// Zeroes the padding bytes at the end of the lines of a 24 bpp image. Called after the pixels have been written,
// so the memory pages are already mapped (by the threads writing the pixels).
static void ZeroPadding24bpp(uint8_t* pImage, int nWidth, int nHeight) {
    const size_t nStride = Helpers::DoPadding(nWidth * 3, 4);
    if (nStride > (size_t)nWidth * 3) {
        for (int j = 0; j < nHeight; j++) {
            memset(pImage + j * nStride + (size_t)nWidth * 3, 0, nStride - (size_t)nWidth * 3);
        }
    }
}

// Rotates a block of the source image by 90 degrees (clockwise if bCW, else counter-clockwise),
// target pixel of source pixel (x, y): CW (nHeight - 1 - y, x), CCW (y, nWidth - 1 - x)
static void RotateBlock24bpp(const uint8_t* pSrc, uint8_t* pTgt, int nWidth, int nHeight, int nXStart, int nYStart,
                             int nBlockWidth, int nBlockHeight, bool bCW) {
    const size_t nSrcStride = Helpers::DoPadding(nWidth * 3, 4);
    const ptrdiff_t nTgtStride = Helpers::DoPadding(nHeight * 3, 4);
    // the source line runs along a target column: downwards (CW) or upwards (CCW)
    const ptrdiff_t nTgtStep = bCW ? nTgtStride : -nTgtStride;
    for (int y = nYStart; y < nYStart + nBlockHeight; y++) {
        const uint8_t* pSource = pSrc + y * nSrcStride + (size_t)nXStart * 3;
        uint8_t* pTarget = bCW ? pTgt + nXStart * nTgtStride + (ptrdiff_t)(nHeight - 1 - y) * 3
                             : pTgt + (ptrdiff_t)(nWidth - 1 - nXStart) * nTgtStride + (ptrdiff_t)y * 3;
        for (int x = 0; x < nBlockWidth; x++) {
            CopyPixel24bpp(pTarget, pSource);
            pSource += 3;
            pTarget += nTgtStep;
        }
    }
}

void* CBasicProcessing::Rotate24bpp(int nWidth, int nHeight, const void* pPixels, int nRotationAngleCW) {
    if (pPixels == nullptr || (nRotationAngleCW != 90 && nRotationAngleCW != 180 && nRotationAngleCW != 270)) {
        return nullptr;
    }
    const uint8_t* pSource = (const uint8_t*)pPixels;
    const size_t nSrcStride = Helpers::DoPadding(nWidth * 3, 4);

    if (nRotationAngleCW == 180) {
        uint8_t* pTarget = new(std::nothrow) uint8_t[nSrcStride * nHeight];
        if (pTarget == nullptr) return nullptr;
        ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
            for (int j = nFirstLine; j < nEndLine; j++) {
                const uint8_t* pSrc = pSource + j * nSrcStride;
                uint8_t* pTgt = pTarget + (nHeight - 1 - j) * nSrcStride + (size_t)(nWidth - 1) * 3;
                for (int i = 0; i < nWidth; i++) {
                    CopyPixel24bpp(pTgt, pSrc);
                    pTgt -= 3;
                    pSrc += 3;
                }
            }
        });
        ZeroPadding24bpp(pTarget, nWidth, nHeight);
        return pTarget;
    }

    uint8_t* pTarget = new(std::nothrow) uint8_t[(size_t)Helpers::DoPadding(nHeight * 3, 4) * nWidth];
    if (pTarget == nullptr) return nullptr;
    const int cnBlockSize = 32;
    ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
        for (int nY = nFirstLine; nY < nEndLine; nY += cnBlockSize) {
            for (int nX = 0; nX < nWidth; nX += cnBlockSize) {
                RotateBlock24bpp(pSource, pTarget, nWidth, nHeight, nX, nY,
                    std::min(cnBlockSize, nWidth - nX), std::min(cnBlockSize, nEndLine - nY), nRotationAngleCW == 90);
            }
        }
    });
    ZeroPadding24bpp(pTarget, nHeight, nWidth);
    return pTarget;
}

void* CBasicProcessing::Mirror24bpp(int nWidth, int nHeight, const void* pPixels, bool bHorizontally) {
    if (pPixels == nullptr) {
        return nullptr;
    }
    const size_t nStride = Helpers::DoPadding(nWidth * 3, 4);
    uint8_t* pTarget = new(std::nothrow) uint8_t[nStride * nHeight];
    if (pTarget == nullptr) return nullptr;
    ProcessLinesParallel(nWidth, nHeight, [=](int nFirstLine, int nEndLine) {
        for (int j = nFirstLine; j < nEndLine; j++) {
            const uint8_t* pSrc = (const uint8_t*)pPixels + (bHorizontally ? j : (nHeight - 1 - j)) * nStride;
            uint8_t* pTgt = pTarget + j * nStride;
            if (bHorizontally) {
                pSrc += (size_t)(nWidth - 1) * 3;
                for (int i = 0; i < nWidth; i++) {
                    CopyPixel24bpp(pTgt, pSrc);
                    pTgt += 3;
                    pSrc -= 3;
                }
            } else {
                memcpy(pTgt, pSrc, (size_t)nWidth * 3);
            }
        }
    });
    ZeroPadding24bpp(pTarget, nWidth, nHeight);
    return pTarget;
}

void CBasicProcessing::MirrorVInplace(int nWidth, int nHeight, int nStride, void* pBitmapPixels) {
    for (int j = 0; j < (nHeight >> 1); j++) {
        if ((nStride & 3) == 0) {
            const size_t nPixelsPerStride = nStride >> 2;
            uint32_t* pSource = (uint32_t*)pBitmapPixels + (nHeight - 1 - j) * nPixelsPerStride;
            uint32_t* pTgt = (uint32_t*)pBitmapPixels + j * nPixelsPerStride;
            for (size_t i = 0; i < nPixelsPerStride; i++) {
                uint32_t t = *pTgt;
                *pTgt = *pSource;
                *pSource = t;
                pTgt++; pSource++;
            }
        } else {
            uint8_t* pSource = (uint8_t*)pBitmapPixels + (size_t)(nHeight - 1 - j) * nStride;
            uint8_t* pTgt = (uint8_t*)pBitmapPixels + (size_t)j * nStride;
            for (int i = 0; i < nStride; i++) {
                uint8_t t = *pTgt;
                *pTgt = *pSource;
                *pSource = t;
                pTgt++; pSource++;
            }
        }
    }
}

void* CBasicProcessing::Convert8bppTo32bppBitmap(int nWidth, int nHeight, const void* pBitmapPixels, const uint8_t* pPalette) {
    if (pBitmapPixels == nullptr || pPalette == nullptr) {
        return nullptr;
    }
    int nPaddedWidthS = Helpers::DoPadding(nWidth, 4);
    int nPadS = nPaddedWidthS - nWidth;
    uint32_t* pNewBitmap = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pNewBitmap == nullptr) return nullptr;
    uint32_t* pTargetBitmap = pNewBitmap;
    const uint8_t* pSourceBitmap = (uint8_t*)pBitmapPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTargetBitmap++ = pPalette[4 * *pSourceBitmap] + pPalette[4 * *pSourceBitmap + 1] * 256 + pPalette[4 * *pSourceBitmap + 2] * 65536 + ALPHA_OPAQUE;
            pSourceBitmap ++;
        }
        pSourceBitmap += nPadS;
    }
    return pNewBitmap;
}

void CBasicProcessing::Convert32bppTo24bppBitmap(int nWidth, int nHeight, void* pBitmapTarget, 
                                              const void* pBitmapSource, bool bFlip) {
    if (pBitmapTarget == nullptr || pBitmapSource == nullptr) {
        return;
    }
    int nPaddedWidth = Helpers::DoPadding(nWidth*3, 4);
    int nPad = nPaddedWidth - nWidth*3;
    uint8_t* pTargetBitmap = (uint8_t*)pBitmapTarget;
    for (int j = 0; j < nHeight; j++) {
        const uint8_t* pSourceBitmap;
        if (bFlip) {
            pSourceBitmap = (uint8_t*)pBitmapSource + (size_t)(nHeight - 1 - j)*nWidth*4;
        } else {
            pSourceBitmap = (uint8_t*)pBitmapSource + (size_t)j*nWidth*4;
        }
        for (int i = 0; i < nWidth; i++) {
            *pTargetBitmap++ = pSourceBitmap[0];
            *pTargetBitmap++ = pSourceBitmap[1];
            *pTargetBitmap++ = pSourceBitmap[2];
            pSourceBitmap += 4;
        }
        pTargetBitmap += nPad;
    }
}

void* CBasicProcessing::Convert1To4Channels(int nWidth, int nHeight, const void* pPixels) {
    if (pPixels == nullptr) {
        return nullptr;
    }
    int nPadSrc = Helpers::DoPadding(nWidth, 4) - nWidth;
    uint32_t* pNewBitmap = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pNewBitmap == nullptr) return nullptr;
    uint32_t* pTarget = pNewBitmap;
    const uint8_t* pSource = (uint8_t*)pPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTarget++ = *pSource + *pSource * 256 + *pSource * 65536 + ALPHA_OPAQUE;
            pSource++;
        }
        pSource += nPadSrc;
    }
    return pNewBitmap;
}

void* CBasicProcessing::Convert3To4Channels(int nWidth, int nHeight, const void* pIJLPixels) {
    if (pIJLPixels == nullptr) {
        return nullptr;
    }
    int nPadSrc = Helpers::DoPadding(nWidth*3, 4) - nWidth*3;
    uint32_t* pNewBitmap = new(std::nothrow) uint32_t[nWidth * nHeight];
    if (pNewBitmap == nullptr) return nullptr;
    uint32_t* pTarget = pNewBitmap;
    const uint8_t* pSource = (uint8_t*)pIJLPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTarget++ = pSource[0] + pSource[1] * 256 + pSource[2] * 65536 + ALPHA_OPAQUE;
            pSource += 3;
        }
        pSource += nPadSrc;
    }
    return pNewBitmap;
}

void* CBasicProcessing::CopyRect32bpp(void* pTarget, const void* pSource,  DSize targetSize, DRect targetRect,
                                      DSize sourceSize, DRect sourceRect) {
    if (pSource == nullptr || sourceRect.Size() != targetRect.Size() || 
        sourceRect.left < 0 || sourceRect.right > sourceSize.cx ||
        sourceRect.top < 0 || sourceRect.bottom > sourceSize.cy ||
        targetRect.left < 0 || targetRect.right > targetSize.cx ||
        targetRect.top < 0 || targetRect.bottom > targetSize.cy) {
        return nullptr;
    }
    uint32_t* pSourceBitmap = (uint32_t*)pSource;
    uint32_t* pTargetBitmap = (uint32_t*)pTarget;
    if (pTargetBitmap == nullptr) {
        pTargetBitmap = new(std::nothrow) uint32_t[targetSize.cx * targetSize.cy];
        if (pTargetBitmap == nullptr) return nullptr;
        pTarget = pTargetBitmap;
    }

    pTargetBitmap += (targetRect.top * targetSize.cx) + targetRect.left;
    pSourceBitmap += (sourceRect.top * sourceSize.cx) + sourceRect.left;
    for (int y = 0; y < sourceRect.Height(); y++) {
        memcpy(pTargetBitmap, pSourceBitmap, sourceRect.Width()*sizeof(uint32_t));
        pTargetBitmap += targetSize.cx;
        pSourceBitmap += sourceSize.cx;
    }

    return pTarget;
}

void* CBasicProcessing::Crop32bpp(int nWidth, int nHeight, const void* pBitmapPixels, DRect cropRect) {
    if (pBitmapPixels == nullptr || cropRect.Width() == 0 || cropRect.Height() == 0) {
        return nullptr;
    }

    return CopyRect32bpp(nullptr, pBitmapPixels, cropRect.Size(), DRect(0, 0, cropRect.Width(), cropRect.Height()),
        DSize(nWidth, nHeight), cropRect);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Simple point sampling resize and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::PointSample(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize, 
    DSize sourceSize, const void* pPixels, int nChannels) {
    if (fullTargetSize.cx < 1 || fullTargetSize.cy < 1 ||
        clippedTargetSize.cx < 1 || clippedTargetSize.cy < 1 ||
        fullTargetOffset.x < 0 || fullTargetOffset.y < 0 ||
        clippedTargetSize.cx + fullTargetOffset.x > fullTargetSize.cx ||
        clippedTargetSize.cy + fullTargetOffset.y > fullTargetSize.cy ||
        pPixels == nullptr || (nChannels != 3 && nChannels != 4)) {
        return nullptr;
    }

    uint8_t* pBitmap = new(std::nothrow) uint8_t[(size_t)clippedTargetSize.cx * 4 * clippedTargetSize.cy];
    if (pBitmap == nullptr) return nullptr;

    uint32_t nIncrementX, nIncrementY;
    if (fullTargetSize.cx <= sourceSize.cx) {
        // Downsampling
        nIncrementX = ((uint32_t)sourceSize.cx << 16)/fullTargetSize.cx + 1;
        nIncrementY = ((uint32_t)sourceSize.cy << 16)/fullTargetSize.cy + 1;
    } else {
        // Upsampling
        nIncrementX = (fullTargetSize.cx == 1) ? 0 : (uint32_t)((65536*(uint32_t)(sourceSize.cx - 1) + 65535)/(fullTargetSize.cx - 1));
        nIncrementY = (fullTargetSize.cy == 1) ? 0 : (uint32_t)((65536*(uint32_t)(sourceSize.cy - 1) + 65535)/(fullTargetSize.cy - 1));
    }

    int nPaddedSourceWidth = Helpers::DoPadding(sourceSize.cx * nChannels, 4);
    const uint8_t* pSrc = nullptr;
    uint8_t* pDst = pBitmap;
    uint32_t nCurY = fullTargetOffset.y*nIncrementY;
    uint32_t nStartX = fullTargetOffset.x*nIncrementX;
    for (int j = 0; j < clippedTargetSize.cy; j++) {
        // 64 bit offset: images can have more than 4 GiB of pixel data
        pSrc = (uint8_t*)pPixels + (size_t)nPaddedSourceWidth * (nCurY >> 16);
        uint32_t nCurX = nStartX;
        if (nChannels == 3) {
            for (int i = 0; i < clippedTargetSize.cx; i++) {
                uint32_t sx = nCurX >> 16; 
                uint32_t s = sx*3;
                uint32_t d = i*4;
                pDst[d] = pSrc[s];
                pDst[d+1] = pSrc[s+1];
                pDst[d+2] = pSrc[s+2];
                pDst[d+3] = 0xFF;
                nCurX += nIncrementX;
            }
        } else {
            for (int i = 0; i < clippedTargetSize.cx; i++) {
                uint32_t sx = nCurX >> 16; 
                ((uint32_t*)pDst)[i] = ((uint32_t*)pSrc)[sx];
                nCurX += nIncrementX;
            }
        }
        pDst += clippedTargetSize.cx*4;
        nCurY += nIncrementY;
    }
    return pBitmap;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Helper methods for high quality resizing (C++ implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

// Apply filtering in x-direction and rotate
// nSourceWidth: Width of the image in pSource in pixels
// nTargetWidth: Target width of the image after filtering.
// nHeight: Height of the target image in pixels
// nSourceBytesPerPixel: 3 or 4, the target will always have 4 bytes per pixel
// nStartX_FP: 16.16 fixed point number, start of filtering in source image
// nStartY: First row to filter in source image (Offset, not in FP format)
// nIncrementX_FP: 16.16 fixed point number, increment in x-direction in source image
// filter: Filter to apply (in x direction)
// nFilterOffset: Offset into filter (to filter.Indices array)
// pSource: Source image
// Returns the filtered image of size(nHeight, nTargetWidth)
static uint8_t* ApplyFilter(int nSourceWidth, int nTargetWidth, int nHeight,
                          int nSourceBytesPerPixel,
                          int nStartX_FP, int nStartY, int nIncrementX_FP,
                          const FilterKernelBlock& filter,
                          int nFilterOffset,
                          const uint8_t* pSource) {

    uint8_t* pTarget = new(std::nothrow) uint8_t[(size_t)nTargetWidth*4*nHeight];
    if (pTarget == nullptr) return nullptr;

    // width of new image is (after rotation) : nHeight
    // height of new image is (after rotation) : nTargetWidth
    
    int nPaddedSourceWidth = Helpers::DoPadding(nSourceWidth * nSourceBytesPerPixel, 4);
    const uint8_t* pSourcePixelLine = nullptr;
    uint8_t* pTargetPixelLine = nullptr;
    const int FP_05 = 255; // rounding correction because in filter 1.0 is 16383 but we shift by 14 what is a division by 16384
    for (int j = 0; j < nHeight; j++) {
        pSourcePixelLine = ((uint8_t*) pSource) + (size_t)nPaddedSourceWidth * (j + nStartY);
        pTargetPixelLine = pTarget + 4*j;
        uint8_t* pTargetPixel = pTargetPixelLine;
        uint32_t nX = nStartX_FP;
        for (int i = 0; i < nTargetWidth; i++) {
            uint32_t nXSourceInt = nX >> 16;
            FilterKernel* pKernel = filter.Indices[i + nFilterOffset];
            const uint8_t* pSourcePixel = pSourcePixelLine + nSourceBytesPerPixel*(nXSourceInt - pKernel->FilterOffset);
            int nPixelValue1 = 0;
            int nPixelValue2 = 0;
            int nPixelValue3 = 0;
            for (int n = 0; n < pKernel->FilterLen; n++) {
                nPixelValue1 += pKernel->Kernel[n] * pSourcePixel[0];
                nPixelValue2 += pKernel->Kernel[n] * pSourcePixel[1];
                nPixelValue3 += pKernel->Kernel[n] * pSourcePixel[2];
                pSourcePixel += nSourceBytesPerPixel;
            }
            nPixelValue1 = (nPixelValue1 + FP_05) >> 14;
            nPixelValue2 = (nPixelValue2 + FP_05) >> 14;
            nPixelValue3 = (nPixelValue3 + FP_05) >> 14;

            *pTargetPixel++ = (uint8_t)std::max(0, std::min(255, nPixelValue1));
            *pTargetPixel++ = (uint8_t)std::max(0, std::min(255, nPixelValue2));
            *pTargetPixel++ = (uint8_t)std::max(0, std::min(255, nPixelValue3));
            *pTargetPixel++ = 0xFF;
            // rotate: go to next row in target - width of target is nHeight
            pTargetPixel = pTargetPixel - 4 + nHeight*4;
            nX += nIncrementX_FP;
        }
    }

    return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality upsampling (C++ implementation)
// Used with generic CPUs where no SIMD extensions are available.
// This will NOT use linear color space! (it would need INT64 or FLOAT!)
// This WILL still use a Cubic kernel.
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::SampleUp_HQ(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels) {

    // Resizing consists of resize in x direction followed by resize in y direction.
    // To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
    // thus enabling to use the same loop on the rows for both resize directions.
    // The image is correctly orientated again after two rotation/flip operations!

    if (pPixels == nullptr || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return nullptr;
    }
    int nTargetWidth = clippedTargetSize.cx;
    int nTargetHeight = clippedTargetSize.cy;
    int nSourceWidth = sourceSize.cx;
    int nSourceHeight = sourceSize.cy;

    uint32_t nIncrementX = (uint32_t)(65536*(uint32_t)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
    uint32_t nIncrementY = (uint32_t)(65536*(uint32_t)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

    // Caution: This code assumes a upsampling filter kernel of length 4, with a filter offset of 1
    int nFirstY = std::max(0, int((uint32_t)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
    int nLastY = std::min<int>(sourceSize.cy - 1, ((uint32_t)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2);
    int nTempTargetWidth = nLastY - nFirstY + 1;
    int nTempTargetHeight = nTargetWidth;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncrementX*fullTargetOffset.x;
    int nStartY = nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    CResizeFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic, FilterSIMDType_None);
    const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();

    uint8_t* pTemp = ApplyFilter(nSourceWidth, nTempTargetHeight, nTempTargetWidth,
        nChannels, nStartX, nFirstY, nIncrementX,
        kernelsX, nFilterOffsetX, (const uint8_t*)pPixels);
    if (pTemp == nullptr) return nullptr;

    CResizeFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic, FilterSIMDType_None);
    const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

    uint8_t* pBitmap = ApplyFilter(nTempTargetWidth, nTargetHeight, nTargetWidth,
            4, nStartY, 0, nIncrementY,
            kernelsY, nFilterOffsetY, pTemp);

    delete[] pTemp;

    return pBitmap;
}


/////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (C++ implementation)
// Used with generic CPUs where no SIMD extensions are available.
// This will NOT use linear color space! (it would need INT64 or FLOAT!)
// This WILL still use Hermite/Mitchell/Catrom/Lanczos2 kernel as specified.
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::SampleDown_HQ(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter) {
    // Resizing consists of resize in x direction followed by resize in y direction.
    // To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
    // thus enabling to use the same loop on the rows for both resize directions.
    // The image is correctly orientated again after two rotation/flip operations!

    if (pPixels == nullptr || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return nullptr;
    }
    CResizeFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter, FilterSIMDType_None);
    const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();
    CResizeFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter, FilterSIMDType_None);
    const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

    uint32_t nIncrementX = ((uint32_t)sourceSize.cx << 16)/fullTargetSize.cx + 1;
    uint32_t nIncrementY = ((uint32_t)sourceSize.cy << 16)/fullTargetSize.cy + 1;

    int nIncOffsetX = (nIncrementX - 65536) >> 1;
    int nIncOffsetY = (nIncrementY - 65536) >> 1;
    int nFirstY = (uint32_t)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
    nFirstY = std::max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
    int nLastY  = (uint32_t)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
    FilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
    nLastY  = std::min<int>(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
    int nTempTargetWidth = nLastY - nFirstY + 1;
    int nTempTargetHeight = clippedTargetSize.cx;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x;
    int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    uint8_t* pTemp = ApplyFilter(sourceSize.cx, nTempTargetHeight, nTempTargetWidth,
        nChannels, nStartX, nFirstY, nIncrementX,
        kernelsX, nFilterOffsetX, (const uint8_t*)pPixels);
    if (pTemp == nullptr) return nullptr;

    uint8_t* pBitmap = ApplyFilter(nTempTargetWidth, clippedTargetSize.cy, clippedTargetSize.cx,
            4, nStartY, 0, nIncrementY,
            kernelsY, nFilterOffsetY, pTemp);

    delete[] pTemp;

    return pBitmap;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (Helpers for SSE and MMX implementation, Rotate/RotateToBitmap are only used by SSE/MMX)
/////////////////////////////////////////////////////////////////////////////////////////////

// Rotates a line of 'simdPixelsPerRegister' pixels from source to targt
inline static const float* RotateLine(const float* pSource, float* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {
    for (int i = 0; i < simdPixelsPerRegister - 1; i++)
    {
        *pTarget = *pSource++; pTarget += nIncTargetLine;
    }
    *pTarget = *pSource++;

    return pSource;
}

inline static const float* RotateLineToBitmap_1(const float* pSource, uint8_t* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

    for (int i = 0; i < simdPixelsPerRegister - 1; i++)
    {
        *((uint8_t*)pTarget) = LinRGB12_sRGB8[(int)(*pSource)]; pSource++;  pTarget += nIncTargetLine;
    }
    *((uint8_t*)pTarget) = LinRGB12_sRGB8[(int)(*pSource)]; pSource++;

    return pSource;
}

inline static const float* RotateLineToBitmap(const float* pSource, uint8_t* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

    for (int i = 0; i < simdPixelsPerRegister - 1; i++)
    {
        *pTarget = LinRGB12_sRGB8[(int)(*pSource++)]; pTarget += nIncTargetLine;
    }
    *pTarget = LinRGB12_sRGB8[(int)(*pSource++)];

    return pSource;
}

// Rotate a block in a CXMMImage. Blockwise rotation is needed because with normal
// rotation, trashing occurs, making rotation a very slow operation.
// The input format is what the ApplyFilter_SSE() method outputs:
// RRRRRRRRGGGGGGGGBBBBBBBB... (blocks of 'simdPixelsPerRegister' pixels of a channel).
// After rotation, the format is line interleaved again:
// RRRRRRRRRRR...
// GGGGGGGGGGG...
// BBBBBBBBBBB...
// nTargetPaddedWidth: padded width (line size in floats) of the target image
static void RotateBlock(const float* pSrc, float* pTgt, int nWidth, int nTargetPaddedWidth,
                        int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
                        int simdPixelsPerRegister) {
    int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
    int nIncTargetChannel = nTargetPaddedWidth;
    int nIncTargetLine = nIncTargetChannel * 3;
    int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
    const float* pSource = pSrc + (size_t)nPaddedWidth * 3 * nYStart + nXStart * 3;
    float* pTarget = pTgt + (size_t)nTargetPaddedWidth * 3 * nXStart + nYStart;
    float* pStartYPtr = pTarget;
    int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
    int nTargetIncrement = ((simdPixelsPerRegister - 1) * nIncTargetLine) + nIncTargetChannel;

    for (int i = 0; i < nBlockHeight; i++) {
        for (int j = 0; j < nLoopX; j++) {
            pSource = RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget += nIncTargetChannel;
            pSource =  RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget += nIncTargetChannel;
            pSource =  RotateLine(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget += nTargetIncrement;
        }
        pStartYPtr++;
        pTarget = pStartYPtr;
        pSource += nIncSource;
    }
}

// Same as above, directly rotates into a 32 bpp bitmap
static void RotateBlockToBitmap(const float* pSrc, uint8_t* pTgt, int nWidth, int nHeight,
                             int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
                             int simdPixelsPerRegister) {
    int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
    //int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
    int nIncTargetLine = nHeight * 4;
    int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
    const float* pSource = pSrc + (size_t)nPaddedWidth * 3 * nYStart + nXStart * 3;
    uint8_t* pTarget = pTgt + (size_t)nHeight * 4 * nXStart + nYStart * 4;
    uint8_t* pStartYPtr = pTarget;
    int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
    int nTargetIncrement = simdPixelsPerRegister * nIncTargetLine - 2;

    for (int i = 0; i < nBlockHeight; i++) {
        for (int j = 0; j < nLoopX; j++) {
            pSource = RotateLineToBitmap_1(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget++;
            pSource = RotateLineToBitmap(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget++;
            pSource = RotateLineToBitmap(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget += nTargetIncrement;
        }
        pStartYPtr += 4;
        pTarget = pStartYPtr;
        pSource += nIncSource;
    }
}

// RotateFlip the source image by 90 deg and return rotated image
// RotateFlip is invertible: img = RotateFlip(RotateFlip(img))
static CXMMImage* Rotate(const CXMMImage* pSourceImg, int simdPixelsPerRegister) {
    // the target is the source of the next filter pass, thus avoid cache aliasing
    CXMMImage* targetImage = new CXMMImage(pSourceImg->GetHeight(), pSourceImg->GetWidth(), true, simdPixelsPerRegister, true);
    if (targetImage->AlignedPtr() == nullptr) {
        delete targetImage;
        return nullptr;
    }
    const float* pSource = (const float*) pSourceImg->AlignedPtr();
    float* pTarget = (float*) targetImage->AlignedPtr();

    const int cnBlockSize = 32;
    int nX = 0, nY = 0;
    while (nY < pSourceImg->GetHeight()) {
        nX = 0;
        while (nX < pSourceImg->GetWidth()) {
            RotateBlock(pSource, pTarget, pSourceImg->GetWidth(), targetImage->GetPaddedWidth(),
                nX, nY, 
                std::min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX), // !! here we need to use the padded width
                std::min(cnBlockSize, pSourceImg->GetHeight() - nY),
                simdPixelsPerRegister);
            nX += cnBlockSize;
        }
        nY += cnBlockSize;
    }

    // zero the padding columns, they are not written above but read by the subsequent filter pass
    // (up to the next multiple of simdPixelsPerRegister, the columns beyond are never read)
    int nPadColumns = Helpers::DoPadding(targetImage->GetWidth(), simdPixelsPerRegister) - targetImage->GetWidth();
    if (nPadColumns > 0) {
        float* pRow = pTarget + targetImage->GetWidth();
        for (int i = 0; i < 3 * targetImage->GetPaddedHeight(); i++) {
            memset(pRow, 0, nPadColumns * sizeof(float));
            pRow += targetImage->GetPaddedWidth();
        }
    }

    return targetImage;
}

// RotateFlip the source image by 90 deg and return rotated image as 32 bpp bitmap
static void* RotateToBitmap(const CXMMImage* pSourceImg, int simdPixelsPerRegister, uint8_t* pTarget = nullptr) {

    const float* pSource = (const float*) pSourceImg->AlignedPtr();
    if (pTarget == nullptr) {
        pTarget = new(std::nothrow) uint8_t[(size_t)pSourceImg->GetHeight() * 4 * Helpers::DoPadding(pSourceImg->GetWidth(), simdPixelsPerRegister)];
        if (pTarget == nullptr) return nullptr;
    }

    const int cnBlockSize = 32;
    int nX = 0, nY = 0;
    while (nY < pSourceImg->GetHeight()) {
        nX = 0;
        while (nX < pSourceImg->GetWidth()) {
            RotateBlockToBitmap(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
                nX, nY, 
                std::min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX),  // !! here we need to use the padded width
                std::min(cnBlockSize, pSourceImg->GetHeight() - nY),
                simdPixelsPerRegister);

            nX += cnBlockSize;
        }
        nY += cnBlockSize;
    }

    return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality filtering (SSE implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

// Apply filter in y direction in SSE
// No inline assembly is supported in 64 bit mode, thus intrinsics are used instead.
// nSourceHeight: Height of source image, only here to match interface of C++ implementation
// nTargetHeight: Height of target image after resampling
// nWidth: Width of source image
// nStartY_FP: 16.16 fixed point number, denoting the y-start subpixel coordinate
// nStartX: Start of filtering in x-direction (not an FP number)
// nIncrementY_FP: 16.16 fixed point number, denoting the increment for the y coordinates
// filter: filter to apply
// nFilterOffset: Offset into filter (to filter.Indices array)
// pSourceImg: Source image
// The filter is applied to 'nTargetHeight' rows of the input image at positions
// nStartY_FP, nStartY_FP+nIncrementY_FP, ... 
// In X direction, the filter is applied starting from column /nStartX\ to (including) \nStartX+nWidth-1/
// where the /\ symbol denotes padding to lower 8 pixel boundary and \/ padding to the upper 8 pixel
// boundary.
static CXMMImage* ApplyFilter_SSE(int nSourceHeight, int nTargetHeight, int nWidth,
    int nStartY_FP, int nStartX, int nIncrementY_FP,
    const XMMFilterKernelBlock& filter,
    int nFilterOffset, const CXMMImage* pSourceImg, bool bRoundResult) {

    int nStartXAligned = nStartX & ~3;
    int nEndXAligned = (nStartX + nWidth + 3) & ~3;
    CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 4);
    if (tempImage->AlignedPtr() == nullptr) {
        delete tempImage;
        return nullptr;
    }

    int nCurY = nStartY_FP;
    int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(float);
    int nRowLenBytes = nChannelLenBytes * 3;
    int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 2;
    const uint8_t* pSourceStart = (const uint8_t*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(float);
    XMMFilterKernel** pKernelIndexStart = filter.Indices;

    alignas(16) float XMM255[4] = {4095.0,  4095.0, 4095.0, 4095.0};

    __m128 xmm0 = _mm_setzero_ps();
    __m128 xmm1 = *((__m128*)XMM255);
    __m128 xmm2;
    __m128 xmm3;
    __m128 xmm4 = _mm_setzero_ps();
    __m128 xmm5 = _mm_setzero_ps();
    __m128 xmm6 = _mm_setzero_ps();
    __m128 xmm7;

    __m128* pDestination = (__m128*)tempImage->AlignedPtr();

    for (int y = 0; y < nTargetHeight; y++) {
        uint32_t nCurYInt = (uint32_t)nCurY >> 16; // integer part of Y
        int filterIndex = y + nFilterOffset;
        XMMFilterKernel* pKernel = pKernelIndexStart[filterIndex];
        int filterLen = pKernel->FilterLen;
        int filterOffset = pKernel->FilterOffset;
        const __m128* pFilterStart = (__m128*)&(pKernel->Kernel);
        const __m128* pSourceRow = (const __m128*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);

        for (int x = 0; x < nNumberOfBlocksX; x++) {
            const __m128* pSource = pSourceRow;
            const __m128* pFilter = pFilterStart;
            xmm4 = _mm_setzero_ps();
            xmm5 = _mm_setzero_ps();
            xmm6 = _mm_setzero_ps();
            for (int i = 0; i < filterLen; i++) {
                xmm7 = *pFilter;        // filter range was adjusted to output maximum of 1.0]

                // the pixel data RED channel
                xmm2 = *pSource;
                xmm2 = _mm_mul_ps(xmm2, xmm7);
                xmm4 = _mm_add_ps(xmm4, xmm2);
                pSource = (__m128*)((uint8_t*)pSource + nChannelLenBytes);

                // the pixel data GREEN channel
                xmm3 = *pSource;
                xmm3 = _mm_mul_ps(xmm3, xmm7);
                xmm5 = _mm_add_ps(xmm5, xmm3);
                pSource = (__m128*)((uint8_t*)pSource + nChannelLenBytes);

                // the pixel data BLUE channel
                xmm2 = *pSource;
                xmm2 = _mm_mul_ps(xmm2, xmm7);
                xmm6 = _mm_add_ps(xmm6, xmm2);
                pSource = (__m128*)((uint8_t*)pSource + nChannelLenBytes);

                pFilter++;
            }

            if (bRoundResult == true) {
                // limit to range <=255 (in xmm1)
                xmm4 = _mm_min_ps(xmm4, xmm1);
                xmm5 = _mm_min_ps(xmm5, xmm1);
                xmm6 = _mm_min_ps(xmm6, xmm1);

                // limit to range >=0 (in xmm0)
                xmm4 = _mm_max_ps(xmm4, xmm0);
                xmm5 = _mm_max_ps(xmm5, xmm0);
                xmm6 = _mm_max_ps(xmm6, xmm0);

                // round to nearest integer
                xmm4 = _mm_round_ps(xmm4, _MM_FROUND_TO_NEAREST_INT);
                xmm5 = _mm_round_ps(xmm5, _MM_FROUND_TO_NEAREST_INT);
                xmm6 = _mm_round_ps(xmm6, _MM_FROUND_TO_NEAREST_INT);
                }

            // store result in blocks
            *pDestination++ = xmm4;
            *pDestination++ = xmm5;
            *pDestination++ = xmm6;

            pSourceRow++;
        };

        nCurY += nIncrementY_FP;
    };

    return tempImage;
}


/////////////////////////////////////////////////////////////////////////////////////////////
// High quality down- and up-sampling (SIMD implementation)
/////////////////////////////////////////////////////////////////////////////////////////////

void* SampleDown_HQ_MMX_SSE_Core(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
                                 DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, bool bSSE, uint8_t* pTarget) {

    CAutoXMMFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
    const XMMFilterKernelBlock& kernelsY = filterY.Kernels();
    CAutoXMMFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
    const XMMFilterKernelBlock& kernelsX = filterX.Kernels();

    uint32_t nIncrementX = ((uint32_t)sourceSize.cx << 16)/fullTargetSize.cx + 1;
    uint32_t nIncrementY = ((uint32_t)sourceSize.cy << 16)/fullTargetSize.cy + 1;

    int nIncOffsetX = (nIncrementX - 65536) >> 1;
    int nIncOffsetY = (nIncrementY - 65536) >> 1;
    int nFirstX = (uint32_t)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
    nFirstX = std::max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
    int nLastX  = (uint32_t)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
    XMMFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
    nLastX  = std::min<int>(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
    int nFirstY = (uint32_t)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
    nFirstY = std::max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
    int nLastY  = (uint32_t)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
    XMMFilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
    nLastY  = std::min<int>(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536*nFirstX;
    int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    // Create
    CXMMImage* pImage1 = new CXMMImage(sourceSize.cx, sourceSize.cy, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 4);
    if (pImage1->AlignedPtr() == nullptr) {
        delete pImage1;
        return nullptr;
    }

    // Resize Y
    CXMMImage* pImage2 = ApplyFilter_SSE(pImage1->GetHeight(), clippedTargetSize.cy, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == nullptr) return nullptr;

    // Rotate
    CXMMImage* pImage3 = Rotate(pImage2, 4);
    delete pImage2;
    if (pImage3 == nullptr) return nullptr;

    // Resize Y again
    CXMMImage* pImage4 = ApplyFilter_SSE(pImage3->GetHeight(), clippedTargetSize.cx, clippedTargetSize.cy, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == nullptr) return nullptr;

    // Rotate back
    void* pTargetBitmap = RotateToBitmap(pImage4, 4, pTarget);

    delete pImage4;
    return pTargetBitmap;
}

void* SampleUp_HQ_MMX_SSE_Core(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, bool bSSE, uint8_t* pTarget) {
    int nTargetWidth = clippedTargetSize.cx;
    int nTargetHeight = clippedTargetSize.cy;
    int nSourceWidth = sourceSize.cx;
    int nSourceHeight = sourceSize.cy;

    uint32_t nIncrementX = (uint32_t)(65536*(uint32_t)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
    uint32_t nIncrementY = (uint32_t)(65536*(uint32_t)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

    int nFirstX = std::max(0, int((uint32_t)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
    int nLastX = std::min<int>(sourceSize.cx - 1, ((uint32_t)(nIncrementX*(fullTargetOffset.x + nTargetWidth - 1)) >> 16) + 2);
    int nFirstY = std::max(0, int((uint32_t)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
    int nLastY = std::min<int>(sourceSize.cy - 1, ((uint32_t)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2);
    //int nFirstTargetWidth = nLastX - nFirstX + 1;
    //int nFirstTargetHeight = nTargetHeight;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncrementX*fullTargetOffset.x - 65536*nFirstX;
    int nStartY = nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    CAutoXMMFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic);
    const XMMFilterKernelBlock& kernelsY = filterY.Kernels();

    CAutoXMMFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic);
    const XMMFilterKernelBlock& kernelsX = filterX.Kernels();

    // Resize Y
    CXMMImage* pImage1 = new CXMMImage(nSourceWidth, nSourceHeight, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 4);
    if (pImage1->AlignedPtr() == nullptr) {
        delete pImage1;
        return nullptr;
    }
    CXMMImage* pImage2 = ApplyFilter_SSE(pImage1->GetHeight(), nTargetHeight, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == nullptr) return nullptr;
    CXMMImage* pImage3 = Rotate(pImage2, 4);
    delete pImage2;
    if (pImage3 == nullptr) return nullptr;
    CXMMImage* pImage4 = ApplyFilter_SSE(pImage3->GetHeight(), nTargetWidth, nTargetHeight, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == nullptr) return nullptr;
    void* pTargetBitmap = RotateToBitmap(pImage4, 4, pTarget);
    delete pImage4;
    
    return pTargetBitmap;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// AVX2 implementation of high quality down- and up-sampling
//
// Each thread of the pool processes one slice of target lines (CRequestUpDownSampling::ProcessSlice):
// SampleSlice_HQ_AVX     splits the slice into tiles of target columns if needed (ring buffer must fit in L2)
// SampleTile_HQ_AVX      per tile: source lines are converted once into a ring buffer (CXMMImage::ConvertLine_AVX),
//                        then in strips of target lines:
//   FilterLine_AVX       vertical filter from the ring buffer, block layout
//   Rotate_AVX           8x8 transposes into line interleaved layout (columns become lines)
//   FilterToBitmap_AVX      horizontal filter, conversion to sRGB 8 bit and transpose, written directly to the target bitmap
// All intermediate buffers are per thread scratch buffers (see XMMImage.cpp). The results are bit identical to the
// former stripwise implementation (see BENCHMARKS.md). The SSE/MMX path above is unchanged.
/////////////////////////////////////////////////////////////////////////////////////////////

// Vertical filter for one target line (AVX). The source lines are passed as pointers (into the ring buffer of
// SampleSlice_HQ_AVX). The FMAs are done in kernel order, as in the former stripwise implementation (same results).
// ppLines: nFilterLen pointers to the first channel of the source lines, the other two channels follow at nChannelLen floats
// The result is written in block layout (8 x channel 0, 8 x channel 1, 8 x channel 2, next 8 pixels, ...),
// returns the destination pointer after the line.
static __m256* FilterLine_AVX(const float* const* ppLines, int nFilterLen, const float* pKernel,
                              int nChannelLen, int nNumberOfBlocksX, __m256* pDestination) {
    for (int x = 0; x < nNumberOfBlocksX; x++) {
        __m256 ymmR = _mm256_setzero_ps();
        __m256 ymmG = _mm256_setzero_ps();
        __m256 ymmB = _mm256_setzero_ps();

        for (int i = 0; i < nFilterLen; i++) {
            __m256 ymmKernel = _mm256_broadcast_ss(pKernel + i);
            const float* pR = ppLines[i] + x * 8;
            ymmR = _mm256_fmadd_ps(_mm256_load_ps(pR), ymmKernel, ymmR);
            ymmG = _mm256_fmadd_ps(_mm256_load_ps(pR + nChannelLen), ymmKernel, ymmG);
            ymmB = _mm256_fmadd_ps(_mm256_load_ps(pR + 2 * nChannelLen), ymmKernel, ymmB);
        }

        *pDestination++ = ymmR;
        *pDestination++ = ymmG;
        *pDestination++ = ymmB;
    }
    return pDestination;
}

// LinRGB12_sRGB8 widened to 32 bit, for lookups with _mm256_i32gather_epi32
static const uint32_t* GetLinRGB12_sRGB8_32() {
    static const struct CLUT32 {
        uint32_t Values[4096];
        CLUT32() {
            for (int i = 0; i < 4096; i++) {
                Values[i] = LinRGB12_sRGB8[i];
            }
        }
    } lut;
    return lut.Values;
}

// Transposes the 8x8 matrix of 32 bit elements in r (r[i] is row i)
static inline void Transpose8x8_AVX(__m256 r[8]) {
    __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
    __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
    __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
    __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
    __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]);
    __m256 t5 = _mm256_unpackhi_ps(r[4], r[5]);
    __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]);
    __m256 t7 = _mm256_unpackhi_ps(r[6], r[7]);
    __m256 s0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 s6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 s7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
    r[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
    r[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
    r[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
    r[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
    r[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
    r[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
    r[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
    r[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
}

// AVX version of Rotate(pSourceImg, 8), same result: rotates a vertically filtered strip (block layout as written
// by FilterLine_AVX: per line 8 x channel 0, 8 x channel 1, 8 x channel 2, next 8 pixels, ...) into the line
// interleaved layout read by FilterToBitmap_AVX. Blocks of 8 lines x 8 pixels are transposed in registers.
// The padding columns of the target (lines missing in the last block of 8) are set to zero.
static CXMMImage* Rotate_AVX(const CXMMImage* pSourceImg) {
    CXMMImage* targetImage = new CXMMImage(pSourceImg->GetHeight(), pSourceImg->GetWidth(), true, 8, true);
    if (targetImage->AlignedPtr() == nullptr) {
        delete targetImage;
        return nullptr;
    }
    const float* pSource = (const float*)pSourceImg->AlignedPtr();
    const int nSourceLineLen = 3 * pSourceImg->GetPaddedWidth();
    const int nTargetChannelLen = targetImage->GetPaddedWidth();
    const int nNumberOfBlocksX = pSourceImg->GetPaddedWidth() >> 3;
    const int nLines = pSourceImg->GetHeight();
    __m256 r[8];

    // column blocks in the outer loop: the 24 target lines of a column block are written sequentially
    for (int nBlockX = 0; nBlockX < nNumberOfBlocksX; nBlockX++) {
        for (int nY = 0; nY < nLines; nY += 8) {
            int nBlockLines = std::min(8, nLines - nY);
            for (int nChannel = 0; nChannel < 3; nChannel++) {
                const float* pBlock = pSource + (size_t)nY * nSourceLineLen + nBlockX * 24 + nChannel * 8;
                for (int k = 0; k < nBlockLines; k++) {
                    r[k] = _mm256_load_ps(pBlock + (size_t)k * nSourceLineLen);
                }
                for (int k = nBlockLines; k < 8; k++) {
                    r[k] = _mm256_setzero_ps();
                }
                Transpose8x8_AVX(r);
                for (int k = 0; k < 8; k++) {
                    _mm256_store_ps(targetImage->GetLine(nBlockX * 8 + k) + nChannel * nTargetChannelLen + nY, r[k]);
                }
            }
        }
    }

    return targetImage;
}

// Horizontal filter on the rotated strip directly into the target bitmap (AVX). Replaces the former horizontal
// filter pass with rounding followed by RotateToBitmap, with the same arithmetic, thus same results.
// pSourceImg: rotated strip, line j holds source column j (relative to the first converted column), nStripLines values
// per channel. For each step, 8 target columns x 8 strip lines are filtered in registers, converted to sRGB 8 bit
// and transposed, then 8 pixels are written per target line.
// nStartX_FP, nIncrementX_FP: 16.16 fixed point position of the first target column and increment
// nFilterOffset: index of the first target column in filter.Indices
// pTarget: first pixel of the strip in the target bitmap (nTargetWidth pixels are written per line),
// nTargetStride: pixels per line of the target bitmap
static void FilterToBitmap_AVX(const CXMMImage* pSourceImg, int nTargetWidth, int nStripLines,
    int nStartX_FP, int nIncrementX_FP, const AVXFilterKernelBlock& filter, int nFilterOffset,
    uint8_t* pTarget, int nTargetStride) {

    const uint32_t* pLUT = GetLinRGB12_sRGB8_32();
    const int nChannelLen = pSourceImg->GetPaddedWidth();
    const int nLineLen = 3 * nChannelLen;
    const int nNumberOfBlocks = (nStripLines + 7) >> 3;
    const __m256 ymmZero = _mm256_setzero_ps();
    const __m256 ymmMax = _mm256_set1_ps(4095.0f);
    const __m256i ymmAlpha = _mm256_set1_epi32((int)ALPHA_OPAQUE);

    const float* pFirstLine[8];
    const AVXFilterKernel* pKernels[8];
    __m256 ymmPixels[8];

    for (int nX = 0; nX < nTargetWidth; nX += 8) {
        int nColumns = std::min(8, nTargetWidth - nX);
        for (int c = 0; c < nColumns; c++) {
            uint32_t nCurX = (uint32_t)nStartX_FP + (uint32_t)(nX + c) * (uint32_t)nIncrementX_FP;
            pKernels[c] = filter.Indices[nX + c + nFilterOffset];
            pFirstLine[c] = pSourceImg->GetLine((int)(nCurX >> 16) - pKernels[c]->FilterOffset);
        }
        for (int c = nColumns; c < 8; c++) {
            ymmPixels[c] = _mm256_setzero_ps();
        }

        for (int nBlock = 0; nBlock < nNumberOfBlocks; nBlock++) {
            // filter 8 strip lines (vector lanes) for each of the target columns
            for (int c = 0; c < nColumns; c++) {
                const float* pR = pFirstLine[c] + nBlock * 8;
                const float* pKernel = pKernels[c]->Kernel;
                int nFilterLen = pKernels[c]->FilterLen;
                __m256 ymmR = _mm256_setzero_ps();
                __m256 ymmG = _mm256_setzero_ps();
                __m256 ymmB = _mm256_setzero_ps();
                for (int i = 0; i < nFilterLen; i++) {
                    __m256 ymmKernel = _mm256_broadcast_ss(pKernel + i);
                    ymmR = _mm256_fmadd_ps(_mm256_load_ps(pR), ymmKernel, ymmR);
                    ymmG = _mm256_fmadd_ps(_mm256_load_ps(pR + nChannelLen), ymmKernel, ymmG);
                    ymmB = _mm256_fmadd_ps(_mm256_load_ps(pR + 2 * nChannelLen), ymmKernel, ymmB);
                    pR += nLineLen;
                }
                // Clamping [0, 4095] and rounding, then 12 bit linear -> 8 bit sRGB
                ymmR = _mm256_round_ps(_mm256_min_ps(_mm256_max_ps(ymmR, ymmZero), ymmMax), _MM_FROUND_TO_NEAREST_INT);
                ymmG = _mm256_round_ps(_mm256_min_ps(_mm256_max_ps(ymmG, ymmZero), ymmMax), _MM_FROUND_TO_NEAREST_INT);
                ymmB = _mm256_round_ps(_mm256_min_ps(_mm256_max_ps(ymmB, ymmZero), ymmMax), _MM_FROUND_TO_NEAREST_INT);
                // (channel order of the source is B, G, R, the variable names are historical)
                __m256i ymmC0 = _mm256_i32gather_epi32((const int*)pLUT, _mm256_cvtps_epi32(ymmR), 4);
                __m256i ymmC1 = _mm256_i32gather_epi32((const int*)pLUT, _mm256_cvtps_epi32(ymmG), 4);
                __m256i ymmC2 = _mm256_i32gather_epi32((const int*)pLUT, _mm256_cvtps_epi32(ymmB), 4);
                __m256i ymmPixel = _mm256_or_si256(_mm256_or_si256(ymmC0, _mm256_slli_epi32(ymmC1, 8)),
                                                   _mm256_or_si256(_mm256_slli_epi32(ymmC2, 16), ymmAlpha));
                ymmPixels[c] = _mm256_castsi256_ps(ymmPixel);
            }

            // ymmPixels[c] holds column c for 8 lines -> transpose to 8 lines of 8 columns
            Transpose8x8_AVX(ymmPixels);
            int nLines = std::min(8, nStripLines - nBlock * 8);
            uint8_t* pDst = pTarget + ((size_t)nBlock * 8 * nTargetStride + nX) * 4;
            if (nColumns == 8) {
                for (int nLine = 0; nLine < nLines; nLine++) {
                    _mm256_storeu_si256((__m256i*)(pDst + (size_t)nLine * nTargetStride * 4), _mm256_castps_si256(ymmPixels[nLine]));
                }
            } else {
                const __m256i ymmMask = _mm256_cmpgt_epi32(_mm256_set1_epi32(nColumns), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
                for (int nLine = 0; nLine < nLines; nLine++) {
                    _mm256_maskstore_epi32((int*)(pDst + (size_t)nLine * nTargetStride * 4), ymmMask, _mm256_castps_si256(ymmPixels[nLine]));
                }
            }
            for (int c = nColumns; c < 8; c++) {
                ymmPixels[c] = _mm256_setzero_ps();
            }
        }
    }
}

// Size budget for each of the intermediate images of one strip (vertically filtered, rotated), chosen such that
// they stay in the L2 cache as far as possible. A strip has at least 8 lines.
constexpr int AVX_STRIP_BUDGET_BYTES = 256 * 1024;

// Cache sizes of the CPU in bytes: L2 per core, L3 shared by all cores. Queried once from the operating system,
// defaults if unknown.
struct CCacheSizes {
    size_t nL2 = 256 * 1024;
    size_t nL3 = 8 * 1024 * 1024;
};

static const CCacheSizes& GetCacheSizes() {
    static const CCacheSizes sizes = [] {
        CCacheSizes c;
        long nL2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
        long nL3 = sysconf(_SC_LEVEL3_CACHE_SIZE);
        if (nL2 > 0) c.nL2 = (size_t)nL2;
        if (nL3 > 0) c.nL3 = (size_t)nL3;
        return c;
    }();
    return sizes;
}

// Longest vertical kernel for the target lines nFirstLine to nFirstLine + nLines - 1 (= lines of the ring buffer)
static int MaxFilterLen(const AVXFilterKernelBlock& kernels, int nFirstLine, int nLines) {
    int nMaxLen = 1;
    for (int y = 0; y < nLines; y++) {
        nMaxLen = std::max(nMaxLen, kernels.Indices[nFirstLine + y]->FilterLen);
    }
    return nMaxLen;
}

// Resamples one tile of the target image (lines fullTargetOffset.y to fullTargetOffset.y + clippedTargetSize.cy - 1,
// columns fullTargetOffset.x to fullTargetOffset.x + clippedTargetSize.cx - 1) on the calling thread (AVX).
// Downsampling with eFilter, or upsampling for eFilter == Filter_Upsampling_Bicubic.
// pTarget points to the first pixel of the tile in the target bitmap, which has nTargetStride pixels per line.
// Each source line is converted to linear float only once, into a ring buffer holding the lines needed by the
// vertical filter. The vertically filtered lines are processed in strips: rotate, then filter horizontally
// directly into the target bitmap.
static bool SampleTile_HQ_AVX(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter,
    const AVXFilterKernelBlock& kernelsX, const AVXFilterKernelBlock& kernelsY, uint8_t* pTarget, int nTargetStride) {

    bool bUpsampling = (eFilter == Filter_Upsampling_Bicubic);

    // 16.16 fixed point increments and the needed source columns
    uint32_t nIncrementX, nIncrementY;
    int nIncOffsetX, nIncOffsetY, nFirstX, nLastX;
    if (bUpsampling) {
        nIncrementX = (uint32_t)(65536 * (uint32_t)(sourceSize.cx - 1) / (fullTargetSize.cx - 1));
        nIncrementY = (uint32_t)(65536 * (uint32_t)(sourceSize.cy - 1) / (fullTargetSize.cy - 1));
        nIncOffsetX = 0;
        nIncOffsetY = 0;
        nFirstX = std::max(0, int((uint32_t)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
        nLastX  = std::min<int>(sourceSize.cx - 1, ((uint32_t)(nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16) + 2);
    } else {
        nIncrementX = ((uint32_t)sourceSize.cx << 16) / fullTargetSize.cx + 1;
        nIncrementY = ((uint32_t)sourceSize.cy << 16) / fullTargetSize.cy + 1;
        nIncOffsetX = (nIncrementX - 65536) >> 1;
        nIncOffsetY = (nIncrementY - 65536) >> 1;
        nFirstX = (uint32_t)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
        nFirstX = std::max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
        nLastX = (uint32_t)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
        AVXFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
        nLastX = std::min<int>(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
    }
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536 * nFirstX;
    int nSectionWidth = nLastX - nFirstX + 1;
    int nPaddedSectionWidth = Helpers::DoPadding(nSectionWidth, 8);

    // Ring buffer of converted source lines, as large as the longest vertical kernel of the slice.
    // Source line n is kept in ring line n % nRingLines.
    int nRingLines = MaxFilterLen(kernelsY, fullTargetOffset.y, clippedTargetSize.cy);
    CXMMImage ring(nSectionWidth, nRingLines, false, 8, true);
    if (ring.AlignedPtr() == nullptr) {
        return false;
    }
    int nSrcLineWidthPadded = Helpers::DoPadding(sourceSize.cx * nChannels, 4);
    const uint8_t* pSrcFirstColumn = (const uint8_t*)pPixels + (long long)nFirstX * nChannels;
    int nNextLine = 0; // next source line to convert, all lines before are converted (the last nRingLines are in the ring)
    const float* ppLines[MAX_FILTER_LEN];

    int nBytesPerLine = 3 * sizeof(float) * std::max(nPaddedSectionWidth, Helpers::DoPadding(clippedTargetSize.cx, 8));
    int nStripHeight = std::max(8, (AVX_STRIP_BUDGET_BYTES / nBytesPerLine) & ~7);

    for (int nStripStart = 0; nStripStart < clippedTargetSize.cy; nStripStart += nStripHeight) {
        int nStripSize = std::min(nStripHeight, clippedTargetSize.cy - nStripStart);

        // Resize Y
        CXMMImage* pImage2 = new CXMMImage(nPaddedSectionWidth, nStripSize, 8);
        if (pImage2->AlignedPtr() == nullptr) {
            delete pImage2;
            return false;
        }
        __m256* pDestination = (__m256*)pImage2->AlignedPtr();
        for (int y = nStripStart; y < nStripStart + nStripSize; y++) {
            int nTargetY = fullTargetOffset.y + y;
            const AVXFilterKernel* pKernel = kernelsY.Indices[nTargetY];
            int nFirstLine = (int)((uint32_t)(nIncOffsetY + nIncrementY*nTargetY) >> 16) - pKernel->FilterOffset;
            int nEndLine = nFirstLine + pKernel->FilterLen;
            // the first lines of the kernel must still be in the ring (kernel start lines never decrease)
            if (nFirstLine < 0 || nEndLine > sourceSize.cy || nFirstLine < nNextLine - nRingLines) {
                assert(false);
                delete pImage2;
                return false;
            }
            nNextLine = std::max(nNextLine, nFirstLine);
            while (nNextLine < nEndLine) {
                ring.ConvertLine_AVX(nNextLine % nRingLines, pSrcFirstColumn + (long long)nNextLine * nSrcLineWidthPadded, nChannels, 8);
                nNextLine++;
            }
            for (int i = 0; i < pKernel->FilterLen; i++) {
                ppLines[i] = ring.GetLine((nFirstLine + i) % nRingLines);
            }
            pDestination = FilterLine_AVX(ppLines, pKernel->FilterLen, pKernel->Kernel, ring.GetPaddedWidth(),
                                          nPaddedSectionWidth >> 3, pDestination);
        }

        // Rotate
        CXMMImage* pImage3 = Rotate_AVX(pImage2);
        delete pImage2;
        if (pImage3 == nullptr) return false;

        // Resize X directly into the target bitmap
        FilterToBitmap_AVX(pImage3, clippedTargetSize.cx, nStripSize, nStartX, nIncrementX, kernelsX, fullTargetOffset.x,
                        pTarget + (size_t)nTargetStride * 4 * nStripStart, nTargetStride);
        delete pImage3;
    }

    return true;
}

// Resamples one slice of the target image (lines fullTargetOffset.y to fullTargetOffset.y + clippedTargetSize.cy - 1)
// on the calling thread (AVX), see SampleTile_HQ_AVX. pTarget points to the first line of the slice in the target bitmap.
// For strong downsampling (long vertical kernels) the ring buffer for the full width would not fit in the cache,
// then the slice is split into tiles of target columns. The source columns at the tile borders are converted
// and filtered vertically twice (overlap of the horizontal kernel), the results are identical.
// Tile width: the ring buffer should use at most half of the L2 cache. For very strong downsampling this would need
// tiles narrower than MIN_L2_TILE_WIDTH, where the overlap gets expensive. Then the ring buffer gets half of the
// L3 share of a thread instead (L3 / threads / 2), with tiles between MIN_TILE_WIDTH and MIN_L2_TILE_WIDTH.
// Measured on i7-10700K, 40000x30591 -> 1478x1130, 8 threads: tile width 64: 867 ms, 24 (chosen): 376 ms.
static bool SampleSlice_HQ_AVX(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, uint8_t* pTarget) {

    CAutoAVXFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
    const AVXFilterKernelBlock& kernelsY = filterY.Kernels();
    CAutoAVXFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
    const AVXFilterKernelBlock& kernelsX = filterX.Kernels();

    const int MIN_L2_TILE_WIDTH = 64;
    const int MIN_TILE_WIDTH = 8;
    const CCacheSizes& cache = GetCacheSizes();
    const size_t nBytesPerSourceColumn = (size_t)MaxFilterLen(kernelsY, fullTargetOffset.y, clippedTargetSize.cy) * 3 * sizeof(float);
    const double dTargetPerSourceColumn = (double)fullTargetSize.cx / sourceSize.cx;
    // widest tile (in target columns, multiple of 8) whose ring buffer fits into nBudget bytes
    auto tileWidthFor = [&](size_t nBudget) {
        return (int)((double)(nBudget / nBytesPerSourceColumn) * dTargetPerSourceColumn) & ~7;
    };
    double dSourceColumns = (double)sourceSize.cx * clippedTargetSize.cx / fullTargetSize.cx;
    int nTileWidth = clippedTargetSize.cx;
    if (dSourceColumns * nBytesPerSourceColumn > cache.nL2 / 2) {
        nTileWidth = tileWidthFor(cache.nL2 / 2);
        if (nTileWidth < MIN_L2_TILE_WIDTH) {
            const int nThreads = std::max(1, CProcessingThreadPool::This().NumberOfThreads());
            nTileWidth = std::clamp(tileWidthFor(cache.nL3 / nThreads / 2), MIN_TILE_WIDTH, MIN_L2_TILE_WIDTH);
        }
    }

    for (int nTileStart = 0; nTileStart < clippedTargetSize.cx; nTileStart += nTileWidth) {
        int nTileSize = std::min(nTileWidth, clippedTargetSize.cx - nTileStart);
        if (!SampleTile_HQ_AVX(fullTargetSize, DPoint(fullTargetOffset.x + nTileStart, fullTargetOffset.y),
                               DSize(nTileSize, clippedTargetSize.cy), sourceSize, pPixels, nChannels, eFilter,
                               kernelsX, kernelsY, pTarget + (size_t)nTileStart * 4, clippedTargetSize.cx)) {
            return false;
        }
    }
    return true;
}

void* CBasicProcessing::SampleDown_HQ_SIMD(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
                                           DSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, SIMDArchitecture simd) {

    if (pPixels == nullptr || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return nullptr;
    }
    
    int padding = (simd == AVX2) ? 8 : 4;
    uint8_t* pTarget = new(std::nothrow) uint8_t[(size_t)clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
    if (pTarget == nullptr) return nullptr;
    CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
    CRequestUpDownSampling request(pPixels, sourceSize,
        pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
        nChannels, eFilter, simd);
    bool bSuccess = threadPool.Process(&request);

    if (!bSuccess) {
        delete[] pTarget;
        return nullptr;
    }
    return pTarget;
}

void* CBasicProcessing::SampleUp_HQ_SIMD(DSize fullTargetSize, DPoint fullTargetOffset, DSize clippedTargetSize,
    DSize sourceSize, const void* pPixels, int nChannels, SIMDArchitecture simd) {
    if (pPixels == nullptr || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return nullptr;
    }
    int padding = (simd == AVX2) ? 8 : 4;
    uint8_t* pTarget = new(std::nothrow) uint8_t[(size_t)clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
    if (pTarget == nullptr) return nullptr;
    CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
    CRequestUpDownSampling request(pPixels, sourceSize,
        pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
        nChannels, Filter_Upsampling_Bicubic, simd);
    bool bSuccess = threadPool.Process(&request);

    if (!bSuccess) {
        delete[] pTarget;
        return nullptr;
    }
    return pTarget;
}

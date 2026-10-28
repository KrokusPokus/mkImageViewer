#include "BasicProcessing.h"
#include "ResizeFilter.h"
#include "XMMImage.h"
#include "Helpers.h"
#include "ProcessingThreadPool.h"
#include "LookupTables.h"    // needed for LinRGB12_sRGB8[4096]

#include <immintrin.h> // Beinhaltet alle SSE/AVX-Intrinsics und _mm_round_ps für GCC/Clang/MSVC

// Fallback für _MM_ALIGN16, falls der Compiler es nicht bereitstellt
#ifndef _MM_ALIGN16
    #if defined(_MSC_VER)
        #define _MM_ALIGN16 __declspec(align(16))
    #else
        #define _MM_ALIGN16 __attribute__((aligned(16)))
    #endif
#endif

#include <cmath>

// This macro allows for aligned definition of a 16 byte value with initialization of the 8 components
// to a single value
#define DECLARE_ALIGNED_DQWORD(name, initializer) \
    int16 _tempVal##name[16]; \
    int16* name = (int16*)((((std::uintptr_t)&(_tempVal##name) + 15) & ~15)); \
    name[0] = name[1] = name[2] = name[3] = name[4] = name[5] = name[6] = name[7] = initializer;

#define ALPHA_OPAQUE 0xFF000000

/////////////////////////////////////////////////////////////////////////////////////////////
// Processing images stripwise on thread pool
/////////////////////////////////////////////////////////////////////////////////////////////

static void* SampleDown_HQ_MMX_SSE_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pIJLPixels, int nChannels, EFilterType eFilter, bool bSSE, uint8* pTarget);

static void* SampleDown_HQ_AVX_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pIJLPixels, int nChannels, EFilterType eFilter, uint8* pTarget);

static void* SampleUp_HQ_MMX_SSE_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pIJLPixels, int nChannels, bool bSSE,
    uint8* pTarget);

static void* SampleUp_HQ_AVX_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pIJLPixels, int nChannels,
    uint8* pTarget);

static void* ApplyLDC32bpp_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize dibSize,
    CSize ldcMapSize, const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
    float fBlackPt, float fWhitePt, float fBlackPtSteepness, uint32* pTarget);

//---------------------------------------------------------------------------------------------

// Request for upsampling or downsampling
class CRequestUpDownSampling : public CProcessingRequest {
public:
    CRequestUpDownSampling(const void* pSourcePixels, CSize sourceSize, void* pTargetPixels,
        CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
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

    virtual bool ProcessStrip(int offsetY, int sizeY) {
        if (Filter == Filter_Upsampling_Bicubic) {
            if (SIMD == CBasicProcessing::AVX2)
                return NULL != SampleUp_HQ_AVX_Core(FullTargetSize,
                    CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                    CSize(ClippedTargetSize.cx, sizeY),
                    SourceSize, SourcePixels,
                    Channels,
                    (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
            else
                return NULL != SampleUp_HQ_MMX_SSE_Core(FullTargetSize,
                    CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                    CSize(ClippedTargetSize.cx, sizeY),
                    SourceSize, SourcePixels,
                    Channels, SIMD == CBasicProcessing::SSE,
                    (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
        }
        else if (SIMD == CBasicProcessing::AVX2)
            return NULL != SampleDown_HQ_AVX_Core(FullTargetSize,
                CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                CSize(ClippedTargetSize.cx, sizeY),
                SourceSize, SourcePixels,
                Channels,
                Filter,
                (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
        else
            return NULL != SampleDown_HQ_MMX_SSE_Core(FullTargetSize,
                CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
                CSize(ClippedTargetSize.cx, sizeY),
                SourceSize, SourcePixels,
                Channels,
                Filter, SIMD == CBasicProcessing::SSE,
                (uint8*)TargetPixels + ClippedTargetSize.cx * 4 * offsetY);
    }

    int Channels;
    EFilterType Filter;
    CBasicProcessing::SIMDArchitecture SIMD;
};

class CRequestLDC : public CProcessingRequest {
public:
    CRequestLDC(const void* pSourcePixels, CSize sourceSize, void* pTargetPixels,
        CSize fullTargetSize, CPoint fullTargetOffset,
        CSize ldcMapSize, const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
        float fBlackPt, float fWhitePt, float fBlackPtSteepness)
        : CProcessingRequest(pSourcePixels, sourceSize, pTargetPixels, fullTargetSize, fullTargetOffset, sourceSize) {
        LDCMapSize = ldcMapSize;
        SatLUTs = pSatLUTs;
        LUT = pLUT;
        LDCMap = pLDCMap;
        BlackPt = fBlackPt;
        WhitePt = fWhitePt;
        BlackPtSteepness = fBlackPtSteepness;
    }

    virtual bool ProcessStrip(int offsetY, int sizeY) {
        return NULL != ApplyLDC32bpp_Core(FullTargetSize,
            CPoint(FullTargetOffset.x, FullTargetOffset.y + offsetY),
            CSize(ClippedTargetSize.cx, sizeY),
            LDCMapSize,
            (const uint32*)SourcePixels + ClippedTargetSize.cx * offsetY,
            SatLUTs, LUT, LDCMap,
            BlackPt, WhitePt, BlackPtSteepness,
            (uint32*)TargetPixels + ClippedTargetSize.cx * offsetY);
    }

    CSize LDCMapSize;
    const int32* SatLUTs;
    const uint8* LUT;
    const uint8* LDCMap;
    float BlackPt;
    float WhitePt;
    float BlackPtSteepness;
};

/////////////////////////////////////////////////////////////////////////////////////////////
// LUT creation for saturation, contrast and brightness and application of LUT
/////////////////////////////////////////////////////////////////////////////////////////////

uint8* CBasicProcessing::CreateSingleChannelLUT(double dContrastEnh, double dGamma) {
    dContrastEnh = std::min(0.5, std::max(-0.5, dContrastEnh));
    dGamma = std::min(10.0, std::max(0.1, dGamma));

    double dX = 0.0;
    double dInc = 1.0/255.0;
    double dValue;
    uint8* pLUT = new uint8[256];

    if (dContrastEnh >= 0) {
        // we define a piecewise function with quadratic terms on the left and right
        // and a linear term between
        dContrastEnh = dContrastEnh*0.5;
        double dA = 1.0/(1 - dContrastEnh);
        double dB = -dA*dContrastEnh/2;

        double dXs = (0.25 - dB)/dA;
        double dXe = (0.75 - dB)/dA;

        double dAs = (dA*dXs - 0.25)/(dXs*dXs);
        double dBs = dA - 2*dAs*dXs;

        for (int i = 0; i < 256; i++) {
            if (dX < dXs) {
                dValue = dAs*dX*dX + dBs*dX;
            } else if (dX > dXe) {
                dValue = 1.0 - (dAs*(1.0 - dX)*(1.0 - dX) + dBs*(1.0 - dX));
            } else {
                dValue = dA*dX + dB;
            }
            pLUT[i] = (uint8) (0.5 + 255*pow(std::min(1.0, std::max(0.0, dValue)), dGamma));
            dX += dInc;
        }
    } else {
        // define a quadratic piecewise S-curve
        double dFactor = -dContrastEnh*1.5;
        for (int i = 0; i < 256; i++) {
            if (dX < 0.5) {
                dValue = dX + dFactor*(0.25*0.25 - (dX - 0.25)*(dX - 0.25));
            } else  {
                dValue = dX - dFactor*(0.25*0.25 - (dX - 0.75)*(dX - 0.75));
            }
            pLUT[i] = (uint8) (0.5 + 255*pow(std::min(1.0, std::max(0.0, dValue)), dGamma));
            dX += dInc;
        }
    }

    return pLUT;
}

int32* CBasicProcessing::CreateColorSaturationLUTs(double dSaturation) {
    const double cdScaler = 1 << 16;
    int32* pLUTs = new int32[6 * 256];
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

void* CBasicProcessing::Apply3ChannelLUT32bpp(int nWidth, int nHeight, const void* pDIBPixels, const uint8* pLUT) {
    if (pDIBPixels == NULL || pLUT == NULL) {
        return NULL;
    }

    uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
    if (pTarget == NULL) return NULL;
    const uint32* pSrc = (uint32*)pDIBPixels;
    uint32* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            uint32 nSrcPixels = *pSrc;
            *pTgt = pLUT[nSrcPixels & 0xFF] + pLUT[256 + ((nSrcPixels >> 8) & 0xFF)] * 256 + 
                pLUT[512 + ((nSrcPixels >> 16) & 0xFF)] * 65536 + ALPHA_OPAQUE;
            pTgt++; pSrc++;
        }
    }
    return pTarget;
}

void* CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight, const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT) {
    if (pDIBPixels == NULL || pSatLUTs == NULL || pLUT == NULL) {
        return NULL;
    }

    const int cnScaler = 1 << 16;
    const int cnMax = 255 * cnScaler;
    uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
    if (pTarget == NULL) return NULL;
    const uint32* pSrc = (uint32*)pDIBPixels;
    uint32* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            uint32 nSrcPixels = *pSrc;
            int32 nSrcBlue = nSrcPixels & 0xFF;
            int32 nSrcGreen = (nSrcPixels >> 8) & 0xFF;
            int32 nSrcRed = (nSrcPixels >> 16) & 0xFF;
            int32 nRed = pSatLUTs[nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
            int32 nGreen = pSatLUTs[768 + nSrcRed] + pSatLUTs[1024 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
            int32 nBlue = pSatLUTs[768 + nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[1280 + nSrcBlue];
            *pTgt = pLUT[std::max(0, std::min(cnMax, nBlue)) >> 16] + pLUT[256 + (std::max(0, std::min(cnMax, nGreen)) >> 16)] * 256 +
                pLUT[512 + (std::max(0, std::min(cnMax, nRed)) >> 16)] * 65536 + ALPHA_OPAQUE;
            pTgt++; pSrc++;
        }
    }
    return pTarget;
}

// Create the LDC response LUT between black and white points. This LUT makes sure
// that neither black nor white point is altered by the LDC.
static int32* CreateMulLUT(float fBlackPt, float fWhitePt, float fBlackPtSteepness) {
    const float cfFactor = 0.8f; // multiplication factor -> Strength of LDC
    const float cfSteepnessBlack = 0.6f; // how fast is the full strength reached after black pt
    const float cfSteepnessWhite = 0.6f; // how fast is the full strength reached after white pt

    int32* pNewLUT = new int32[256];
    if (fWhitePt <= fBlackPt) {
        memset(pNewLUT, 0, 256*sizeof(int32));
        return pNewLUT;
    }
    fBlackPtSteepness = std::max(0.0f, std::min(1.0f, (1.0f - 0.98f*fBlackPtSteepness)));
    float fMid = (fBlackPt + fWhitePt)/2;
    float fEndSlopeB = fBlackPt +cfSteepnessBlack*(fMid - fBlackPt);
    float fEndSlopeW = fWhitePt - cfSteepnessWhite*(fWhitePt - fMid);
    for (int i = 0; i < 256; i++) {
        int nLUTValue;
        float f = i*(1.0f/255.0f);
        if (f < fBlackPt) {
            nLUTValue = 0;
        } else if (f >= fWhitePt) {
            nLUTValue = 0;
        } else {
            if (f < fEndSlopeB) {
                float fValue = (f - fBlackPt)/(fEndSlopeB - fBlackPt);
                nLUTValue = (int)(cfFactor*16384*powf(fValue, fBlackPtSteepness) + 0.5f);
            } else if (f > fEndSlopeW) {
                nLUTValue = (int)(cfFactor*16384*(1.0f - (f - fEndSlopeW)/(fWhitePt - fEndSlopeW)) + 0.5f);
            } else {
                nLUTValue = (int)(cfFactor*16384 + 0.5f);
            }
        }
        pNewLUT[i] = (int32)nLUTValue;
    }
    return pNewLUT;
}

void* ApplyLDC32bpp_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize dibSize,
                                      CSize ldcMapSize, const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
                                      float fBlackPt, float fWhitePt, float fBlackPtSteepness, uint32* pTarget) {

    uint32 nIncrementX, nIncrementY;
    nIncrementX = (ldcMapSize.cx == 1) ? 0 : (uint32)((65536*(uint32)(ldcMapSize.cx - 1))/(fullTargetSize.cx - 1) - 1);
    nIncrementY = (ldcMapSize.cy == 1) ? 0 : (uint32)((65536*(uint32)(ldcMapSize.cy - 1))/(fullTargetSize.cy - 1) - 1);

    uint32 nCurY = fullTargetOffset.y*nIncrementY;
    uint32 nStartX = fullTargetOffset.x*nIncrementX;

    const int cnScaler = 1 << 16;
    const int cnMax = 255 * cnScaler;
    const int32* pMulLUT = CreateMulLUT(fBlackPt, fWhitePt, fBlackPtSteepness);
    const uint32* pSrc = (uint32*)pDIBPixels;
    uint32* pTgt = pTarget;
    for (int j = 0; j < dibSize.cy; j++) {
        uint32 nCurYTrunc = nCurY >> 16;
        uint32 nCurYFrac = nCurY & 0xFFFF;
        const uint8* pLDCMapSrc = pLDCMap + ldcMapSize.cx * nCurYTrunc;
        uint32 nCurX = nStartX;
        if (pSatLUTs == NULL) {
            for (int i = 0; i < dibSize.cx; i++) {
                // perform bilinear interpolation of mask
                uint32 nCurXTrunc = nCurX >> 16;
                uint32 nCurXFrac = nCurX & 0xFFFF;
                uint32 nMaskTopLeft  = pLDCMapSrc[nCurXTrunc];
                uint32 nMaskTopRight = pLDCMapSrc[nCurXTrunc + 1];
                uint32 nMaskBottomLeft = pLDCMapSrc[nCurXTrunc + ldcMapSize.cx];
                uint32 nMaskBottomRight = pLDCMapSrc[nCurXTrunc + ldcMapSize.cx + 1];
                uint32 nLeft = ((int)nCurYFrac*(int)(nMaskBottomLeft - nMaskTopLeft) >> 16) + nMaskTopLeft;
                uint32 nRight = ((int)nCurYFrac*(int)(nMaskBottomRight - nMaskTopRight) >> 16) + nMaskTopRight;
                int32 nMaskValue = ((int)nCurXFrac*(int)(nRight - nLeft) >> 16) + nLeft - 127;

                uint32 nSrcPixels = *pSrc;
                uint32 nBlue = pLUT[nSrcPixels & 0xFF];
                nBlue = nBlue + (nMaskValue*pMulLUT[nBlue] >> 14);
                uint32 nGreen = pLUT[((nSrcPixels >> 8) & 0xFF) + 256];
                nGreen = nGreen + (nMaskValue*pMulLUT[nGreen] >> 14);
                uint32 nRed = pLUT[((nSrcPixels >> 16) & 0xFF) + 512];
                nRed = nRed + (nMaskValue*pMulLUT[nRed] >> 14);

                *pTgt = std::max(0, std::min(255, (int)nBlue)) + std::max(0, std::min(255, (int)nGreen))*256 + std::max(0, std::min(255, (int)nRed))*65536 + ALPHA_OPAQUE;
                pTgt++; pSrc++;
                nCurX += nIncrementX;
            }
        } else {
            for (int i = 0; i < dibSize.cx; i++) {
                // perform bilinear interpolation of mask
                uint32 nCurXTrunc = nCurX >> 16;
                uint32 nCurXFrac = nCurX & 0xFFFF;
                uint32 nMaskTopLeft  = pLDCMapSrc[nCurXTrunc];
                uint32 nMaskTopRight = pLDCMapSrc[nCurXTrunc + 1];
                uint32 nMaskBottomLeft = pLDCMapSrc[nCurXTrunc + ldcMapSize.cx];
                uint32 nMaskBottomRight = pLDCMapSrc[nCurXTrunc + ldcMapSize.cx + 1];
                uint32 nLeft = ((int)nCurYFrac*(int)(nMaskBottomLeft - nMaskTopLeft) >> 16) + nMaskTopLeft;
                uint32 nRight = ((int)nCurYFrac*(int)(nMaskBottomRight - nMaskTopRight) >> 16) + nMaskTopRight;
                int32 nMaskValue = ((int)nCurXFrac*(int)(nRight - nLeft) >> 16) + nLeft - 127;

                // Apply saturation LUTs and three channel LUT
                uint32 nSrcPixels = *pSrc;
                int32 nSrcBlue = nSrcPixels & 0xFF;
                int32 nSrcGreen = (nSrcPixels >> 8) & 0xFF;
                int32 nSrcRed = (nSrcPixels >> 16) & 0xFF;
                int32 nRed = pSatLUTs[nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
                int32 nGreen = pSatLUTs[768 + nSrcRed] + pSatLUTs[1024 + nSrcGreen] + pSatLUTs[512 + nSrcBlue];
                int32 nBlue = pSatLUTs[768 + nSrcRed] + pSatLUTs[256 + nSrcGreen] + pSatLUTs[1280 + nSrcBlue];
                nBlue = pLUT[std::max(0, std::min(cnMax, nBlue)) >> 16];
                nBlue = nBlue + (nMaskValue*pMulLUT[nBlue] >> 14);
                nGreen = pLUT[(std::max(0, std::min(cnMax, nGreen)) >> 16) + 256];
                nGreen = nGreen + (nMaskValue*pMulLUT[nGreen] >> 14);
                nRed = pLUT[(std::max(0, std::min(cnMax, nRed)) >> 16) + 512];
                nRed = nRed + (nMaskValue*pMulLUT[nRed] >> 14);

                *pTgt = std::max(0, std::min(255, (int)nBlue)) + std::max(0, std::min(255, (int)nGreen))*256 + std::max(0, std::min(255, (int)nRed))*65536 + ALPHA_OPAQUE;
                pTgt++; pSrc++;
                nCurX += nIncrementX;
            }
        }
        nCurY += nIncrementY;
    }
    delete[] pMulLUT;
    return pTarget;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Conversion and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

// Rotate a block of a 32 bit DIB from source to target by 90 or 270 degrees
static void RotateBlock32bpp(const uint32* pSrc, uint32* pTgt, int nWidth, int nHeight,
                             int nXStart, int nYStart, int nBlockWidth, int nBlockHeight, bool bCW) {
    int nIncTargetLine = nHeight;
    int nIncSource = nWidth - nBlockWidth;
    const uint32* pSource = pSrc + nWidth * nYStart + nXStart;
    uint32* pTarget = bCW ? pTgt + nIncTargetLine * nXStart + (nHeight - 1 - nYStart) :
        pTgt + nIncTargetLine * (nWidth - 1 - nXStart) + nYStart;
    uint32* pStartYPtr = pTarget;
    if (!bCW) nIncTargetLine = -nIncTargetLine;
    int nIncStartYPtr = bCW ? -1 : +1;

    for (uint32 i = 0; i < nBlockHeight; i++) {
        for (uint32 j = 0; j < nBlockWidth; j++) {
            *pTarget = *pSource;
            pTarget += nIncTargetLine;
            pSource++;
        }
        pStartYPtr += nIncStartYPtr;
        pTarget = pStartYPtr;
        pSource += nIncSource;
    }
}

// 180 degrees rotation
static void* Rotate32bpp180(int nWidth, int nHeight, const void* pDIBPixels) {
    uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
    if (pTarget == NULL) return NULL;
    const uint32* pSource = (uint32*)pDIBPixels;
    for (uint32 i = 0; i < nHeight; i++) {
        uint32* pTgt = pTarget + nWidth*(nHeight - 1 - i) + (nWidth - 1);
        const uint32* pSrc = pSource + nWidth*i;
        for (uint32 j = 0; j < nWidth; j++) {
            *pTgt = *pSrc;
            pTgt -= 1;
            pSrc += 1;
        }
    }
    return pTarget;
}

void* CBasicProcessing::Rotate32bpp(int nWidth, int nHeight, const void* pDIBPixels, int nRotationAngleCW) {
    if (pDIBPixels == NULL) {
        return NULL;
    }
    if (nRotationAngleCW != 90 && nRotationAngleCW != 180 && nRotationAngleCW != 270) {
        return NULL; // not allowed
    } else if (nRotationAngleCW == 180) {
        return Rotate32bpp180(nWidth, nHeight, pDIBPixels);
    }

    uint32* pTarget = new(std::nothrow) uint32[nHeight * nWidth];
    if (pTarget == NULL) return NULL;
    const uint32* pSource = (uint32*)pDIBPixels;

    const int cnBlockSize = 32;
    int nX = 0, nY = 0;
    while (nY < nHeight) {
        nX = 0;
        while (nX < nWidth) {
            RotateBlock32bpp(pSource, pTarget, nWidth, nHeight,
                nX, nY, 
                std::min(cnBlockSize, nWidth - nX),
                std::min(cnBlockSize, nHeight - nY), nRotationAngleCW == 90);
            nX += cnBlockSize;
        }
        nY += cnBlockSize;
    }

    return pTarget;
}

void* CBasicProcessing::Mirror32bpp(int nWidth, int nHeight, const void* pDIBPixels, bool bHorizontally) {
    return bHorizontally ? CBasicProcessing::MirrorH32bpp(nWidth, nHeight, pDIBPixels) :
        CBasicProcessing::MirrorV32bpp(nWidth, nHeight, pDIBPixels);
}

void* CBasicProcessing::MirrorH32bpp(int nWidth, int nHeight, const void* pDIBPixels) {
    uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
    if (pTarget == NULL) return NULL;
    uint32* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        const uint32* pSource = (uint32*)pDIBPixels + (j + 1) * nWidth - 1;
        for (int i = 0; i < nWidth; i++) {
            *pTgt = *pSource;
            pTgt++; pSource--;
        }
    }
    return pTarget;
}

void* CBasicProcessing::MirrorV32bpp(int nWidth, int nHeight, const void* pDIBPixels) {
    uint32* pTarget = new(std::nothrow) uint32[nWidth * nHeight];
    if (pTarget == NULL) return NULL;
    uint32* pTgt = pTarget;
    for (int j = 0; j < nHeight; j++) {
        const uint32* pSource = (uint32*)pDIBPixels + (nHeight - 1 - j) * nWidth;
        for (int i = 0; i < nWidth; i++) {
            *pTgt = *pSource;
            pTgt++; pSource++;
        }
    }
    return pTarget;
}

void CBasicProcessing::MirrorVInplace(int nWidth, int nHeight, int nStride, void* pDIBPixels) {
    for (int j = 0; j < (nHeight >> 1); j++) {
        if ((nStride & 3) == 0) {
            uint32 nPixelsPerStride = nStride >> 2;
            uint32* pSource = (uint32*)pDIBPixels + (nHeight - 1 - j) * nPixelsPerStride;
            uint32* pTgt = (uint32*)pDIBPixels + j * nPixelsPerStride;
            for (int i = 0; i < nPixelsPerStride; i++) {
                uint32 t = *pTgt;
                *pTgt = *pSource;
                *pSource = t;
                pTgt++; pSource++;
            }
        } else {
            uint8* pSource = (uint8*)pDIBPixels + (nHeight - 1 - j) * nStride;
            uint8* pTgt = (uint8*)pDIBPixels + j * nStride;
            for (int i = 0; i < nStride; i++) {
                uint8 t = *pTgt;
                *pTgt = *pSource;
                *pSource = t;
                pTgt++; pSource++;
            }
        }
    }
}

void* CBasicProcessing::Convert8bppTo32bppDIB(int nWidth, int nHeight, const void* pDIBPixels, const uint8* pPalette) {
    if (pDIBPixels == NULL || pPalette == NULL) {
        return NULL;
    }
    int nPaddedWidthS = Helpers::DoPadding(nWidth, 4);
    int nPadS = nPaddedWidthS - nWidth;
    uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
    if (pNewDIB == NULL) return NULL;
    uint32* pTargetDIB = pNewDIB;
    const uint8* pSourceDIB = (uint8*)pDIBPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTargetDIB++ = pPalette[4 * *pSourceDIB] + pPalette[4 * *pSourceDIB + 1] * 256 + pPalette[4 * *pSourceDIB + 2] * 65536 + ALPHA_OPAQUE;
            pSourceDIB ++;
        }
        pSourceDIB += nPadS;
    }
    return pNewDIB;
}

void CBasicProcessing::Convert32bppTo24bppDIB(int nWidth, int nHeight, void* pDIBTarget, 
                                              const void* pDIBSource, bool bFlip) {
    if (pDIBTarget == NULL || pDIBSource == NULL) {
        return;
    }
    int nPaddedWidth = Helpers::DoPadding(nWidth*3, 4);
    int nPad = nPaddedWidth - nWidth*3;
    uint8* pTargetDIB = (uint8*)pDIBTarget;
    for (int j = 0; j < nHeight; j++) {
        const uint8* pSourceDIB;
        if (bFlip) {
            pSourceDIB = (uint8*)pDIBSource + (nHeight - 1 - j)*nWidth*4;
        } else {
            pSourceDIB = (uint8*)pDIBSource + j*nWidth*4;
        }
        for (int i = 0; i < nWidth; i++) {
            *pTargetDIB++ = pSourceDIB[0];
            *pTargetDIB++ = pSourceDIB[1];
            *pTargetDIB++ = pSourceDIB[2];
            pSourceDIB += 4;
        }
        pTargetDIB += nPad;
    }
}

void* CBasicProcessing::Convert1To4Channels(int nWidth, int nHeight, const void* pPixels) {
    if (pPixels == NULL) {
        return NULL;
    }
    int nPadSrc = Helpers::DoPadding(nWidth, 4) - nWidth;
    uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
    if (pNewDIB == NULL) return NULL;
    uint32* pTarget = pNewDIB;
    const uint8* pSource = (uint8*)pPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTarget++ = *pSource + *pSource * 256 + *pSource * 65536 + ALPHA_OPAQUE;
            pSource++;
        }
        pSource += nPadSrc;
    }
    return pNewDIB;
}

void* CBasicProcessing::Convert3To4Channels(int nWidth, int nHeight, const void* pIJLPixels) {
    if (pIJLPixels == NULL) {
        return NULL;
    }
    int nPadSrc = Helpers::DoPadding(nWidth*3, 4) - nWidth*3;
    uint32* pNewDIB = new(std::nothrow) uint32[nWidth * nHeight];
    if (pNewDIB == NULL) return NULL;
    uint32* pTarget = pNewDIB;
    const uint8* pSource = (uint8*)pIJLPixels;
    for (int j = 0; j < nHeight; j++) {
        for (int i = 0; i < nWidth; i++) {
            *pTarget++ = pSource[0] + pSource[1] * 256 + pSource[2] * 65536 + ALPHA_OPAQUE;
            pSource += 3;
        }
        pSource += nPadSrc;
    }
    return pNewDIB;
}

void* CBasicProcessing::CopyRect32bpp(void* pTarget, const void* pSource,  CSize targetSize, CRect targetRect,
                                      CSize sourceSize, CRect sourceRect) {
    if (pSource == NULL || sourceRect.Size() != targetRect.Size() || 
        sourceRect.left < 0 || sourceRect.right > sourceSize.cx ||
        sourceRect.top < 0 || sourceRect.bottom > sourceSize.cy ||
        targetRect.left < 0 || targetRect.right > targetSize.cx ||
        targetRect.top < 0 || targetRect.bottom > targetSize.cy) {
        return NULL;
    }
    uint32* pSourceDIB = (uint32*)pSource;
    uint32* pTargetDIB = (uint32*)pTarget;
    if (pTargetDIB == NULL) {
        pTargetDIB = new(std::nothrow) uint32[targetSize.cx * targetSize.cy];
        if (pTargetDIB == NULL) return NULL;
        pTarget = pTargetDIB;
    }

    pTargetDIB += (targetRect.top * targetSize.cx) + targetRect.left;
    pSourceDIB += (sourceRect.top * sourceSize.cx) + sourceRect.left;
    for (int y = 0; y < sourceRect.Height(); y++) {
        memcpy(pTargetDIB, pSourceDIB, sourceRect.Width()*sizeof(uint32));
        pTargetDIB += targetSize.cx;
        pSourceDIB += sourceSize.cx;
    }

    return pTarget;
}

void* CBasicProcessing::Crop32bpp(int nWidth, int nHeight, const void* pDIBPixels, CRect cropRect) {
    if (pDIBPixels == NULL || cropRect.Width() == 0 || cropRect.Height() == 0) {
        return NULL;
    }

    return CopyRect32bpp(NULL, pDIBPixels, cropRect.Size(), CRect(0, 0, cropRect.Width(), cropRect.Height()),
        CSize(nWidth, nHeight), cropRect);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Simple point sampling resize and rotation methods
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::PointSample(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize, 
    CSize sourceSize, const void* pPixels, int nChannels) {
    if (fullTargetSize.cx < 1 || fullTargetSize.cy < 1 ||
        clippedTargetSize.cx < 1 || clippedTargetSize.cy < 1 ||
        fullTargetOffset.x < 0 || fullTargetOffset.x < 0 ||
        clippedTargetSize.cx + fullTargetOffset.x > fullTargetSize.cx ||
        clippedTargetSize.cy + fullTargetOffset.y > fullTargetSize.cy ||
        pPixels == NULL || (nChannels != 3 && nChannels != 4)) {
        return NULL;
    }

    uint8* pDIB = new(std::nothrow) uint8[clippedTargetSize.cx*4 * clippedTargetSize.cy];
    if (pDIB == NULL) return NULL;

    uint32 nIncrementX, nIncrementY;
    if (fullTargetSize.cx <= sourceSize.cx) {
        // Downsampling
        nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
        nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;
    } else {
        // Upsampling
        nIncrementX = (fullTargetSize.cx == 1) ? 0 : (uint32)((65536*(uint32)(sourceSize.cx - 1) + 65535)/(fullTargetSize.cx - 1));
        nIncrementY = (fullTargetSize.cy == 1) ? 0 : (uint32)((65536*(uint32)(sourceSize.cy - 1) + 65535)/(fullTargetSize.cy - 1));
    }

    int nPaddedSourceWidth = Helpers::DoPadding(sourceSize.cx * nChannels, 4);
    const uint8* pSrc = NULL;
    uint8* pDst = pDIB;
    uint32 nCurY = fullTargetOffset.y*nIncrementY;
    uint32 nStartX = fullTargetOffset.x*nIncrementX;
    for (int j = 0; j < clippedTargetSize.cy; j++) {
        pSrc = (uint8*)pPixels + nPaddedSourceWidth * (nCurY >> 16);
        uint32 nCurX = nStartX;
        if (nChannels == 3) {
            for (int i = 0; i < clippedTargetSize.cx; i++) {
                uint32 sx = nCurX >> 16; 
                uint32 s = sx*3;
                uint32 d = i*4;
                pDst[d] = pSrc[s];
                pDst[d+1] = pSrc[s+1];
                pDst[d+2] = pSrc[s+2];
                pDst[d+3] = 0xFF;
                nCurX += nIncrementX;
            }
        } else {
            for (int i = 0; i < clippedTargetSize.cx; i++) {
                uint32 sx = nCurX >> 16; 
                ((uint32*)pDst)[i] = ((uint32*)pSrc)[sx];
                nCurX += nIncrementX;
            }
        }
        pDst += clippedTargetSize.cx*4;
        nCurY += nIncrementY;
    }
    return pDIB;
}

static void RotateInplace(double& dX, double& dY, double dAngle) {
    double dXr = cos(dAngle) * dX - sin(dAngle) * dY;
    double dYr = sin(dAngle) * dX + cos(dAngle) * dY;
    dX = dXr;
    dY = dYr;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality rotation using bicubic sampling
/////////////////////////////////////////////////////////////////////////////////////////////

#define NUM_KERNELS_LOG2 6
#define NUM_KERNELS_BICUBIC 65
#define FP_HALF 8192

// Bicubic resampling of one pixel at pSource (in 3 or 4 channel format) into one destination pixel in 32 bit format
// The fractional part of the pixel position is given in .16 fixed point format
// pKernels contains NUM_KERNELS_BICUBIC precalculated bicubic filter kernels each of length 4, applied with offset -1 to the source pixels
static void InterpolateBicubic(const uint8* pSource, uint8* pDest, int16* pKernels, int32 nFracX, int32 nFracY, int nPaddedSourceWidth, int nChannels) {
    int16* pKernelX = &(pKernels[4*(nFracX >> (16 - NUM_KERNELS_LOG2))]);
    int16* pKernelY = &(pKernels[4*(nFracY >> (16 - NUM_KERNELS_LOG2))]);
    int nChannels2 = nChannels * 2;
    for (int i = 0; i < nChannels; i++) {
        const uint8* pSrc = pSource;
        pSrc -= nPaddedSourceWidth;

        int32 nSum1 = pSrc[-nChannels] * pKernelX[0] + pSrc[0] * pKernelX[1] + pSrc[nChannels] * pKernelX[2] + pSrc[nChannels2] * pKernelX[3] + FP_HALF;
        pSrc += nPaddedSourceWidth;
        int32 nSum2 = pSrc[-nChannels] * pKernelX[0] + pSrc[0] * pKernelX[1] + pSrc[nChannels] * pKernelX[2] + pSrc[nChannels2] * pKernelX[3] + FP_HALF;
        pSrc += nPaddedSourceWidth;
        int32 nSum3 = pSrc[-nChannels] * pKernelX[0] + pSrc[0] * pKernelX[1] + pSrc[nChannels] * pKernelX[2] + pSrc[nChannels2] * pKernelX[3] + FP_HALF;
        pSrc += nPaddedSourceWidth;
        int32 nSum4 = pSrc[-nChannels] * pKernelX[0] + pSrc[0] * pKernelX[1] + pSrc[nChannels] * pKernelX[2] + pSrc[nChannels2] * pKernelX[3] + FP_HALF;

        int32 nSum = (nSum1 >> 14) * pKernelY[0] + (nSum2 >> 14) * pKernelY[1] + (nSum3 >> 14) * pKernelY[2] + (nSum4 >> 14) * pKernelY[3] + FP_HALF;
        nSum = nSum >> 14;
        *pDest++ = std::min(255, std::max(0, nSum));
        pSource++;
    }
    if (nChannels == 3) *pDest = 0xFF;
}

static inline void Get4Pixels(const uint8* pSrc, uint8 dest[4], int nFrom, int nTo, int nChannels, uint32 nFill) {
    int nC = -nChannels;
    for (int i = 0; i < 4; i++) {
        dest[i] = (i >= nFrom + 1 && i <= nTo + 1) ? pSrc[nC] : (uint8)nFill;
        nC += nChannels;
    }
}

// Same as method above but with border handling, assuming that the filter kernels are only evaluated from nXFrom to nXTo and nYFrom to nYTo
// Pixels that are outside this range are assumed to have value nBackColor
static void InterpolateBicubicBorder(const uint8* pSource, uint8* pDest, int16* pKernels, 
                                     int32 nFracX, int32 nFracY, int nXFrom, int nXTo, int nYFrom, int nYTo, int nPaddedSourceWidth, int nChannels, uint32 nBackColor) {
    int16* pKernelX = &(pKernels[4*(nFracX >> (16 - NUM_KERNELS_LOG2))]);
    int16* pKernelY = &(pKernels[4*(nFracY >> (16 - NUM_KERNELS_LOG2))]);
    uint32 nBackColorShifted = nBackColor;
    for (int i = 0; i < nChannels; i++) {
        const uint8* pSrc = pSource;
        pSrc -= nPaddedSourceWidth;
        uint32 nFill = nBackColorShifted & 0xFF;
        int32 nFillShifted = nFill << 14;
        nBackColorShifted = nBackColorShifted >> 8;

        uint8 pixels[4];

        int32 nSum1 = nFillShifted;
        if (nYFrom <= -1) {
            Get4Pixels(pSrc, pixels, nXFrom, nXTo, nChannels, nFill); 
            nSum1 = pixels[0] * pKernelX[0] + pixels[1] * pKernelX[1] + pixels[2] * pKernelX[2] + pixels[3] * pKernelX[3] + FP_HALF;
        }
        pSrc += nPaddedSourceWidth;
        int nSum2 = nFillShifted;
        if (nYFrom <= 0 && nYTo >= 0) {
            Get4Pixels(pSrc, pixels, nXFrom, nXTo, nChannels, nFill); 
            nSum2 = pixels[0] * pKernelX[0] + pixels[1] * pKernelX[1] + pixels[2] * pKernelX[2] + pixels[3] * pKernelX[3] + FP_HALF;
        }
        pSrc += nPaddedSourceWidth;
        int32 nSum3 = nFillShifted;
        if (nYFrom <= 1 && nYTo >= 1) {
            Get4Pixels(pSrc, pixels, nXFrom, nXTo, nChannels, nFill); 
            nSum3 = pixels[0] * pKernelX[0] + pixels[1] * pKernelX[1] + pixels[2] * pKernelX[2] + pixels[3] * pKernelX[3] + FP_HALF;
        }
        pSrc += nPaddedSourceWidth;
        int32 nSum4 = nFillShifted;
        if (nYTo >= 2) {
            Get4Pixels(pSrc, pixels, nXFrom, nXTo, nChannels, nFill); 
            nSum4 = pixels[0] * pKernelX[0] + pixels[1] * pKernelX[1] + pixels[2] * pKernelX[2] + pixels[3] * pKernelX[3] + FP_HALF;
        }

        int32 nSum = (nSum1 >> 14) * pKernelY[0] + (nSum2 >> 14) * pKernelY[1] + (nSum3 >> 14) * pKernelY[2] + (nSum4 >> 14) * pKernelY[3] + FP_HALF;
        nSum = nSum >> 14;

        *pDest++ = std::min(255, std::max(0, nSum));
        pSource++;
    }
    if (nChannels == 3) *pDest = 0xFF;
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
static uint8* ApplyFilter(int nSourceWidth, int nTargetWidth, int nHeight,
                          int nSourceBytesPerPixel,
                          int nStartX_FP, int nStartY, int nIncrementX_FP,
                          const FilterKernelBlock& filter,
                          int nFilterOffset,
                          const uint8* pSource) {

    uint8* pTarget = new(std::nothrow) uint8[nTargetWidth*4*nHeight];
    if (pTarget == NULL) return NULL;

    // width of new image is (after rotation) : nHeight
    // height of new image is (after rotation) : nTargetWidth
    
    int nPaddedSourceWidth = Helpers::DoPadding(nSourceWidth * nSourceBytesPerPixel, 4);
    const uint8* pSourcePixelLine = NULL;
    uint8* pTargetPixelLine = NULL;
    const int FP_05 = 255; // rounding correction because in filter 1.0 is 16383 but we shift by 14 what is a division by 16384
    for (int j = 0; j < nHeight; j++) {
        pSourcePixelLine = ((uint8*) pSource) + nPaddedSourceWidth * (j + nStartY);
        pTargetPixelLine = pTarget + 4*j;
        uint8* pTargetPixel = pTargetPixelLine;
        uint32 nX = nStartX_FP;
        for (int i = 0; i < nTargetWidth; i++) {
            uint32 nXSourceInt = nX >> 16;
            FilterKernel* pKernel = filter.Indices[i + nFilterOffset];
            const uint8* pSourcePixel = pSourcePixelLine + nSourceBytesPerPixel*(nXSourceInt - pKernel->FilterOffset);
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

            *pTargetPixel++ = (uint8)std::max(0, std::min(255, nPixelValue1));
            *pTargetPixel++ = (uint8)std::max(0, std::min(255, nPixelValue2));
            *pTargetPixel++ = (uint8)std::max(0, std::min(255, nPixelValue3));
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

void* CBasicProcessing::SampleUp_HQ(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pPixels, int nChannels) {

    // Resizing consists of resize in x direction followed by resize in y direction.
    // To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
    // thus enabling to use the same loop on the rows for both resize directions.
    // The image is correctly orientated again after two rotation/flip operations!

    if (pPixels == NULL || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return NULL;
    }
    int nTargetWidth = clippedTargetSize.cx;
    int nTargetHeight = clippedTargetSize.cy;
    int nSourceWidth = sourceSize.cx;
    int nSourceHeight = sourceSize.cy;

    uint32 nIncrementX = (uint32)(65536*(uint32)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
    uint32 nIncrementY = (uint32)(65536*(uint32)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

    // Caution: This code assumes a upsampling filter kernel of length 4, with a filter offset of 1
    int nFirstY = std::max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
    int nLastY = std::min<int>(sourceSize.cy - 1, ((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2);
    int nTempTargetWidth = nLastY - nFirstY + 1;
    int nTempTargetHeight = nTargetWidth;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncrementX*fullTargetOffset.x;
    int nStartY = nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    CResizeFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic, FilterSIMDType_None);
    const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();

    uint8* pTemp = ApplyFilter(nSourceWidth, nTempTargetHeight, nTempTargetWidth,
        nChannels, nStartX, nFirstY, nIncrementX,
        kernelsX, nFilterOffsetX, (const uint8*)pPixels);
    if (pTemp == NULL) return NULL;

    CResizeFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic, FilterSIMDType_None);
    const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

    uint8* pDIB = ApplyFilter(nTempTargetWidth, nTargetHeight, nTargetWidth,
            4, nStartY, 0, nIncrementY,
            kernelsY, nFilterOffsetY, pTemp);

    delete[] pTemp;

    return pDIB;
}


/////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (C++ implementation)
// Used with generic CPUs where no SIMD extensions are available.
// This will NOT use linear color space! (it would need INT64 or FLOAT!)
// This WILL still use Hermite/Mitchell/Catrom/Lanczos2 kernel as specified.
/////////////////////////////////////////////////////////////////////////////////////////////

void* CBasicProcessing::SampleDown_HQ(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter) {
    // Resizing consists of resize in x direction followed by resize in y direction.
    // To simplify implementation, the method performs a 90 degree rotation/flip while resizing,
    // thus enabling to use the same loop on the rows for both resize directions.
    // The image is correctly orientated again after two rotation/flip operations!

    if (pPixels == NULL || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return NULL;
    }
    CResizeFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter, FilterSIMDType_None);
    const FilterKernelBlock& kernelsX = filterX.GetFilterKernels();
    CResizeFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter, FilterSIMDType_None);
    const FilterKernelBlock& kernelsY = filterY.GetFilterKernels();

    uint32 nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
    uint32 nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;

    int nIncOffsetX = (nIncrementX - 65536) >> 1;
    int nIncOffsetY = (nIncrementY - 65536) >> 1;
    int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
    nFirstY = std::max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
    int nLastY  = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
    FilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
    nLastY  = std::min<int>(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
    int nTempTargetWidth = nLastY - nFirstY + 1;
    int nTempTargetHeight = clippedTargetSize.cx;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x;
    int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    uint8* pTemp = ApplyFilter(sourceSize.cx, nTempTargetHeight, nTempTargetWidth,
        nChannels, nStartX, nFirstY, nIncrementX,
        kernelsX, nFilterOffsetX, (const uint8*)pPixels);
    if (pTemp == NULL) return NULL;

    uint8* pDIB = ApplyFilter(nTempTargetWidth, clippedTargetSize.cy, clippedTargetSize.cx,
            4, nStartY, 0, nIncrementY,
            kernelsY, nFilterOffsetY, pTemp);

    delete[] pTemp;

    return pDIB;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// High quality downsampling (Helpers for SSE and MMX implementation)
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

inline static const float* RotateLineToDIB_1(const float* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

    for (int i = 0; i < simdPixelsPerRegister - 1; i++)
    {
        *((uint8*)pTarget) = LinRGB12_sRGB8[(int)(*pSource)]; pSource++;  pTarget += nIncTargetLine;
    }
    *((uint8*)pTarget) = LinRGB12_sRGB8[(int)(*pSource)]; pSource++;

    return pSource;
}

inline static const float* RotateLineToDIB(const float* pSource, uint8* pTarget, int nIncTargetLine, int simdPixelsPerRegister) {

    for (int i = 0; i < simdPixelsPerRegister - 1; i++)
    {
        *pTarget = LinRGB12_sRGB8[(int)(*pSource++)]; pTarget += nIncTargetLine;
    }
    *pTarget = LinRGB12_sRGB8[(int)(*pSource++)];

    return pSource;
}

// Rotate a block in a CXMMImage. Blockwise rotation is needed because with normal
// rotation, trashing occurs, making rotation a very slow operation.
// The input format is what the ResizeYCore() method outputs:
// RRRRRRRRGGGGGGGGBBBBBBBB... (blocks of 'simdPixelsPerRegister' pixels of a channel).
// After rotation, the format is line interleaved again:
// RRRRRRRRRRR...
// GGGGGGGGGGG...
// BBBBBBBBBBB...
static void RotateBlock(const float* pSrc, float* pTgt, int nWidth, int nHeight,
                        int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
                        int simdPixelsPerRegister) {
    int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
    int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
    int nIncTargetChannel = nPaddedHeight;
    int nIncTargetLine = nIncTargetChannel * 3;
    int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
    const float* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
    float* pTarget = pTgt + nPaddedHeight * 3 * nXStart + nYStart;
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

// Same as above, directly rotates into a 32 bpp DIB
static void RotateBlockToDIB(const float* pSrc, uint8* pTgt, int nWidth, int nHeight,
                             int nXStart, int nYStart, int nBlockWidth, int nBlockHeight,
                             int simdPixelsPerRegister) {
    int nPaddedWidth = Helpers::DoPadding(nWidth, simdPixelsPerRegister);
    //int nPaddedHeight = Helpers::DoPadding(nHeight, simdPixelsPerRegister);
    int nIncTargetLine = nHeight * 4;
    int nIncSource = nPaddedWidth * 3 - nBlockWidth * 3;
    const float* pSource = pSrc + nPaddedWidth * 3 * nYStart + nXStart * 3;
    uint8* pTarget = pTgt + nHeight * 4 * nXStart + nYStart * 4;
    uint8* pStartYPtr = pTarget;
    int nLoopX = Helpers::DoPadding(nBlockWidth, simdPixelsPerRegister) / simdPixelsPerRegister;
    int nTargetIncrement = simdPixelsPerRegister * nIncTargetLine - 2;

    for (int i = 0; i < nBlockHeight; i++) {
        for (int j = 0; j < nLoopX; j++) {
            pSource = RotateLineToDIB_1(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget++;
            pSource = RotateLineToDIB(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
            pTarget++;
            pSource = RotateLineToDIB(pSource, pTarget, nIncTargetLine, simdPixelsPerRegister);
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
    CXMMImage* targetImage = new CXMMImage(pSourceImg->GetHeight(), pSourceImg->GetWidth(), true, simdPixelsPerRegister);
    if (targetImage->AlignedPtr() == NULL) {
        delete targetImage;
        return NULL;
    }
    const float* pSource = (const float*) pSourceImg->AlignedPtr();
    float* pTarget = (float*) targetImage->AlignedPtr();

    const int cnBlockSize = 32;
    int nX = 0, nY = 0;
    while (nY < pSourceImg->GetHeight()) {
        nX = 0;
        while (nX < pSourceImg->GetWidth()) {
            RotateBlock(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
                nX, nY, 
                std::min(cnBlockSize, pSourceImg->GetPaddedWidth() - nX), // !! here we need to use the padded width
                std::min(cnBlockSize, pSourceImg->GetHeight() - nY),
                simdPixelsPerRegister);
            nX += cnBlockSize;
        }
        nY += cnBlockSize;
    }

    return targetImage;
}

// RotateFlip the source image by 90 deg and return rotated image as 32 bpp DIB
static void* RotateToDIB(const CXMMImage* pSourceImg, int simdPixelsPerRegister, uint8* pTarget = NULL) {

    const float* pSource = (const float*) pSourceImg->AlignedPtr();
    if (pTarget == NULL) {
        pTarget = new(std::nothrow) uint8[pSourceImg->GetHeight() * 4 * Helpers::DoPadding(pSourceImg->GetWidth(), simdPixelsPerRegister)];
        if (pTarget == NULL) return NULL;
    }

    const int cnBlockSize = 32;
    int nX = 0, nY = 0;
    while (nY < pSourceImg->GetHeight()) {
        nX = 0;
        while (nX < pSourceImg->GetWidth()) {
            RotateBlockToDIB(pSource, pTarget, pSourceImg->GetWidth(), pSourceImg->GetHeight(),
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

CXMMImage* ApplyFilter_AVX(int nSourceHeight, int nTargetHeight, int nWidth,
    int nStartY_FP, int nStartX, int nIncrementY_FP,
    const AVXFilterKernelBlock& filter,
    int nFilterOffset, const CXMMImage* pSourceImg, bool bRoundResult) {

    int nStartXAligned = nStartX & ~7;
    int nEndXAligned = (nStartX + nWidth + 7) & ~7;
    CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, 8);
    if (tempImage->AlignedPtr() == NULL) {
        delete tempImage;
        return NULL;
    }

    int nCurY = nStartY_FP;
    int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(float);
    int nRowLenBytes = nChannelLenBytes * 3;
    int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 3;

    const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(float);
    AVXFilterKernel** pKernelIndexStart = filter.Indices;

    const __m256 ymmZero = _mm256_setzero_ps();
    const __m256 ymmMax = _mm256_set1_ps(4095.0f); // (Wert entsprechend YMM255)

    __m256* pDestination = (__m256*)tempImage->AlignedPtr();

    for (int y = 0; y < nTargetHeight; y++) {
        uint32 nCurYInt = (uint32)nCurY >> 16;
        int filterIndex = y + nFilterOffset;
        AVXFilterKernel* pKernel = pKernelIndexStart[filterIndex];
        int filterLen = pKernel->FilterLen;
        int filterOffset = pKernel->FilterOffset;
        const __m256* pFilterStart = (__m256*) & (pKernel->Kernel);
        const __m256* pSourceRow = (const __m256*)(pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes);

        for (int x = 0; x < nNumberOfBlocksX; x++) {
            const __m256* pFilter = pFilterStart;

            const uint8* pR = (const uint8*)pSourceRow;
            const uint8* pG = pR + nChannelLenBytes;
            const uint8* pB = pG + nChannelLenBytes;

            __m256 ymmR = _mm256_setzero_ps();
            __m256 ymmG = _mm256_setzero_ps();
            __m256 ymmB = _mm256_setzero_ps();

            for (int i = 0; i < filterLen; i++) {
                __m256 ymmKernel = *pFilter++;

                // Fused Multiply-Add: ymmR += (*pR) * ymmKernel
                ymmR = _mm256_fmadd_ps(*((const __m256*)pR), ymmKernel, ymmR);
                ymmG = _mm256_fmadd_ps(*((const __m256*)pG), ymmKernel, ymmG);
                ymmB = _mm256_fmadd_ps(*((const __m256*)pB), ymmKernel, ymmB);

                pR += nRowLenBytes;
                pG += nRowLenBytes;
                pB += nRowLenBytes;
            }

            if (bRoundResult) {
                // Clamping [0, 4095]
                ymmR = _mm256_min_ps(_mm256_max_ps(ymmR, ymmZero), ymmMax);
                ymmG = _mm256_min_ps(_mm256_max_ps(ymmG, ymmZero), ymmMax);
                ymmB = _mm256_min_ps(_mm256_max_ps(ymmB, ymmZero), ymmMax);

                ymmR = _mm256_round_ps(ymmR, _MM_FROUND_TO_NEAREST_INT);
                ymmG = _mm256_round_ps(ymmG, _MM_FROUND_TO_NEAREST_INT);
                ymmB = _mm256_round_ps(ymmB, _MM_FROUND_TO_NEAREST_INT);
            }

            *pDestination++ = ymmR;
            *pDestination++ = ymmG;
            *pDestination++ = ymmB;

            pSourceRow++;
        }

        nCurY += nIncrementY_FP;
    }

    return tempImage;
}

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
    if (tempImage->AlignedPtr() == NULL) {
        delete tempImage;
        return NULL;
    }

    int nCurY = nStartY_FP;
    int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(float);
    int nRowLenBytes = nChannelLenBytes * 3;
    int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> 2;
    const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(float);
    XMMFilterKernel** pKernelIndexStart = filter.Indices;

    _MM_ALIGN16 float XMM255[4] = {4095.0,  4095.0, 4095.0, 4095.0};

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
        uint32 nCurYInt = (uint32)nCurY >> 16; // integer part of Y
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
                pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

                // the pixel data GREEN channel
                xmm3 = *pSource;
                xmm3 = _mm_mul_ps(xmm3, xmm7);
                xmm5 = _mm_add_ps(xmm5, xmm3);
                pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

                // the pixel data BLUE channel
                xmm2 = *pSource;
                xmm2 = _mm_mul_ps(xmm2, xmm7);
                xmm6 = _mm_add_ps(xmm6, xmm2);
                pSource = (__m128*)((uint8*)pSource + nChannelLenBytes);

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

void* SampleDown_HQ_MMX_SSE_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
                                 CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, bool bSSE, uint8* pTarget) {

    CAutoXMMFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
    const XMMFilterKernelBlock& kernelsY = filterY.Kernels();
    CAutoXMMFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
    const XMMFilterKernelBlock& kernelsX = filterX.Kernels();

    uint32 nIncrementX = (uint32)(sourceSize.cx << 16)/fullTargetSize.cx + 1;
    uint32 nIncrementY = (uint32)(sourceSize.cy << 16)/fullTargetSize.cy + 1;

    int nIncOffsetX = (nIncrementX - 65536) >> 1;
    int nIncOffsetY = (nIncrementY - 65536) >> 1;
    int nFirstX = (uint32)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
    nFirstX = std::max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
    int nLastX  = (uint32)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
    XMMFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
    nLastX  = std::min<int>(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
    int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
    nFirstY = std::max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
    int nLastY  = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
    XMMFilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
    nLastY  = std::min<int>(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536*nFirstX;
    int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536*nFirstY;

    // Create
    CXMMImage* pImage1 = new CXMMImage(sourceSize.cx, sourceSize.cy, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 4);
    if (pImage1->AlignedPtr() == NULL) {
        delete pImage1;
        return NULL;
    }

    // Resize Y
    CXMMImage* pImage2 = ApplyFilter_SSE(pImage1->GetHeight(), clippedTargetSize.cy, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == NULL) return NULL;

    // Rotate
    CXMMImage* pImage3 = Rotate(pImage2, 4);
    delete pImage2;
    if (pImage3 == NULL) return NULL;

    // Resize Y again
    CXMMImage* pImage4 = ApplyFilter_SSE(pImage3->GetHeight(), clippedTargetSize.cx, clippedTargetSize.cy, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == NULL) return NULL;

    // Rotate back
    void* pTargetDIB = RotateToDIB(pImage4, 4, pTarget);

    delete pImage4;
    return pTargetDIB;
}

void* SampleDown_HQ_AVX_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
                             CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, uint8* pTarget) {

    CAutoAVXFilter filterY(sourceSize.cy, fullTargetSize.cy, eFilter);
    const AVXFilterKernelBlock& kernelsY = filterY.Kernels();
    CAutoAVXFilter filterX(sourceSize.cx, fullTargetSize.cx, eFilter);
    const AVXFilterKernelBlock& kernelsX = filterX.Kernels();

    uint32 nIncrementX = (uint32)(sourceSize.cx << 16) / fullTargetSize.cx + 1;
    uint32 nIncrementY = (uint32)(sourceSize.cy << 16) / fullTargetSize.cy + 1;

    int nIncOffsetX = (nIncrementX - 65536) >> 1;
    int nIncOffsetY = (nIncrementY - 65536) >> 1;
    int nFirstX = (uint32)(nIncOffsetX + nIncrementX*fullTargetOffset.x) >> 16;
    nFirstX = std::max(0, nFirstX - kernelsX.Indices[fullTargetOffset.x]->FilterOffset);
    int nLastX = (uint32)(nIncOffsetX + nIncrementX*(fullTargetOffset.x + clippedTargetSize.cx - 1)) >> 16;
    AVXFilterKernel* pLastXFilter = kernelsX.Indices[fullTargetOffset.x + clippedTargetSize.cx - 1];
    nLastX = std::min<int>(sourceSize.cx - 1, nLastX - pLastXFilter->FilterOffset + pLastXFilter->FilterLen - 1);
    int nFirstY = (uint32)(nIncOffsetY + nIncrementY*fullTargetOffset.y) >> 16;
    nFirstY = std::max(0, nFirstY - kernelsY.Indices[fullTargetOffset.y]->FilterOffset);
    int nLastY = (uint32)(nIncOffsetY + nIncrementY*(fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16;
    AVXFilterKernel* pLastYFilter = kernelsY.Indices[fullTargetOffset.y + clippedTargetSize.cy - 1];
    nLastY = std::min<int>(sourceSize.cy - 1, nLastY - pLastYFilter->FilterOffset + pLastYFilter->FilterLen - 1);
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncOffsetX + nIncrementX*fullTargetOffset.x - 65536 * nFirstX;
    int nStartY = nIncOffsetY + nIncrementY*fullTargetOffset.y - 65536 * nFirstY;

    // Create
    CXMMImage* pImage1 = new CXMMImage(sourceSize.cx, sourceSize.cy, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 8);
    if (pImage1->AlignedPtr() == NULL) {
        delete pImage1;
        return NULL;
    }

    // Resize Y
    CXMMImage* pImage2 = ApplyFilter_AVX(pImage1->GetHeight(), clippedTargetSize.cy, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == NULL) return NULL;

    // Rotate
    CXMMImage* pImage3 = Rotate(pImage2, 8);
    delete pImage2;
    if (pImage3 == NULL) return NULL;

    // Resize Y again
    CXMMImage* pImage4 = ApplyFilter_AVX(pImage3->GetHeight(), clippedTargetSize.cx, clippedTargetSize.cy, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == NULL) return NULL;

    // Rotate back
    void* pTargetDIB = RotateToDIB(pImage4, 8, pTarget);

    delete pImage4;
    return pTargetDIB;
}

void* SampleUp_HQ_MMX_SSE_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pPixels, int nChannels, bool bSSE, uint8* pTarget) {
    int nTargetWidth = clippedTargetSize.cx;
    int nTargetHeight = clippedTargetSize.cy;
    int nSourceWidth = sourceSize.cx;
    int nSourceHeight = sourceSize.cy;

    uint32 nIncrementX = (uint32)(65536*(uint32)(nSourceWidth - 1)/(fullTargetSize.cx - 1));
    uint32 nIncrementY = (uint32)(65536*(uint32)(nSourceHeight - 1)/(fullTargetSize.cy - 1));

    int nFirstX = std::max(0, int((uint32)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
    int nLastX = std::min<int>(sourceSize.cx - 1, ((uint32)(nIncrementX*(fullTargetOffset.x + nTargetWidth - 1)) >> 16) + 2);
    int nFirstY = std::max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
    int nLastY = std::min<int>(sourceSize.cy - 1, ((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2);
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
    if (pImage1->AlignedPtr() == NULL) {
        delete pImage1;
        return NULL;
    }
    CXMMImage* pImage2 = ApplyFilter_SSE(pImage1->GetHeight(), nTargetHeight, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == NULL) return NULL;
    CXMMImage* pImage3 = Rotate(pImage2, 4);
    delete pImage2;
    if (pImage3 == NULL) return NULL;
    CXMMImage* pImage4 = ApplyFilter_SSE(pImage3->GetHeight(), nTargetWidth, nTargetHeight, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == NULL) return NULL;
    void* pTargetDIB = RotateToDIB(pImage4, 4, pTarget);
    delete pImage4;
    
    return pTargetDIB;
}

void* SampleUp_HQ_AVX_Core(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pPixels, int nChannels, uint8* pTarget) {

    int nTargetWidth = clippedTargetSize.cx;
    int nTargetHeight = clippedTargetSize.cy;
    int nSourceWidth = sourceSize.cx;
    int nSourceHeight = sourceSize.cy;

    uint32 nIncrementX = (uint32)(65536 * (uint32)(nSourceWidth - 1) / (fullTargetSize.cx - 1));
    uint32 nIncrementY = (uint32)(65536 * (uint32)(nSourceHeight - 1) / (fullTargetSize.cy - 1));

    int nFirstX = std::max(0, int((uint32)(nIncrementX*fullTargetOffset.x) >> 16) - 1);
    int nLastX  = std::min<int>(sourceSize.cx - 1, ((uint32)(nIncrementX*(fullTargetOffset.x + nTargetWidth - 1)) >> 16) + 2);
    int nFirstY = std::max(0, int((uint32)(nIncrementY*fullTargetOffset.y) >> 16) - 1);
    int nLastY  = std::min<int>(sourceSize.cy - 1, ((uint32)(nIncrementY*(fullTargetOffset.y + nTargetHeight - 1)) >> 16) + 2);
    //int nFirstTargetWidth = nLastX - nFirstX + 1;
    //int nFirstTargetHeight = nTargetHeight;
    int nFilterOffsetX = fullTargetOffset.x;
    int nFilterOffsetY = fullTargetOffset.y;
    int nStartX = nIncrementX*fullTargetOffset.x - 65536 * nFirstX;
    int nStartY = nIncrementY*fullTargetOffset.y - 65536 * nFirstY;

    CAutoAVXFilter filterY(nSourceHeight, fullTargetSize.cy, Filter_Upsampling_Bicubic);
    const AVXFilterKernelBlock& kernelsY = filterY.Kernels();

    CAutoAVXFilter filterX(nSourceWidth, fullTargetSize.cx, Filter_Upsampling_Bicubic);
    const AVXFilterKernelBlock& kernelsX = filterX.Kernels();

    // Resize Y
    CXMMImage* pImage1 = new CXMMImage(nSourceWidth, nSourceHeight, nFirstX, nLastX, nFirstY, nLastY, pPixels, nChannels, 8);
    if (pImage1->AlignedPtr() == NULL) {
        delete pImage1;
        return NULL;
    }
    CXMMImage* pImage2 = ApplyFilter_AVX(pImage1->GetHeight(), nTargetHeight, pImage1->GetWidth(), nStartY, 0, nIncrementY, kernelsY, nFilterOffsetY, pImage1, false);
    delete pImage1;
    if (pImage2 == NULL) return NULL;
    CXMMImage* pImage3 = Rotate(pImage2, 8);
    delete pImage2;
    if (pImage3 == NULL) return NULL;
    CXMMImage* pImage4 = ApplyFilter_AVX(pImage3->GetHeight(), nTargetWidth, nTargetHeight, nStartX, 0, nIncrementX, kernelsX, nFilterOffsetX, pImage3, true);
    delete pImage3;
    if (pImage4 == NULL) return NULL;
    void* pTargetDIB = RotateToDIB(pImage4, 8, pTarget);
    delete pImage4;

    return pTargetDIB;
}

void* CBasicProcessing::SampleDown_HQ_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
                                           CSize sourceSize, const void* pPixels, int nChannels, EFilterType eFilter, SIMDArchitecture simd) {

    if (pPixels == NULL || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return NULL;
    }
    
    int padding = (simd == AVX2) ? 8 : 4;
    uint8* pTarget = new(std::nothrow) uint8[clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
    if (pTarget == NULL) return NULL;
    CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
    CRequestUpDownSampling request(pPixels, sourceSize,
        pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
        nChannels, eFilter, simd);
    bool bSuccess = threadPool.Process(&request);

    return bSuccess ? pTarget : NULL;
}

void* CBasicProcessing::SampleUp_HQ_SIMD(CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize,
    CSize sourceSize, const void* pPixels, int nChannels, SIMDArchitecture simd) {
    if (pPixels == NULL || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0) {
        return NULL;
    }
    int padding = (simd == AVX2) ? 8 : 4;
    uint8* pTarget = new(std::nothrow) uint8[clippedTargetSize.cx * 4 * Helpers::DoPadding(clippedTargetSize.cy, padding)];
    if (pTarget == NULL) return NULL;
    CProcessingThreadPool& threadPool = CProcessingThreadPool::This();
    CRequestUpDownSampling request(pPixels, sourceSize,
        pTarget, fullTargetSize, fullTargetOffset, clippedTargetSize,
        nChannels, Filter_Upsampling_Bicubic, simd);
    bool bSuccess = threadPool.Process(&request);

    return bSuccess ? pTarget : NULL;
}

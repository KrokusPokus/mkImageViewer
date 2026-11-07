#include "BaseImage.h"

#include "BasicProcessing.h"
#include "EXIFReader.h"
#include "Helpers.h"
#include "HistogramCorr.h"
#include "SettingsProvider.h"

#include <QColor>
#include <QDebug>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

///////////////////////////////////////////////////////////////////////////////////
// Static helpers
///////////////////////////////////////////////////////////////////////////////////

static bool SupportsSIMD(Helpers::CPUType cpuType) {
    switch (cpuType)
    {
    case Helpers::CPU_MMX:
    case Helpers::CPU_SSE:
    case Helpers::CPU_AVX2:
        return true;
    default:
        return false;
    }
}

static CBasicProcessing::SIMDArchitecture ToSIMDArchitecture(Helpers::CPUType cpuType) {
    switch (cpuType)
    {
    case Helpers::CPU_MMX:
        return CBasicProcessing::MMX;
    case Helpers::CPU_SSE:
        return CBasicProcessing::SSE;
    case Helpers::CPU_AVX2:
        return CBasicProcessing::AVX2;
    default:
        assert(false);
        return (CBasicProcessing::SIMDArchitecture)(-1);
    }
}

///////////////////////////////////////////////////////////////////////////////////
// Public interface
///////////////////////////////////////////////////////////////////////////////////

CBaseImage::CBaseImage(int nWidth, int nHeight, void* pPixels, void* pEXIFData, int nChannels,
                    EImageFormat eImageFormat, bool bIsAnimation, int nFrameIndex, int nNumberOfFrames, int nFrameTimeMs, bool bInArchive)
    : m_rotationParams{ 0 },
    m_bInArchive(bInArchive)
{
    if (nChannels == 3 || nChannels == 4) {
        m_pOrigPixels = pPixels;
        m_nOriginalChannels = nChannels;
    } else if (nChannels == 1) {
        m_pOrigPixels = CBasicProcessing::Convert1To4Channels(nWidth, nHeight, pPixels);
        delete[] static_cast<uint8_t*>(pPixels);
        m_nOriginalChannels = 4;
    } else {
        assert(false);
        delete[] static_cast<uint8_t*>(pPixels);
        m_pOrigPixels = nullptr;
        m_nOriginalChannels = 0;
    }

    if (pEXIFData != nullptr) {
        unsigned char * pEXIF = (unsigned char *)pEXIFData;
        m_nEXIFSize = pEXIF[2]*256 + pEXIF[3] + 2;
        m_pEXIFData = new char[m_nEXIFSize];
        memcpy(m_pEXIFData, pEXIFData, m_nEXIFSize);
        m_pEXIFReader = new CEXIFReader(m_pEXIFData, m_nEXIFSize, eImageFormat);
    } else {
        m_nEXIFSize = 0;
        m_pEXIFData = nullptr;
        m_pEXIFReader = nullptr;
    }

    m_eImageFormat = eImageFormat;
    m_bIsAnimation = bIsAnimation;
// For containers like CBZ, the meaning of "bIsAnimation" is actually "bMultipleFiles"
// It shouldn't really be needed for anything, since that check is done in ContainerHasMultipleImages with (m_nNumberOfFrames > 1)
// Setting m_bIsAnimation to "false" fixes several issues, all caused by IsAnimation() checks.
    if (m_bInArchive) {
        m_bIsAnimation = false;
    }
    m_nFrameIndex = nFrameIndex;
    m_nNumberOfFrames = nNumberOfFrames;
    m_nFrameTimeMs = nFrameTimeMs;

    m_nOrigWidth = m_nInitOrigWidth = nWidth;
    m_nOrigHeight = m_nInitOrigHeight = nHeight;
    m_pBitmapPixels = nullptr;
    m_pBitmapPixelsLUTProcessed = nullptr;
    m_pLastBitmap = nullptr;
    
    m_pLUTRGB = nullptr;
    m_pSaturationLUTs = nullptr;
    m_eProcFlags = PFLAG_None;
    m_nInitialRotation = 0;
    m_dInitialZoom = -1;
    m_initialOffsets = DPoint(0, 0);

    m_bCropped = false;
    m_bRotationByEXIF = false;
    m_bFirstReprocessing = true;
    m_bitmapLoadDelayMs = 0;
    m_fileLoadDelayMs = 0;
    m_FullTargetSize = DSize(0, 0);
    m_ClippingSize = DSize(0, 0);
    m_TargetOffset = DPoint(0, 0);

    // Histogram of the original pixels for the automatic correction (computed here, i.e. in the load thread)
    m_pHistogram = new CHistogram(*this);}

CBaseImage::~CBaseImage() {
    delete[] static_cast<uint8_t*>(m_pOrigPixels);
    m_pOrigPixels = nullptr;
    delete[] static_cast<uint8_t*>(m_pBitmapPixels);
    m_pBitmapPixels = nullptr;
    delete[] static_cast<uint8_t*>(m_pBitmapPixelsLUTProcessed);
    m_pBitmapPixelsLUTProcessed = nullptr;
    delete[] m_pLUTRGB;
    m_pLUTRGB = nullptr;
    delete[] m_pSaturationLUTs;
    m_pSaturationLUTs = nullptr;
    delete m_pHistogram;
    m_pHistogram = nullptr;
    m_pLastBitmap = nullptr;
    delete[] static_cast<uint8_t*>(m_pEXIFData);
    m_pEXIFData = nullptr;
    delete m_pEXIFReader;
    m_pEXIFReader = nullptr;
}

void CBaseImage::ResampleWithPan(void* & pBitmapPixels, void* & pBitmapPixelsLUTProcessed, DSize fullTargetSize,
                                 DSize clippingSize, DPoint targetOffset, DRect oldClippingRect,
                                 EProcessingFlags eProcFlags, const CImageProcessingParams & imageProcParams, 
                                 double dRotation, EResizeType eResizeType) {
    DPoint oldOffset = oldClippingRect.TopLeft();
    DSize oldSize = oldClippingRect.Size();
    DRect newClippingRect = DRect(targetOffset, clippingSize);
    DRect sourceRect;
    if (sourceRect.IntersectRect(oldClippingRect, newClippingRect)) {
        // there is an intersection, reuse the non LUT processed bitmap
        sourceRect.OffsetRect(-oldOffset.x, -oldOffset.y);
        DRect targetRect = DRect(DPoint(std::max<long>(0, oldOffset.x - newClippingRect.left), std::max<long>(0, oldOffset.y - newClippingRect.top)),
            DSize(sourceRect.Width(), sourceRect.Height()));

        bool bCanUseLUTProcBitmap = ApplyCorrectionLUT(imageProcParams, eProcFlags, pBitmapPixelsLUTProcessed, fullTargetSize,
            targetOffset, pBitmapPixels, clippingSize, true, false) != nullptr;

        // the LUT processed pixels cannot be used and the original pixels are not available -
        // full recreation of bitmaps is needed
        if (!bCanUseLUTProcBitmap && pBitmapPixels == nullptr) {
            delete[] static_cast<uint8_t*>(pBitmapPixelsLUTProcessed); pBitmapPixelsLUTProcessed = nullptr;
            return;
        }

        // Copy the reusable part of original bitmap pixels
        void* pPannedPixels = (bCanUseLUTProcBitmap == false) ? 
            CBasicProcessing::CopyRect32bpp(nullptr, pBitmapPixels, clippingSize, targetRect, oldSize, sourceRect) :
            nullptr;

        // get rid of original bitmap, will be recreated automatically when needed
        delete[] static_cast<uint8_t*>(pBitmapPixels); pBitmapPixels = nullptr;

        // Copy the reusable part of processed bitmap pixels
        void* pPannedPixelsLUTProcessed = bCanUseLUTProcBitmap ? 
            CBasicProcessing::CopyRect32bpp(nullptr, pBitmapPixelsLUTProcessed, clippingSize, targetRect, oldSize, sourceRect) :
            nullptr;

        // Delete old LUT processed bitmap, we copied the part that can be reused to a new bitmap (pPannedPixelsLUTProcessed)
        delete[] static_cast<uint8_t*>(pBitmapPixelsLUTProcessed); pBitmapPixelsLUTProcessed = nullptr;

        if (targetRect.top > 0) {
            DSize clipSize(clippingSize.cx, targetRect.top);
            void* pTop = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcBitmap) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pTop,
                    clippingSize, DRect(DPoint(0, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
            } else {
                void* pTopProc = nullptr;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pTopProc, fullTargetSize, targetOffset, pTop, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pTopProc,
                    clippingSize, DRect(DPoint(0, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pTopProc);
            }

            delete[] static_cast<uint8_t*>(pTop);
        }
        if (targetRect.bottom < clippingSize.cy) {
            DSize clipSize(clippingSize.cx, clippingSize.cy -  targetRect.bottom);
            DPoint offset(targetOffset.x, targetOffset.y + targetRect.bottom);
            void* pBottom = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcBitmap) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pBottom,
                    clippingSize, DRect(DPoint(0, targetRect.bottom), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
            } else {
                void* pBottomProc = nullptr;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pBottomProc, fullTargetSize, offset, pBottom, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pBottomProc,
                    clippingSize, DRect(DPoint(0, targetRect.bottom), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pBottomProc);
            }

            delete[] static_cast<uint8_t*>(pBottom);
        }
        if (targetRect.left > 0) {
            DSize clipSize(targetRect.left, clippingSize.cy);
            void* pLeft = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcBitmap) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pLeft,
                    clippingSize, DRect(DPoint(0, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
            } else {
                void* pLeftProc = nullptr;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pLeftProc, fullTargetSize, targetOffset, pLeft, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pLeftProc,
                    clippingSize, DRect(DPoint(0, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pLeftProc);
            }

            delete[] static_cast<uint8_t*>(pLeft);
        }
        if (targetRect.right < clippingSize.cx) {
            DSize clipSize(clippingSize.cx -  targetRect.right, clippingSize.cy);
            DPoint offset(targetOffset.x + targetRect.right, targetOffset.y);
            void* pRight = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcBitmap) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pRight,
                    clippingSize, DRect(DPoint(targetRect.right, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
            } else {
                void* pRigthProc = nullptr;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pRigthProc, fullTargetSize, offset, pRight, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pRigthProc,
                    clippingSize, DRect(DPoint(targetRect.right, 0), clipSize),
                    clipSize, DRect(DPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pRigthProc);
            }

            delete[] static_cast<uint8_t*>(pRight);
        }
        pBitmapPixels = pPannedPixels;
        pBitmapPixelsLUTProcessed = pPannedPixelsLUTProcessed;
        return;
    }

    delete[] static_cast<uint8_t*>(pBitmapPixels); pBitmapPixels = nullptr;
    delete[] static_cast<uint8_t*>(pBitmapPixelsLUTProcessed); pBitmapPixelsLUTProcessed = nullptr;
}

void* CBaseImage::Resample(DSize fullTargetSize, DSize clippingSize, DPoint targetOffset, EProcessingFlags eProcFlags, EResizeType eResizeType) {

    Helpers::CPUType cpu = CSettingsProvider::This().AlgorithmImplementation();
    EFilterType filter = CSettingsProvider::This().DownsamplingFilter();

    if (fullTargetSize.cx > 65535 || fullTargetSize.cy > 65535) return nullptr;

    if (GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling) && !(eResizeType == NoResize) && (filter>0)) {
        if (SupportsSIMD(cpu)) {
            if (eResizeType == UpSample) {
                return CBasicProcessing::SampleUp_HQ_SIMD(fullTargetSize, targetOffset, clippingSize, 
                    DSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, ToSIMDArchitecture(cpu));
            } else {
                return CBasicProcessing::SampleDown_HQ_SIMD(fullTargetSize, targetOffset, clippingSize,
                    DSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter, ToSIMDArchitecture(cpu));
            }
        } else {
            if (eResizeType == UpSample) {
                return CBasicProcessing::SampleUp_HQ(fullTargetSize, targetOffset, clippingSize, 
                    DSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
            } else {
                return CBasicProcessing::SampleDown_HQ(fullTargetSize, targetOffset, clippingSize, 
                    DSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter);
            }
        }
    } else {
        return CBasicProcessing::PointSample(fullTargetSize, targetOffset, clippingSize,
            DSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
    }
}

DPoint CBaseImage::ConvertOffset(DSize fullTargetSize, DSize clippingSize, DPoint targetOffset) {
    int nStartX = (fullTargetSize.cx - clippingSize.cx)/2 - targetOffset.x;
    int nStartY = (fullTargetSize.cy - clippingSize.cy)/2 - targetOffset.y;
    return DPoint(nStartX, nStartY);
}

bool CBaseImage::VerifyRotation(const CRotationParams& rotationParams) {
    if (rotationParams.Mirrored != m_rotationParams.Mirrored && !Mirror(true)) {
        return false;
    }
    int nDiff = ((rotationParams.Rotation - m_rotationParams.Rotation) + 360) % 360;
    if (nDiff != 0) {
        if (!Rotate(nDiff)) return false;
    }
    return true;
}

bool CBaseImage::Rotate(int nRotation) {
    InvalidateAllCachedPixelData();
    void* pNewOriginalPixels = (m_nOriginalChannels == 3)
        ? CBasicProcessing::Rotate24bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels, nRotation)
        : CBasicProcessing::Rotate32bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels, nRotation);
    if (pNewOriginalPixels == nullptr) return false;
    delete[] static_cast<uint8_t*>(m_pOrigPixels);
    m_pOrigPixels = pNewOriginalPixels;
    if (nRotation != 180) {
        // swap width and height
        int nTemp = m_nOrigWidth;
        m_nOrigWidth = m_nOrigHeight;
        m_nOrigHeight = nTemp;
    }
    m_rotationParams.Rotation = (m_rotationParams.Rotation + nRotation) % 360;
    return true;
}

bool CBaseImage::Mirror(bool bHorizontally) {
    InvalidateAllCachedPixelData();
    void* pNewOriginalPixels = (m_nOriginalChannels == 3)
        ? CBasicProcessing::Mirror24bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels, bHorizontally)
        : CBasicProcessing::Mirror32bpp(m_nOrigWidth, m_nOrigHeight, m_pOrigPixels, bHorizontally);
    if (pNewOriginalPixels == nullptr) return false;
    delete[] static_cast<uint8_t*>(m_pOrigPixels);
    m_pOrigPixels = pNewOriginalPixels;
    m_rotationParams.Rotation = CRotationParams::RotationAfterMirror(m_rotationParams.Rotation, bHorizontally);
    m_rotationParams.Mirrored = !m_rotationParams.Mirrored;
    return true;
}

void* CBaseImage::BitmapPixelsLastProcessed(bool bGenerateBitmapIfNeeded) {
    if (bGenerateBitmapIfNeeded && m_pLastBitmap == nullptr) {
        m_pLastBitmap = GetBitmap(m_FullTargetSize, m_ClippingSize, m_TargetOffset, m_imageProcParams, m_eProcFlags);
    }
    return m_pLastBitmap;
}

void CBaseImage::VerifyBitmapPixelsCreated() {
    if (m_pBitmapPixels == nullptr) {
        EResizeType eResizeType = GetResizeType(m_FullTargetSize, DSize(m_nOrigWidth, m_nOrigHeight));
        m_pBitmapPixels = Resample(m_FullTargetSize, m_ClippingSize, m_TargetOffset, m_eProcFlags, eResizeType);
    }
}

void CBaseImage::RestoreInitialParameters(const CImageProcessingParams& imageProcParams, int nRotation, double dZoom,
                                          DPoint offsets) {
    m_dInitialZoom = dZoom;
    m_initialOffsets = offsets;
    m_nInitialRotation = GetRotationFromEXIF(nRotation);
    m_imageProcParamsInitial = imageProcParams;
}

void CBaseImage::SetInitialProcessParams(CProcessParams* pParams) {
    pParams->RotationParams.Rotation = GetRotationFromEXIF(pParams->RotationParams.Rotation);

    m_nInitialRotation = pParams->RotationParams.Rotation;
    m_dInitialZoom = pParams->Zoom;
    m_initialOffsets = pParams->Offsets;
    m_imageProcParamsInitial = pParams->ImageProcParams;
}



///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void* CBaseImage::GetBitmapInternal(DSize fullTargetSize, DSize clippingSize, DPoint targetOffset,
                         const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
                         bool &bParametersChanged) {

    // Check if resampling due to bHighQualityResampling parameter change is needed
    bool bMustResampleQuality = GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling) != GetProcessingFlag(m_eProcFlags, PFLAG_HighQualityResampling);
    bool bTargetSizeChanged = fullTargetSize != m_FullTargetSize;

    // Check if resampling due to change of geometric parameters is needed
    bool bMustResampleGeometry = bTargetSizeChanged || clippingSize != m_ClippingSize || targetOffset != m_TargetOffset;

    qDebug() << "GetBitmap:" << "HQchanged" << bMustResampleQuality
             << "targetChanged" << bTargetSizeChanged
             << "clipChanged" << (clippingSize != m_ClippingSize)
             << "offsetChanged" << (targetOffset != m_TargetOffset)
             << "new full/clip/off:" << fullTargetSize.cx << fullTargetSize.cy << "/" << clippingSize.cx << clippingSize.cy
             << "/" << targetOffset.x << targetOffset.y
             << "old:" << m_FullTargetSize.cx << m_FullTargetSize.cy << "/" << m_ClippingSize.cx << m_ClippingSize.cy
             << "/" << m_TargetOffset.x << m_TargetOffset.y;

    EResizeType eResizeType = GetResizeType(fullTargetSize, DSize(m_nOrigWidth, m_nOrigHeight));

    // the geometrical parameters must be set before calling ApplyCorrectionLUT()
    DRect oldClippingRect = DRect(m_TargetOffset, m_ClippingSize);
    m_FullTargetSize = fullTargetSize;
    m_ClippingSize = clippingSize;
    m_TargetOffset = targetOffset;

    uint64_t startTimeMs = Helpers::GetTimeMs();

    // Check if only the LUT must be reapplied but no resampling (resampling is much slower than the LUTs)
    void * pBitmap = nullptr;
    if (!bMustResampleQuality && !bMustResampleGeometry) {
        pBitmap = ApplyCorrectionLUT(imageProcParams, eProcFlags, m_pBitmapPixelsLUTProcessed, 
            fullTargetSize, targetOffset, m_pBitmapPixels, clippingSize, false, false, bParametersChanged);
    }

    // ApplyCorrectionLUT() could have failed, then recreate the bitmaps
    if (pBitmap == nullptr) {
        // The original is kept with 3 channels: resampling is equally fast for 3 and 4 channels, a conversion
        // would cost time and memory (rotation and mirroring also work on 3 channels)

        bParametersChanged = true;

        // If we only pan, we can resample far more efficiently by only calculating the newly visible areas
        bool bPanningOnly = !m_bFirstReprocessing && !bTargetSizeChanged && !bMustResampleQuality;

        m_bFirstReprocessing = false;
        if (bPanningOnly) {
            ResampleWithPan(m_pBitmapPixels, m_pBitmapPixelsLUTProcessed, fullTargetSize, clippingSize, targetOffset, 
                oldClippingRect, eProcFlags, imageProcParams, 0.0, eResizeType);
        } else {
            delete[] static_cast<uint8_t*>(m_pBitmapPixelsLUTProcessed); m_pBitmapPixelsLUTProcessed = nullptr;
            delete[] static_cast<uint8_t*>(m_pBitmapPixels); m_pBitmapPixels = nullptr;
        }

        // both bitmaps are nullptr, do normal resampling
        if (m_pBitmapPixels == nullptr && m_pBitmapPixelsLUTProcessed == nullptr) {
            m_pBitmapPixels = Resample(fullTargetSize, clippingSize, targetOffset, eProcFlags, eResizeType);
        }

        // if ResampleWithPan() has preserved this bitmap, we can reuse it
        if (m_pBitmapPixelsLUTProcessed == nullptr) {
            pBitmap = ApplyCorrectionLUT(imageProcParams, eProcFlags, m_pBitmapPixelsLUTProcessed, fullTargetSize,
                targetOffset, m_pBitmapPixels, clippingSize, false, false);
        } else {
            pBitmap = m_pBitmapPixelsLUTProcessed;
        }
    }

    uint64_t bitmapLoadDelayMs = Helpers::GetTimeMs() - startTimeMs; 
    if (bitmapLoadDelayMs > 1) {
        m_bitmapLoadDelayMs = bitmapLoadDelayMs;
    }

    // Only now, after ApplyCorrectionLUT(): it compares the new parameters with these to detect a change
    m_eProcFlags = eProcFlags;
    m_imageProcParams = imageProcParams;

    m_pLastBitmap = pBitmap;
    
    return pBitmap;
}

void* CBaseImage::ApplyCorrectionLUT(const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
                                           void * & pCachedTargetBitmap, DSize fullTargetSize, DPoint targetOffset, 
                                           void * pSourceBitmap, DSize bitmapSize,
                                           bool bOnlyCheck, bool bCanTakeOwnershipOfSourceBitmap, bool &bParametersChanged) {

    bool bAutoContrast = GetProcessingFlag(eProcFlags, PFLAG_AutoContrast);
    bool bAutoContrastOld = GetProcessingFlag(m_eProcFlags, PFLAG_AutoContrast);

    bool bMustUseSaturationLUTs = fabs(imageProcParams.Saturation - 1.0) > 1e-4;
    bool bNoLUTsApplied = !bAutoContrast && !bMustUseSaturationLUTs;
    bool bCorrectionFactorChanged = fabs(imageProcParams.ContrastCorrectionFactor-m_imageProcParams.ContrastCorrectionFactor) > 1e-4;
    bool bSaturationChanged = fabs(imageProcParams.Saturation - m_imageProcParams.Saturation) > 1e-4;
    bool bMustReapplyLUTs = bAutoContrast != bAutoContrastOld
                        || bCorrectionFactorChanged
                        || bSaturationChanged;
    bool bMustUse3ChannelLUT = bAutoContrast;

    bParametersChanged = bMustReapplyLUTs;

    if (!bMustReapplyLUTs && pCachedTargetBitmap != nullptr) {
        // consider special case that nothing is dimmed and no LUT is applied but
        // processed pixel is here, we do not want to use it - in this case we just continue.
        if (!bNoLUTsApplied) {
            return pCachedTargetBitmap;
        }
    }

    // If it shall only be checked if this method would be able to reuse the existing pCachedTargetBitmap, return
    if (bOnlyCheck) {
        return nullptr;
    }

    if (pSourceBitmap == nullptr) {
        return nullptr;
    }

    // Calculate the histogram if needed (after rotation)
    if (m_pHistogram == nullptr) {
        m_pHistogram = new CHistogram(*this);
    }

    // Recalculate special histogram if needed
    const CHistogram* pHistogram = m_pHistogram;
    bool bSpecialHistogram = false;
    
    if (bMustUse3ChannelLUT && (m_pLUTRGB == nullptr || bCorrectionFactorChanged || bAutoContrast != bAutoContrastOld)) {
        delete[] m_pLUTRGB;
        float fBrightnessCorrFactor = bAutoContrast ? 1.0f : 0.0f;
        float fContrastCorrFactor = bAutoContrast ? (float) imageProcParams.ContrastCorrectionFactor : 0.0f;
        m_pLUTRGB = CHistogramCorr::CalculateCorrectionLUT(*pHistogram, fBrightnessCorrFactor, fContrastCorrFactor);
    } else if (!bMustUse3ChannelLUT) {
        delete[] m_pLUTRGB;
        m_pLUTRGB = nullptr;
    }
    
    if (bMustUseSaturationLUTs && (m_pSaturationLUTs == nullptr || bSaturationChanged)) {
        delete[] m_pSaturationLUTs;
        m_pSaturationLUTs = CBasicProcessing::CreateColorSaturationLUTs(imageProcParams.Saturation);
    } else if (!bMustUseSaturationLUTs) {
        delete[] m_pSaturationLUTs;
        m_pSaturationLUTs = nullptr;
    }
    
    delete[] static_cast<uint8_t*>(pCachedTargetBitmap);
    pCachedTargetBitmap = nullptr;
    if (bSpecialHistogram) {
        delete pHistogram;
    }

    if (!bNoLUTsApplied) {
        // LUT --> apply correction. Without auto contrast only the saturation is changed (identity LUT).
        uint8_t identityLUT[3*256];
        const uint8_t* pLUT = m_pLUTRGB;
        if (pLUT == nullptr) {
            for (int i = 0; i < 256; i++) {
                identityLUT[i] = identityLUT[i + 256] = identityLUT[i + 256*2] = (uint8_t)i;
            }
            pLUT = identityLUT;
        }

        if (bMustUseSaturationLUTs) {
            pCachedTargetBitmap = CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(bitmapSize.cx, bitmapSize.cy, pSourceBitmap, m_pSaturationLUTs, pLUT);
        } else {
            pCachedTargetBitmap = CBasicProcessing::Apply3ChannelLUT32bpp(bitmapSize.cx, bitmapSize.cy, pSourceBitmap, pLUT);
        }
    } else if (bCanTakeOwnershipOfSourceBitmap) {
        // no LUTs, just take over ownership of source bitmap if we are allowed
        pCachedTargetBitmap = pSourceBitmap;
    } else {
        // no LUTs, no dimming --> return original pixels
        return pSourceBitmap;
    }

    return pCachedTargetBitmap;
}

CBaseImage::EResizeType CBaseImage::GetResizeType(DSize targetSize, DSize sourceSize) {
    if (targetSize.cx == sourceSize.cx && targetSize.cy == sourceSize.cy) {
        return NoResize;
    } else if (targetSize.cx <= sourceSize.cx && targetSize.cy <= sourceSize.cy) {
        return DownSample;
    } else {
        return UpSample;
    }
}

int CBaseImage::GetRotationFromEXIF(int nOrigRotation) {
    if (m_pEXIFReader != nullptr && m_pEXIFReader->ImageOrientationPresent() && CSettingsProvider::This().AutoRotateEXIF()) {
        switch (m_pEXIFReader->GetImageOrientation()) {
            case 1:
                m_bRotationByEXIF = true;
                return 0;
            case 3:
                m_bRotationByEXIF = true;
                return 180;
            case 6:
                m_bRotationByEXIF = true;
                return 90;
            case 8:
                m_bRotationByEXIF = true;
                return 270;
        }
    }

    return nOrigRotation;
}

void CBaseImage::InvalidateAllCachedPixelData() {
    m_pLastBitmap = nullptr;
    delete m_pHistogram; // the sampling grid changes with rotation, the histogram must be recalculated
    m_pHistogram = nullptr;
    delete[] m_pLUTRGB; // calculated from the histogram
    m_pLUTRGB = nullptr;
    delete[] static_cast<uint8_t*>(m_pBitmapPixels);
    m_pBitmapPixels = nullptr;
    delete[] static_cast<uint8_t*>(m_pBitmapPixelsLUTProcessed);
    m_pBitmapPixelsLUTProcessed = nullptr;
    m_ClippingSize = DSize(0, 0);
}

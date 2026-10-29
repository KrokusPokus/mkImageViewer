#include "BaseImage.h"
#include "BasicProcessing.h"

#include "Helpers.h"
#include "SettingsProvider.h"
#include "HistogramCorr.h"
#include "EXIFReader.h"

#include <cmath>
#include <cassert>

#include <QColor>

///////////////////////////////////////////////////////////////////////////////////
// Static helpers
///////////////////////////////////////////////////////////////////////////////////

static void RotateInplace(const CSize& imageSize, double& dX, double& dY, double dAngle) {
    dX -= (imageSize.cx - 1) * 0.5;
    dY -= (imageSize.cy - 1) * 0.5;
    double dXr = cos(dAngle) * dX - sin(dAngle) * dY;
    double dYr = sin(dAngle) * dX + cos(dAngle) * dY;
    dX = dXr;
    dY = dYr;
}

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
        m_pOrigPixels = NULL;
        m_nOriginalChannels = 0;
    }

    if (pEXIFData != NULL) {
        unsigned char * pEXIF = (unsigned char *)pEXIFData;
        m_nEXIFSize = pEXIF[2]*256 + pEXIF[3] + 2;
        m_pEXIFData = new char[m_nEXIFSize];
        memcpy(m_pEXIFData, pEXIFData, m_nEXIFSize);
        m_pEXIFReader = new CEXIFReader(m_pEXIFData, eImageFormat);
    } else {
        m_nEXIFSize = 0;
        m_pEXIFData = NULL;
        m_pEXIFReader = NULL;
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
    m_pDIBPixels = NULL;
    m_pDIBPixelsLUTProcessed = NULL;
    m_pLastDIB = NULL;
    
    m_pLUTAllChannels = NULL;
    m_pLUTRGB = NULL;
    m_pSaturationLUTs = NULL;
    m_eProcFlags = PFLAG_None;
    m_nInitialRotation = 0;
    m_dInitialZoom = -1;
    m_initialOffsets = CPoint(0, 0);

    m_bCropped = false;
    m_bIsDestructivelyProcessed = false;
    m_bRotationByEXIF = false;
    m_bFirstReprocessing = true;
    m_dibLoadDelayMs = 0;
    m_fileLoadDelayMs = 0;
    m_FullTargetSize = CSize(0, 0);
    m_ClippingSize = CSize(0, 0);
    m_TargetOffset = CPoint(0, 0);

    // Histogram of the original pixels for the automatic correction (computed here, i.e. in the load thread)
    m_pHistogram = new CHistogram(*this);}

CBaseImage::~CBaseImage(void) {
    delete[] static_cast<uint8_t*>(m_pOrigPixels);
    m_pOrigPixels = NULL;
    delete[] static_cast<uint8_t*>(m_pDIBPixels);
    m_pDIBPixels = NULL;
    delete[] static_cast<uint8_t*>(m_pDIBPixelsLUTProcessed);
    m_pDIBPixelsLUTProcessed = NULL;
    delete[] m_pLUTAllChannels;
    m_pLUTAllChannels = NULL;
    delete[] m_pLUTRGB;
    m_pLUTRGB = NULL;
    delete[] m_pSaturationLUTs;
    m_pSaturationLUTs = NULL;
    delete m_pHistogram;
    m_pHistogram = NULL;
    m_pLastDIB = NULL;
    delete[] static_cast<uint8_t*>(m_pEXIFData);
    m_pEXIFData = NULL;
    delete m_pEXIFReader;
    m_pEXIFReader = NULL;
}

void CBaseImage::ResampleWithPan(void* & pDIBPixels, void* & pDIBPixelsLUTProcessed, CSize fullTargetSize,
                                 CSize clippingSize, CPoint targetOffset, CRect oldClippingRect,
                                 EProcessingFlags eProcFlags, const CImageProcessingParams & imageProcParams, 
                                 double dRotation, EResizeType eResizeType) {
    CPoint oldOffset = oldClippingRect.TopLeft();
    CSize oldSize = oldClippingRect.Size();
    CRect newClippingRect = CRect(targetOffset, clippingSize);
    CRect sourceRect;
    if (sourceRect.IntersectRect(oldClippingRect, newClippingRect)) {
        // there is an intersection, reuse the non LUT processed DIB
        sourceRect.OffsetRect(-oldOffset.x, -oldOffset.y);
        CRect targetRect = CRect(CPoint(std::max<long>(0, oldOffset.x - newClippingRect.left), std::max<long>(0, oldOffset.y - newClippingRect.top)),
            CSize(sourceRect.Width(), sourceRect.Height()));

        bool bCanUseLUTProcDIB = ApplyCorrectionLUT(imageProcParams, eProcFlags, pDIBPixelsLUTProcessed, fullTargetSize,
            targetOffset, pDIBPixels, clippingSize, true, false) != NULL;

        // the LUT processed pixels cannot be used and the original pixels are not available -
        // full recreation of DIBs is needed
        if (!bCanUseLUTProcDIB && pDIBPixels == NULL) {
            delete[] static_cast<uint8_t*>(pDIBPixelsLUTProcessed); pDIBPixelsLUTProcessed = NULL;
            return;
        }

        // Copy the reusable part of original DIB pixels
        void* pPannedPixels = (bCanUseLUTProcDIB == false) ? 
            CBasicProcessing::CopyRect32bpp(NULL, pDIBPixels, clippingSize, targetRect, oldSize, sourceRect) :
            NULL;

        // get rid of original DIB, will be recreated automatically when needed
        delete[] static_cast<uint8_t*>(pDIBPixels); pDIBPixels = NULL;

        // Copy the reusable part of processed DIB pixels
        void* pPannedPixelsLUTProcessed = bCanUseLUTProcDIB ? 
            CBasicProcessing::CopyRect32bpp(NULL, pDIBPixelsLUTProcessed, clippingSize, targetRect, oldSize, sourceRect) :
            NULL;

        // Delete old LUT processed DIB, we copied the part that can be reused to a new DIB (pPannedPixelsLUTProcessed)
        delete[] static_cast<uint8_t*>(pDIBPixelsLUTProcessed); pDIBPixelsLUTProcessed = NULL;

        if (targetRect.top > 0) {
            CSize clipSize(clippingSize.cx, targetRect.top);
            void* pTop = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcDIB) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pTop,
                    clippingSize, CRect(CPoint(0, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
            } else {
                void* pTopProc = NULL;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pTopProc, fullTargetSize, targetOffset, pTop, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pTopProc,
                    clippingSize, CRect(CPoint(0, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pTopProc);
            }

            delete[] static_cast<uint8_t*>(pTop);
        }
        if (targetRect.bottom < clippingSize.cy) {
            CSize clipSize(clippingSize.cx, clippingSize.cy -  targetRect.bottom);
            CPoint offset(targetOffset.x, targetOffset.y + targetRect.bottom);
            void* pBottom = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcDIB) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pBottom,
                    clippingSize, CRect(CPoint(0, targetRect.bottom), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
            } else {
                void* pBottomProc = NULL;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pBottomProc, fullTargetSize, offset, pBottom, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pBottomProc,
                    clippingSize, CRect(CPoint(0, targetRect.bottom), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pBottomProc);
            }

            delete[] static_cast<uint8_t*>(pBottom);
        }
        if (targetRect.left > 0) {
            CSize clipSize(targetRect.left, clippingSize.cy);
            void* pLeft = Resample(fullTargetSize, clipSize, targetOffset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcDIB) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pLeft,
                    clippingSize, CRect(CPoint(0, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
            } else {
                void* pLeftProc = NULL;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pLeftProc, fullTargetSize, targetOffset, pLeft, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pLeftProc,
                    clippingSize, CRect(CPoint(0, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pLeftProc);
            }

            delete[] static_cast<uint8_t*>(pLeft);
        }
        if (targetRect.right < clippingSize.cx) {
            CSize clipSize(clippingSize.cx -  targetRect.right, clippingSize.cy);
            CPoint offset(targetOffset.x + targetRect.right, targetOffset.y);
            void* pRight = Resample(fullTargetSize, clipSize, offset, eProcFlags, eResizeType);
            
            if (!bCanUseLUTProcDIB) {
                CBasicProcessing::CopyRect32bpp(pPannedPixels, pRight,
                    clippingSize, CRect(CPoint(targetRect.right, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
            } else {
                void* pRigthProc = NULL;
                ApplyCorrectionLUT(imageProcParams, eProcFlags, pRigthProc, fullTargetSize, offset, pRight, clipSize, false, false);
                CBasicProcessing::CopyRect32bpp(pPannedPixelsLUTProcessed, pRigthProc,
                    clippingSize, CRect(CPoint(targetRect.right, 0), clipSize),
                    clipSize, CRect(CPoint(0, 0), clipSize));
                delete[] static_cast<uint8_t*>(pRigthProc);
            }

            delete[] static_cast<uint8_t*>(pRight);
        }
        pDIBPixels = pPannedPixels;
        pDIBPixelsLUTProcessed = pPannedPixelsLUTProcessed;
        return;
    }

    delete[] static_cast<uint8_t*>(pDIBPixels); pDIBPixels = NULL;
    delete[] static_cast<uint8_t*>(pDIBPixelsLUTProcessed); pDIBPixelsLUTProcessed = NULL;
}

void* CBaseImage::Resample(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset, EProcessingFlags eProcFlags, EResizeType eResizeType) {

    Helpers::CPUType cpu = CSettingsProvider::This().AlgorithmImplementation();
    EFilterType filter = CSettingsProvider::This().DownsamplingFilter();

    if (fullTargetSize.cx > 65535 || fullTargetSize.cy > 65535) return NULL;

    if (GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling) && !(eResizeType == NoResize) && (filter>0)) {
        if (SupportsSIMD(cpu)) {
            if (eResizeType == UpSample) {
                return CBasicProcessing::SampleUp_HQ_SIMD(fullTargetSize, targetOffset, clippingSize, 
                    CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, ToSIMDArchitecture(cpu));
            } else {
                return CBasicProcessing::SampleDown_HQ_SIMD(fullTargetSize, targetOffset, clippingSize,
                    CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter, ToSIMDArchitecture(cpu));
            }
        } else {
            if (eResizeType == UpSample) {
                return CBasicProcessing::SampleUp_HQ(fullTargetSize, targetOffset, clippingSize, 
                    CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
            } else {
                return CBasicProcessing::SampleDown_HQ(fullTargetSize, targetOffset, clippingSize, 
                    CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels, filter);
            }
        }
    } else {
        return CBasicProcessing::PointSample(fullTargetSize, targetOffset, clippingSize,
            CSize(m_nOrigWidth, m_nOrigHeight), m_pOrigPixels, m_nOriginalChannels);
    }
}

CPoint CBaseImage::ConvertOffset(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset) {
    int nStartX = (fullTargetSize.cx - clippingSize.cx)/2 - targetOffset.x;
    int nStartY = (fullTargetSize.cy - clippingSize.cy)/2 - targetOffset.y;
    return CPoint(nStartX, nStartY);
}

bool CBaseImage::VerifyRotation(const CRotationParams& rotationParams) {
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
    if (pNewOriginalPixels == NULL) return false;
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
    if (pNewOriginalPixels == NULL) return false;
    delete[] static_cast<uint8_t*>(m_pOrigPixels);
    m_pOrigPixels = pNewOriginalPixels;
    MarkAsDestructivelyProcessed();
    return true;
}

void* CBaseImage::DIBPixelsLastProcessed(bool bGenerateDIBIfNeeded) {
    if (bGenerateDIBIfNeeded && m_pLastDIB == NULL) {
        m_pLastDIB = GetDIB(m_FullTargetSize, m_ClippingSize, m_TargetOffset, m_imageProcParams, m_eProcFlags);
    }
    return m_pLastDIB;
}

void CBaseImage::VerifyDIBPixelsCreated() {
    if (m_pDIBPixels == NULL) {
        EResizeType eResizeType = GetResizeType(m_FullTargetSize, CSize(m_nOrigWidth, m_nOrigHeight));
        m_pDIBPixels = Resample(m_FullTargetSize, m_ClippingSize, m_TargetOffset, m_eProcFlags, eResizeType);
    }
}

void CBaseImage::RestoreInitialParameters(const CImageProcessingParams& imageProcParams, int nRotation, double dZoom,
                                          CPoint offsets) {
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

void* CBaseImage::GetDIBInternal(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset,
                         const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
                         bool &bParametersChanged) {

    // Check if resampling due to bHighQualityResampling parameter change is needed
    bool bMustResampleQuality = GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling) != GetProcessingFlag(m_eProcFlags, PFLAG_HighQualityResampling);
    bool bTargetSizeChanged = fullTargetSize != m_FullTargetSize;

    // Check if resampling due to change of geometric parameters is needed
    bool bMustResampleGeometry = bTargetSizeChanged || clippingSize != m_ClippingSize || targetOffset != m_TargetOffset;

    qDebug() << "GetDIB:" << "HQchanged" << bMustResampleQuality
             << "targetChanged" << bTargetSizeChanged
             << "clipChanged" << (clippingSize != m_ClippingSize)
             << "offsetChanged" << (targetOffset != m_TargetOffset)
             << "new full/clip/off:" << fullTargetSize.cx << fullTargetSize.cy << "/" << clippingSize.cx << clippingSize.cy
             << "/" << targetOffset.x << targetOffset.y
             << "old:" << m_FullTargetSize.cx << m_FullTargetSize.cy << "/" << m_ClippingSize.cx << m_ClippingSize.cy
             << "/" << m_TargetOffset.x << m_TargetOffset.y;

    EResizeType eResizeType = GetResizeType(fullTargetSize, CSize(m_nOrigWidth, m_nOrigHeight));

    // the geometrical parameters must be set before calling ApplyCorrectionLUT()
    CRect oldClippingRect = CRect(m_TargetOffset, m_ClippingSize);
    m_FullTargetSize = fullTargetSize;
    m_ClippingSize = clippingSize;
    m_TargetOffset = targetOffset;

    uint64_t startTimeMs = Helpers::GetTimeMs();

    // Check if only the LUT must be reapplied but no resampling (resampling is much slower than the LUTs)
    void * pDIB = NULL;
    if (!bMustResampleQuality && !bMustResampleGeometry) {
        pDIB = ApplyCorrectionLUT(imageProcParams, eProcFlags, m_pDIBPixelsLUTProcessed, 
            fullTargetSize, targetOffset, m_pDIBPixels, clippingSize, false, false, bParametersChanged);
    }

    // ApplyCorrectionLUT() could have failed, then recreate the DIBs
    if (pDIB == NULL) {
        // The original is kept with 3 channels: resampling is equally fast for 3 and 4 channels, a conversion
        // would cost time and memory (rotation and mirroring also work on 3 channels)

        bParametersChanged = true;

        // If we only pan, we can resample far more efficiently by only calculating the newly visible areas
        bool bPanningOnly = !m_bFirstReprocessing && !bTargetSizeChanged && !bMustResampleQuality;

        m_bFirstReprocessing = false;
        if (bPanningOnly) {
            ResampleWithPan(m_pDIBPixels, m_pDIBPixelsLUTProcessed, fullTargetSize, clippingSize, targetOffset, 
                oldClippingRect, eProcFlags, imageProcParams, 0.0, eResizeType);
        } else {
            delete[] static_cast<uint8_t*>(m_pDIBPixelsLUTProcessed); m_pDIBPixelsLUTProcessed = NULL;
            delete[] static_cast<uint8_t*>(m_pDIBPixels); m_pDIBPixels = NULL;
        }

        // both DIBs are NULL, do normal resampling
        if (m_pDIBPixels == NULL && m_pDIBPixelsLUTProcessed == NULL) {
            m_pDIBPixels = Resample(fullTargetSize, clippingSize, targetOffset, eProcFlags, eResizeType);
        }

        // if ResampleWithPan() has preserved this DIB, we can reuse it
        if (m_pDIBPixelsLUTProcessed == NULL) {
            pDIB = ApplyCorrectionLUT(imageProcParams, eProcFlags, m_pDIBPixelsLUTProcessed, fullTargetSize,
                targetOffset, m_pDIBPixels, clippingSize, false, false);
        } else {
            pDIB = m_pDIBPixelsLUTProcessed;
        }
    }

    uint64_t dibLoadDelayMs = Helpers::GetTimeMs() - startTimeMs; 
    if (dibLoadDelayMs > 1) {
        m_dibLoadDelayMs = dibLoadDelayMs;
    }

    m_eProcFlags = eProcFlags;

    m_pLastDIB = pDIB;
    
    return pDIB;
}

void* CBaseImage::ApplyCorrectionLUT(const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
                                           void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
                                           void * pSourceDIB, CSize dibSize,
                                           bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB, bool &bParametersChanged) {

    bool bAutoContrast = GetProcessingFlag(eProcFlags, PFLAG_AutoContrast);
    bool bAutoContrastOld = GetProcessingFlag(m_eProcFlags, PFLAG_AutoContrast);

    bool bNoContrastAndGammaLUT = fabs(imageProcParams.Contrast) < 1e-4 && fabs(imageProcParams.Gamma - 1) < 1e-4;
    bool bMustUseSaturationLUTs = fabs(imageProcParams.Saturation - 1.0) > 1e-4;
    bool bNoLUTsApplied = bNoContrastAndGammaLUT && !bAutoContrast && !bMustUseSaturationLUTs;
    bool bContrastOrGammaChanged = fabs(imageProcParams.Contrast - m_imageProcParams.Contrast) > 1e-4 ||
        fabs(imageProcParams.Gamma - m_imageProcParams.Gamma) > 1e-4;
    bool bCorrectionFactorChanged = fabs(imageProcParams.ContrastCorrectionFactor-m_imageProcParams.ContrastCorrectionFactor) > 1e-4;
    bool bSaturationChanged = fabs(imageProcParams.Saturation - m_imageProcParams.Saturation) > 1e-4;
    bool bMustReapplyLUTs = bAutoContrast != bAutoContrastOld
                        || bCorrectionFactorChanged
                        || bContrastOrGammaChanged
                        || bSaturationChanged;
    bool bMustUse3ChannelLUT = bAutoContrast;

    bParametersChanged = bMustReapplyLUTs;

    if (!bMustReapplyLUTs && pCachedTargetDIB != NULL) {
        // consider special case that nothing is dimmed and no LUT is applied but
        // processed pixel is here, we do not want to use it - in this case we just continue.
        if (!bNoLUTsApplied) {
            return pCachedTargetDIB;
        }
    }

    // If it shall only be checked if this method would be able to reuse the existing pCachedTargetDIB, return
    if (bOnlyCheck) {
        return NULL;
    }

    if (pSourceDIB == NULL) {
        return NULL;
    }

    // Recalculate LUTs if needed
    if (bContrastOrGammaChanged || m_pLUTAllChannels == NULL) {
        delete[] m_pLUTAllChannels;
        m_pLUTAllChannels = bNoContrastAndGammaLUT ? NULL : CBasicProcessing::CreateSingleChannelLUT(imageProcParams.Contrast, imageProcParams.Gamma);
    }

    // Calculate the histogram if needed (after rotation)
    if (m_pHistogram == NULL) {
        m_pHistogram = new CHistogram(*this);
    }

    // Recalculate special histogram if needed
    const CHistogram* pHistogram = m_pHistogram;
    bool bSpecialHistogram = false;
    
    if (bMustUse3ChannelLUT && (m_pLUTRGB == NULL || bCorrectionFactorChanged || bAutoContrast != bAutoContrastOld)) {
        delete[] m_pLUTRGB;
        float fBrightnessCorrFactor = bAutoContrast ? 1.0f : 0.0f;
        float fContrastCorrFactor = bAutoContrast ? (float) imageProcParams.ContrastCorrectionFactor : 0.0f;
        m_pLUTRGB = CHistogramCorr::CalculateCorrectionLUT(*pHistogram, fBrightnessCorrFactor, fContrastCorrFactor);
    } else if (!bMustUse3ChannelLUT) {
        delete[] m_pLUTRGB;
        m_pLUTRGB = NULL;
    }
    
    if (bMustUseSaturationLUTs && (m_pSaturationLUTs == NULL || bSaturationChanged)) {
        delete[] m_pSaturationLUTs;
        m_pSaturationLUTs = CBasicProcessing::CreateColorSaturationLUTs(imageProcParams.Saturation);
    } else if (!bMustUseSaturationLUTs) {
        delete[] m_pSaturationLUTs;
        m_pSaturationLUTs = NULL;
    }
    
    delete[] static_cast<uint8_t*>(pCachedTargetDIB);
    pCachedTargetDIB = NULL;
    if (bSpecialHistogram) {
        delete pHistogram;
    }

    if (!bNoLUTsApplied) {
        // LUT --> apply correction
        uint8* pLUT = CHistogramCorr::CombineLUTs(m_pLUTAllChannels, m_pLUTRGB);

        if (bMustUseSaturationLUTs) {
            pCachedTargetDIB = CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(dibSize.cx, dibSize.cy, pSourceDIB, m_pSaturationLUTs, pLUT);
        } else {
            pCachedTargetDIB = CBasicProcessing::Apply3ChannelLUT32bpp(dibSize.cx, dibSize.cy, pSourceDIB, pLUT);
        }

        delete[] pLUT;
    } else if (bCanTakeOwnershipOfSourceDIB) {
        // no LUTs, just take over ownership of source DIB if we are allowed
        pCachedTargetDIB = pSourceDIB;
    } else {
        // no LUTs, no dimming --> return original pixels
        return pSourceDIB;
    }

    return pCachedTargetDIB;
}

CBaseImage::EResizeType CBaseImage::GetResizeType(CSize targetSize, CSize sourceSize) {
    if (targetSize.cx == sourceSize.cx && targetSize.cy == sourceSize.cy) {
        return NoResize;
    } else if (targetSize.cx <= sourceSize.cx && targetSize.cy <= sourceSize.cy) {
        return DownSample;
    } else {
        return UpSample;
    }
}

int CBaseImage::GetRotationFromEXIF(int nOrigRotation) {
    if (m_pEXIFReader != NULL && m_pEXIFReader->ImageOrientationPresent() && CSettingsProvider::This().AutoRotateEXIF()) {

        // Some tools rotate the pixel data but do not reset the EXIF orientation flag.
        // In this case the EXIF thumbnail is normally also not rotated.
        // So check if the thumbnail orientation is the same as the image orientation.
        // If not, it can be assumed that someone touched the pixels and we ignore the EXIF
        // orientation.
/* [GF] Do not rely on aspect ratio comparison to assume Exif roation flag wrong.
        if (m_pEXIFReader->GetThumbnailWidth() > 0 && m_pEXIFReader->GetThumbnailHeight() > 0) {
            bool bWHOrig = m_nInitOrigWidth > m_nInitOrigHeight;
            bool bWHThumb = m_pEXIFReader->GetThumbnailWidth() > m_pEXIFReader->GetThumbnailHeight();
            if (bWHOrig != bWHThumb) {
                return nOrigRotation;
            }
        }
*/
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

void CBaseImage::MarkAsDestructivelyProcessed() {
    m_bIsDestructivelyProcessed = true;
    m_rotationParams.Flags = RFLAG_None;
}

void CBaseImage::InvalidateAllCachedPixelData() {
    m_pLastDIB = NULL;
    delete m_pHistogram; // the sampling grid changes with rotation, the histogram must be recalculated
    m_pHistogram = NULL;
    delete[] static_cast<uint8_t*>(m_pDIBPixels);
    m_pDIBPixels = NULL;
    delete[] static_cast<uint8_t*>(m_pDIBPixelsLUTProcessed);
    m_pDIBPixelsLUTProcessed = NULL;
    m_ClippingSize = CSize(0, 0);
}

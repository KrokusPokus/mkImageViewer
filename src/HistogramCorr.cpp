#include "HistogramCorr.h"
#include "BaseImage.h"
#include "Helpers.h"

#include <math.h>

float CHistogramCorr::sm_ContrastCorrectionStrength = 0.5f;
float CHistogramCorr::sm_BrightnessCorrStrength = 0.2f;


// Calculates sum of pixel values using the histogram
static int64_t CalculateSum(const int* pHistogram) {
    int64_t nSum = 0;
    for (int i = 0; i < 256; i++) {
        nSum += pHistogram[i] * i;
    }
    return nSum;
}

///////////////////////////////////////////////////////////////////////////////////
// CHistogram class
///////////////////////////////////////////////////////////////////////////////////

CHistogram::CHistogram(const CBaseImage & image) {
    // The sampling grid is the one of the former local density correction (LDC), which computed this histogram as
    // a side product. Kept unchanged, so the automatic correction gives the same results as before.
    const int NUM_VALUES = 120000;

    int channelR[256]{ 0 }, channelG[256]{ 0 }, channelB[256]{ 0 };
    int channelGrey[256]{ 0 };

    int nWidth = image.OrigWidth();
    int nHeight = image.OrigHeight();
    int nChannels = image.OriginalChannels();
    const uint8* pSourcePixels = (const uint8*)image.OriginalPixels();

    double dFactor = (double)nWidth/nHeight;
    int nSamplesX = Helpers::DoPadding((int)(dFactor*sqrt(NUM_VALUES/dFactor)), 4);
    int nSamplesY = Helpers::DoPadding((int)(nSamplesX/dFactor), 4);

    uint32 nY = 0;
    uint32 nIncX = (uint32)nWidth*65536/nSamplesX;
    uint32 nIncY = (uint32)nHeight*65536/nSamplesY;
    int nLineSize = Helpers::DoPadding(nWidth * nChannels, 4);

    for (int j = 0; j < nSamplesY; j++) {
        uint32 nX = 0;
        // 64 bit offset: images can have more than 4 GiB of pixel data
        const uint8* pSrcStart = pSourcePixels + (size_t)nLineSize*(nY >> 16);
        for (int i = 0; i < nSamplesX; i++) {
            const uint8* pSrc = (nChannels == 3) ? pSrcStart + (nX >> 16)*3 : pSrcStart + (nX >> 16)*4;
            channelB[pSrc[0]]++;
            channelG[pSrc[1]]++;
            channelR[pSrc[2]]++;
            channelGrey[(pSrc[0]*128 + pSrc[1]*640 + pSrc[2]*256) >> 10]++;
            nX += nIncX;
        }
        nY += nIncY;
    }

    Init(channelB, channelG, channelR, channelGrey);
}

void CHistogram::Init(const int* pChannelB, const int* pChannelG, const int* pChannelR, const int* pChannelGrey) {
    m_nTotalValues = 0;
    for (int i = 0; i < 256; i++) {
        m_nTotalValues += pChannelGrey[i];
    }
    m_nBMean = (int) (CalculateSum(pChannelB) / m_nTotalValues);
    m_nGMean = (int) (CalculateSum(pChannelG) / m_nTotalValues);
    m_nRMean = (int) (CalculateSum(pChannelR) / m_nTotalValues);
    memcpy(m_ChannelB, pChannelB, sizeof(int)*256);
    memcpy(m_ChannelG, pChannelG, sizeof(int)*256);
    memcpy(m_ChannelR, pChannelR, sizeof(int)*256);
    memcpy(m_ChannelGrey, pChannelGrey, sizeof(int)*256);
}

// Gets relative area of a part of the histogram, 1.0 is full histogram area
static float GetHistogramArea(const int* pHistogram, int nTotalValues, float fStart, float fEnd) {
    int nStart = (int)(fStart*255 + 0.5f);
    int nEnd = (int)(fEnd*255 + 0.5f);
    int nSum = 0;
    for (int i = nStart; i <= nEnd; i++) {
        nSum += pHistogram[i];
    }
    return (float)nSum/nTotalValues;
}

///////////////////////////////////////////////////////////////////////////////////
// CHistogramCorr class - helpers
///////////////////////////////////////////////////////////////////////////////////

// Calculates the black and white point from the histogram. The given level (> 0) is the number of
// pixels that can be below resp. above black/white point.
static void CalculateBWPoints(float& fBlackPt, float& fWhitePt, const int* pValues, int nLevel) {
    int nSum = 0;
    int i = 0;
    while (nSum < nLevel) {
        nSum += pValues[i];
        i++;
    }
    fBlackPt = (i == 0) ? 0.0f : (i - 1)/255.0f;

    i = 255;
    nSum = 0;
    while (nSum < nLevel) {
        nSum += pValues[i];
        i--;
    }
    fWhitePt = (i == 255) ? 1.0f : (i + 1)/255.0f;
}

// Calculates the 256 entry LUT (in pTarget) from black/white points and the correction values
// The LUT is a linear curve between black and white point, added by a quadratic correction term
static void CalculateLUT(uint8* pTarget, float fBlackPt, float fWhitePt, float fBrightnessCorr, float fMidPt) {
    if (fWhitePt - fBlackPt < 0.001f) {
        fWhitePt = 1.0f;
        fBlackPt = 0.0f;
    }
    float fA = 1.0f/(fWhitePt - fBlackPt);
    float fB = -fBlackPt*fA;

    float fInc = 1.0f/255.0f;
    float fX = 0.0f;
    for (int i = 0; i < 256; i++) {
        float fY = fA*fX + fB;
        if (fY < 0.0f) {
            pTarget[i] = 0;
        } else if (fY > 1.0f) {
            pTarget[i] = 255;
        } else {
            float fTarget;
            if (fX < fMidPt) {
                float fVal = (fX - fMidPt)/(fMidPt - fBlackPt);
                fTarget = fY + fBrightnessCorr*(1.0f - fVal*fVal);
            } else {
                float fVal = (fX - fMidPt)/(fWhitePt - fMidPt);
                fTarget = fY + fBrightnessCorr*(1.0f - fVal*fVal);
            }

            fTarget = std::max(0.0f, std::min(1.0f, fTarget));
            pTarget[i] = (uint8)(fTarget*255 + 0.5f);
        }
        fX += fInc;
    }
}

// Calculates the color compensation needed due to black/white point correction
static float CalcBWColorCompensation(float fMidPt, float fBlackPt, float fWhitePt) {
    float fA = 1.0f/std::max(0.001f, fWhitePt - fBlackPt);
    float fB = -fBlackPt*fA;
    return fA*fMidPt + fB;
}

///////////////////////////////////////////////////////////////////////////////////
// CHistogramCorr class
///////////////////////////////////////////////////////////////////////////////////

uint8* CHistogramCorr::CalculateCorrectionLUT(const CHistogram & histogram,
                                              float fBrightnessCorrectionFactor,
                                              float fContrastCorrectionFactor) {

    const float cCuttingLevel = 0.001f; // black/white point cutting level
    const float cBrightnessCorr = 1.0f; // strength of brightness correction
    const float cLog0dot5 = log10f(0.5f);

    float fContrastStrengthLog = (sm_ContrastCorrectionStrength == 0) ? 100 : log10f(sm_ContrastCorrectionStrength)/cLog0dot5;
    float fContrastFactorLog = (fContrastCorrectionFactor == 0) ? 100 : log10f(fContrastCorrectionFactor)/cLog0dot5;
    float fStrength = std::max(1e-4f, fContrastStrengthLog*fContrastFactorLog);

    int nLevel = (int) (histogram.GetTotalValues()*cCuttingLevel + 0.5f);

    uint8* pLUT = new uint8[3*256];

    // Calculate black point for R,G,B and Grey channel
    float fBlackPtB, fBlackPtG, fBlackPtR, fBlackPtGrey;
    float fWhitePtB, fWhitePtG, fWhitePtR, fWhitePtGrey;
    CalculateBWPoints(fBlackPtB, fWhitePtB, histogram.GetChannelB(), nLevel);
    CalculateBWPoints(fBlackPtG, fWhitePtG, histogram.GetChannelG(), nLevel);
    CalculateBWPoints(fBlackPtR, fWhitePtR, histogram.GetChannelR(), nLevel);
    CalculateBWPoints(fBlackPtGrey, fWhitePtGrey, histogram.GetChannelGrey(), nLevel);

    // Reduce the maximal applied correction - if the black or white point is far from 0,
    // respectively 1, we only apply a decreasing fraction of the full correction.
    float fHistogramWidthFactor = powf((fWhitePtGrey - fBlackPtGrey), fStrength*0.5f);
    fBlackPtB = fBlackPtB*powf((1 - fBlackPtB)*(1 - fBlackPtGrey), fStrength)*fHistogramWidthFactor;
    fBlackPtG = fBlackPtG*powf((1 - fBlackPtG)*(1 - fBlackPtGrey), fStrength)*fHistogramWidthFactor;
    fBlackPtR = fBlackPtR*powf((1 - fBlackPtR)*(1 - fBlackPtGrey), fStrength)*fHistogramWidthFactor;
    fWhitePtB = 1 - (1 - fWhitePtB)*powf(fWhitePtB*fWhitePtGrey, fStrength)*fHistogramWidthFactor;
    fWhitePtG = 1 - (1 - fWhitePtG)*powf(fWhitePtG*fWhitePtGrey, fStrength)*fHistogramWidthFactor;
    fWhitePtR = 1 - (1 - fWhitePtR)*powf(fWhitePtR*fWhitePtGrey, fStrength)*fHistogramWidthFactor;

    // If the brightness decreased by the correction, undo this
    float fMeanBlackPt = (fBlackPtB + fBlackPtG + fBlackPtR)/3;
    float fMeanWhitePt = (fWhitePtB + fWhitePtG + fWhitePtR)/3; 
    float fA = 1.0f/(fMeanWhitePt - fMeanBlackPt);
    float fB = -fMeanBlackPt*fA;
    float fMidPt = (fMeanWhitePt + fMeanBlackPt)*0.5f;
    float fCorr = cBrightnessCorr*(fMidPt*(1.0f - fA) - fB);
    fCorr = std::max(0.0f, fCorr);

    float fYMean = fA*fMidPt + fB;
    float fCorrBlue  = fYMean - CalcBWColorCompensation(fMidPt, fBlackPtB, fWhitePtB);
    float fCorrGreen = fYMean - CalcBWColorCompensation(fMidPt, fBlackPtG, fWhitePtG);
    float fCorrRed   = fYMean - CalcBWColorCompensation(fMidPt, fBlackPtR, fWhitePtR);

    float fMeanGrey = (histogram.GetBMean()*0.1f + histogram.GetGMean()*0.6f + histogram.GetRMean()*0.3f)/255;
    float fBrightnessAdd = (fMeanGrey < 0.5f) ? (0.5f - fMeanGrey)*sm_BrightnessCorrStrength*fBrightnessCorrectionFactor : 0.0f;
    fCorr = (fCorr - fCorrBlue)*0.1f + (fCorr - fCorrGreen)*0.6f + (fCorr - fCorrRed)*0.3f + fBrightnessAdd;

    CalculateLUT(&(pLUT[0]), fBlackPtB, fWhitePtB, fCorrBlue + fCorr, fMidPt);
    CalculateLUT(&(pLUT[256]), fBlackPtG, fWhitePtG, fCorrGreen + fCorr, fMidPt);
    CalculateLUT(&(pLUT[512]), fBlackPtR, fWhitePtR, fCorrRed + fCorr, fMidPt);

    /*
    // The blue channel needs some special attention as we do not want to make an image
    // more blue than it was
    float fYMean = fA*fMidPt + fB;
    float fABlue = 1.0f/(fWhitePtB - fBlackPtB);
    float fBBlue = -fBlackPtB*fABlue;
    float fMidPtBlue = (fWhitePtB - fBlackPtB)*0.5f;
    float fYBlue = fABlue*fMidPtBlue + fBBlue;
    float fCorrBlue;
    if (fYBlue > fYMean) {
        fCorrBlue = fYMean - fYBlue;
    } else {
        fCorrBlue = fCorr;
    }

    CalculateLUT(&(pLUT[0]), fBlackPtB, fWhitePtB, fCorrBlue);
    CalculateLUT(&(pLUT[256]), fBlackPtG, fWhitePtG, fCorr);
    CalculateLUT(&(pLUT[512]), fBlackPtR, fWhitePtR, fCorr);
    */
    return pLUT;
}

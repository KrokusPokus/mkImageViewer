#ifndef HISTOGRAMCORR_H
#define HISTOGRAMCORR_H

#include "ImageProcessingTypes.h"
#include "WTLCompat.h"

class CBaseImage;

// Histogram (R,G,B,Grey) of an image
class CHistogram {
public:
    // Histogram of the original pixels of the image, from a grid of about 120000 point samples
    explicit CHistogram(const CBaseImage & image);

    // Gets the number of pixels used to build the histogram
    int GetTotalValues() const { return m_nTotalValues; }

    // Access to the histogram channels (returned LUTs have 256 values)
    const int* GetChannelR() const { return &(m_ChannelR[0]); }
    const int* GetChannelG() const { return &(m_ChannelG[0]); }
    const int* GetChannelB() const { return &(m_ChannelB[0]); }
    const int* GetChannelGrey() const { return &(m_ChannelGrey[0]); }

    // Mean values of the channels
    int GetBMean() const { return m_nBMean; }
    int GetGMean() const { return m_nGMean; }
    int GetRMean() const { return m_nRMean; }

private:
    int m_nTotalValues;
    int m_ChannelR[256];
    int m_ChannelG[256];
    int m_ChannelB[256];
    int m_ChannelGrey[256];
    int m_nRMean, m_nGMean, m_nBMean;

    // Takes over the channel histograms (256 entries each) and calculates the totals and mean values
    void Init(const int* pChannelB, const int* pChannelG, const int* pChannelR, const int* pChannelGrey);
};

// Automatic contrast correction by histogram analysis
class CHistogramCorr
{
public:
    // strength of histogram correction (black-white points), in 0 .. 1
    static void SetContrastCorrectionStrength(float fStrength) { sm_ContrastCorrectionStrength = fStrength; }

    // strength of brightness correction in 0 .. 1
    static void SetBrightnessCorrectionStrength(float fStrength) { sm_BrightnessCorrStrength = fStrength; }

    // Calculate a three channel correction LUT for contrast and brightness correction given a histogram
    // (black and white point per channel, no color cast correction)
    // The returned LUT contains 3 * 256 entries, BBBB...GGGG...RRRR... format
    static uint8* CalculateCorrectionLUT(const CHistogram & histogram,
        float fBrightnessCorrectionFactor, float fContrastCorrectionFactor);

    // Combine the given two LUTs into a three channel LUT. Result(value) = 3CLUT(1CLUT(value))
    // If both LUTs are NULL, an identity LUT is returned.
    // The returned LUT contains 3 * 256 entries, BBBB...GGGG...RRRR... format
    static uint8* CombineLUTs(const uint8* pSingleChannelLUT, const uint8* pThreeChannelLUT);

private:
    CHistogramCorr(void);

    static float sm_ContrastCorrectionStrength;
    static float sm_BrightnessCorrStrength;
};


#endif // HISTOGRAMCORR_H

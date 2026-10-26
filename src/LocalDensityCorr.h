#ifndef LOCALDENSITYCORR_H
#define LOCALDENSITYCORR_H

#include "ImageProcessingTypes.h"

class CJPEGImage;
class CHistogram;

// Local density correction for an image, lighting shadows and darkening highlights
class CLocalDensityCorr
{
public:
    // Note: Partially constructed LDC object allows getting the histogram and the pixel hash but not much more
    CLocalDensityCorr(const CJPEGImage & image, bool bFullConstruct);
    ~CLocalDensityCorr(void);

    // Gets the histogram.
    // The histogram is a side product of the LDC and can be retrieved if needed
    const CHistogram* GetHistogram() { return m_pHistogramm; }

    // Returns the pixel hash value. Note that for compressed images, using the pixel hash is
    // not a good identifier because the value depends on the implementation of the decompression.
    // For JPEG files use the method Helpers::CalculateJPEGFileHash() instead.
    int64_t GetPixelHash() const { return m_nPixelHash; }

private:
    CHistogram* m_pHistogramm;
    int m_nLDCWidth;
    int m_nLDCHeight;
    uint8* m_pLDCMap;
    uint8* m_pLDCMapMultiplied;
    float m_fBlackPt, m_fWhitePt;
    float m_fMiddleGrey;
    int64_t m_nPixelHash;

    uint16* m_pPointSampledImage;
    int m_nPSIWidth;
    int m_nPSIHeight;

    void SmoothLDCMask();
    void CreateLDCMap();
};


#endif // LOCALDENSITYCORR_H

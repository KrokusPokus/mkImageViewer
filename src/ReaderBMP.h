#ifndef READERBMP_H
#define READERBMP_H

#include "ImageProcessingTypes.h"

class CJPEGImage;

// Simple reader for windows bitmap files (.bmp)
class CReaderBMP
{
public:
    // Returns NULL in case of errors
    static CJPEGImage* ReadBmpImage(LPCTSTR strFileName, bool& bOutOfMemory);
    static CJPEGImage* ReadBmpImage(void * pBuffer, int bufsize, bool& bOutOfMemory, EImageFormat eContainerFormat = IF_Unknown, bool bIsAnimation = false, int nFrameIndex = 0, int nNumberOfFrames = 1, int nFrameTimeMs = 0);
private:
    CReaderBMP(void);
};


#endif // READERBMP_H

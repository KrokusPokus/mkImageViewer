#ifndef READERTGA_H
#define READERTGA_H

#include "Helpers.h"

class CJPEGImage;

// Simple reader for .tga files
class CReaderTGA
{
public:
	// Returns NULL in case of errors. backgroundColor is used for blending transparent parts of the image.
	static CJPEGImage* ReadTgaImage(LPCTSTR strFileName, Helpers::ETransparencyMode nTransparencyMode, bool& bOutOfMemory);
private:
	CReaderTGA(void);
};


#endif // READERTGA_H

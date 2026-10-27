#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "ProcessParams.h"

#include <QString>

class CBaseImage;

// Allows to copy an image or text to the clipboard and to paste an image from the clipboard
class CClipboard {
public:
    // Copy the pixels of the currently visible part of the image as a 24 bpp DIB to the clipboard
    // Zoom is as on screen.
    static void CopyImageToClipboard(CBaseImage * pImage, const QString& fileName);

    // Copy processed full size image as a 24 bpp DIB to the clipboard, i.e. in original resolution.
    static void CopyFullImageToClipboard(CBaseImage * pImage, const CImageProcessingParams& procParams,
        EProcessingFlags eFlags, const QString& fileName);

    // Copy section of full size image as a 24 bpp DIB to the clipboard, i.e. in original resolution.
    static void CopyFullImageToClipboard(CBaseImage * pImage, const CImageProcessingParams& procParams,
        EProcessingFlags eFlags, CRect clipRect, const QString& fileName);

    // Copy file path to the clipboard.
    static void CopyPathToClipboard(CBaseImage* pImage, const QString& fileName);
    
    static void CutFileToClipboard(const QString& fileName);

private:
    CClipboard(void);

    static void DoCopy(int nWidth, int nHeight, const void* pSourceImageDIB32, const QString& fileName);
    static void DoCopyFileNameText(const QString& fileName);
};

#endif // CLIPBOARD_H

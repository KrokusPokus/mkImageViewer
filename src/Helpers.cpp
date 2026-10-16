#include "Helpers.h"
#include "immintrin.h"
#include "MultiMonitorSupport.h"
#include "JPEGImage.h"
#include "FileList.h"
#include "SettingsProvider.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <QString>
#include <string_view>


namespace Helpers {

float DPIScalingFactor = -1.0f;

TCHAR CReplacePipe::sm_buffer[MAX_SIZE_REPLACE_PIPE];

CReplacePipe::CReplacePipe(LPCTSTR sText) {
    _tcsncpy_s(sm_buffer, MAX_SIZE_REPLACE_PIPE, sText, MAX_SIZE_REPLACE_PIPE);
    sm_buffer[MAX_SIZE_REPLACE_PIPE-1] = 0;
    TCHAR* pPtr = sm_buffer;
    while (*pPtr != 0) {
        if (*pPtr == _T('|')) *pPtr = 0;
        pPtr++;
    }
}

static TCHAR buffAppPath[MAX_PATH + 32] = _T("");

LPCTSTR JPEGViewAppDataPath() {
    if (buffAppPath[0] == 0) {
        ::SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffAppPath);
        _tcscat_s(buffAppPath, MAX_PATH + 32, _T("\\JPEGView\\"));
    }
    return buffAppPath;
}

void SetJPEGViewAppDataPath(LPCTSTR sPath) {
    _tcscpy_s(buffAppPath, MAX_PATH + 32, sPath);
}

CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, 
                    bool bAllowZoomIn, bool bFillCrop, bool bLimitAR, EAutoZoomMode eAutoZoomMode, double & dZoom) {
    CSettingsProvider& sp = CSettingsProvider::This();
    int nBookModePageHeight = sp.BookModePageHeight();

    if (eAutoZoomMode == ZM_BookMode) {
        double dImageAR = (double)nWidth/nHeight;
        if (dImageAR >= 1.0) {                        // If it's wider than high, assume a double page
            dZoom = (double)nScreenWidth/nWidth;    //    ... and Fit to screen width
        } else {                                    // else, assume a single page
            double dAR1 = ((double)(nHeight*100) / (nBookModePageHeight * nScreenHeight));    // Zoom to user specified height
            double dAR2 = (double)nWidth/nScreenWidth;                                        //    ...but limit maximum image width to window width
            double dAR = std::max(dAR1, dAR2);
            dZoom = 1.0/dAR;
        }

    } else if (eAutoZoomMode == ZM_None) {
        dZoom = 1.0;
    } else {
        double dAR1 = (double)nWidth/nScreenWidth;
        double dAR2 = (double)nHeight/nScreenHeight;
        double dARMin = std::min(dAR1, dAR2);
        double dARMax = std::max(dAR1, dAR2);
        double dAR = bFillCrop ? dARMin : dARMax;

        if (dAR <= 1.0 && !bAllowZoomIn) {
            if (bFillCrop) dAR = dARMax;
            if (dAR <= 1.0) {
                dZoom = 1.0;
                return CSize(nWidth, nHeight);
            }
        }
        if (bFillCrop && bLimitAR) {
            if (((nWidth/dAR * nHeight/dAR) / (nScreenWidth * nScreenHeight)) > 1.5) {
                dAR = dARMax; // use fit to screen
            }
        }
        dZoom = 1.0/dAR;
        dZoom = std::min(ZoomMax, dZoom);
    }

    CSize size((int)(nWidth * dZoom + 0.5), (int)(nHeight * dZoom + 0.5));

/*
    CSize size((int)(nWidth/dAR + 0.5), (int)(nHeight/dAR + 0.5));
    if (abs(size.cx - nScreenWidth) <= 1 && abs(size.cy - nScreenHeight) <= 1) {
        // allow 1 pixel tolerance to screen size - prefer screen size in this case to prevent another resize operation to fit image
        dZoom = (nScreenWidth > nScreenHeight) ? (double)nScreenWidth/nWidth : (double)nScreenHeight/nHeight;
        dZoom = min(ZoomMax, dZoom);
        return CSize(nScreenWidth, nScreenHeight);
    }
*/
    return size;
}

CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, EAutoZoomMode eAutoZoomMode, double & dZoom) {
    // Do not comment this out! newSize is unused, but GetImageRect() is needed to calculate dZoom!
    CSize newSize = GetImageRect(nWidth, nHeight, nScreenWidth, nScreenHeight,
        eAutoZoomMode == ZM_FitToScreen || eAutoZoomMode == ZM_FillScreen,
        eAutoZoomMode == ZM_FillScreenNoZoom || eAutoZoomMode == ZM_FillScreen,
        eAutoZoomMode == ZM_FillScreenNoZoom, eAutoZoomMode, dZoom);
    return CSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
}

CSize GetVirtualImageSize(CSize originalImageSize, CSize screenSize, EAutoZoomMode eAutoZoomMode, double & dZoom) {
    CSize newSize;
    if (dZoom < 0.0) {
        // zoom not set, interpret as 'fit to screen'
        // ---------------------------------------------
        newSize = Helpers::GetImageRect(originalImageSize.cx, originalImageSize.cy, screenSize.cx, screenSize.cy, eAutoZoomMode, dZoom);
    } else {
        // zoom set, use this value for the new size
        // ---------------------------------------------
        newSize = CSize((int)(originalImageSize.cx * dZoom + 0.5), (int)(originalImageSize.cy * dZoom + 0.5));
    }
//    newSize.cx = max(1, min(65535, newSize.cx));
//    newSize.cy = max(1, min(65535, newSize.cy));
// Do aspect ratio corrected limiting instead
    if (newSize.cx > 65535 || newSize.cy > 65535) {
        double dFac = 65535.0/std::max(newSize.cx,newSize.cy);
        dZoom = dZoom*dFac;
        newSize.cx = (int)(originalImageSize.cx * dZoom + 0.5);
        newSize.cy = (int)(originalImageSize.cy * dZoom + 0.5);
    }
    return newSize;
}

CPoint LimitOffsets(const CPoint& offsets, const CSize & rectSize, const CSize & outerRect) {
    int nMaxOffsetX = (outerRect.cx - rectSize.cx)/2;
    nMaxOffsetX = std::max(0, nMaxOffsetX);
    int nMaxOffsetY = (outerRect.cy - rectSize.cy)/2;
    nMaxOffsetY = std::max(0, nMaxOffsetY);
    return CPoint(std::max<long>(-nMaxOffsetX, std::min<int>(+nMaxOffsetX, offsets.x)), std::max<long>(-nMaxOffsetY, std::min<int>(+nMaxOffsetY, offsets.y)));
}

#ifdef _WIN64
static CPUType ProbeSSEorAVX2() {
    __try {
        // check if CPU supports AVX and the xgetbv instruction
        int abcd[4];
        __cpuid(abcd, 1);
        if ((abcd[2] & 0x18000000) != 0x18000000) // AVX and OSXSAVE bits
            return CPU_SSE;
        if ((abcd[2] & 0x04000000) == 0) // XSAVE bit, support for xgetbv
            return CPU_SSE;

        // check if operating system supports AVX(2)
        unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 6) != 6)
            return CPU_SSE; // nope, only use SSE

        // check if AVX2 instructions are supported
        const int AVX2BITMASK = 1 << 5;
        __cpuidex(abcd, 7, 0);
        return (abcd[1] & AVX2BITMASK) ? CPU_AVX2 : CPU_SSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return CPU_SSE;
    }
}
#endif

CPUType ProbeCPU(void) {
    static CPUType cpuType = CPU_Unknown;
    if (cpuType != CPU_Unknown) {
        return cpuType;
    }

#ifdef _WIN64
    return ProbeSSEorAVX2(); // 64 bit always supports at least SSE
#else
    // Structured exception handling is mandatory, try/catch(...) does not catch such severe stuff.
    cpuType = CPU_Generic;
    __try {
        uint32 FeatureMask;
        _asm {
            mov eax, 1
            cpuid
            mov FeatureMask, edx
        }
        if ((FeatureMask & (1 << 26)) != 0) {
            cpuType = CPU_SSE; // this means SSE2
        } else if ((FeatureMask & (1 << 25)) != 0) {
            cpuType = CPU_MMX; // yes, we need SSE as the pmax/pmin stuff was coming with the PIII and SSE
        } else {
            // last chance - check if AMD and if yes, test for AMD MMX extensions that also implement pmax/pmin
            _asm {
                mov eax, 0
                cpuid
                cmp ebx, 0x68747541 // is AMD processor?
                jne GiveUp
                mov eax, 0x80000001
                cpuid
                mov FeatureMask, edx
GiveUp:
                mov FeatureMask, 0
            }
            if ((FeatureMask & (1 << 22)) != 0) {
                cpuType = CPU_MMX; // extended AMD MMX instructions
            }
        }
        
    } __except ( EXCEPTION_EXECUTE_HANDLER ) {
        // even CPUID is not supported, use generic code
        return cpuType;
    }
    return cpuType;
#endif
}

bool PatternMatch(LPCTSTR & sMatchingPattern, LPCTSTR sString, LPCTSTR sPattern) {
    sMatchingPattern = NULL;
    if (sString == NULL || sPattern == NULL || *sPattern == 0) return false;
    LPCTSTR pInStr = sString;
    LPCTSTR pInPat = sPattern;
    while (*pInPat != 0) {
        bool bThisPatternFails = false;
        while (*pInPat == _T(';')) pInPat++; // skip sequences of separators
        sMatchingPattern = pInPat;
        while (*pInPat != 0 && *pInPat != _T(';')) {
            bool bAsterix = false;
            if (*pInPat == _T('*')) {
                pInPat++;
                bAsterix = true;
            }
            LPCTSTR pPatStart = pInPat;
            while (*pPatStart != 0 && *pPatStart != _T('*') && *pPatStart != _T(';')) pPatStart++;
            int nPatLen = (int)(pPatStart - pInPat);
            if (nPatLen > 0) {
                if (bAsterix) {
                    while (*pInStr != 0 && _tcsnicmp(pInPat, pInStr, nPatLen) != 0) pInStr++;
                    if (*pInStr == 0) {
                        bThisPatternFails = true;
                        while (*pInPat != 0 && *pInPat != _T(';')) pInPat++;
                    } else {
                        pInStr += nPatLen;
                        pInPat += nPatLen;
                    }
                } else {
                    if (_tcsnicmp(pInPat, pInStr, nPatLen) != 0) {
                        bThisPatternFails = true;
                        while (*pInPat != 0 && *pInPat != _T(';')) pInPat++;
                    } else {
                        pInStr += nPatLen;
                        pInPat += nPatLen;
                    }
                }
            }
        }
        if (!bThisPatternFails) return true;
        pInStr = sString;
        if (*pInPat != 0) pInPat++;
    }
    return false;
}

int FindMoreSpecificPattern(LPCTSTR sPattern1, LPCTSTR sPattern2) {
    if (sPattern1 == NULL) {
        return (sPattern2 == NULL) ? 0 : -1;
    } else if (sPattern2 == NULL) {
        return 1;
    }
    while (*sPattern1 != 0 && *sPattern1 != _T(';') && *sPattern2 != 0 && *sPattern2 != _T(';') &&
        _totlower(*sPattern1) == _totlower(*sPattern2)) {
        sPattern1++;
        sPattern2++;
    }
    if (*sPattern1 == 0 || *sPattern1 == _T(';')) {
        if (*sPattern2 == 0 || *sPattern2 == _T(';')) {
            return 0; // no one is more specific
        } else {
            return -1; // sPattern2 is more specific
        }
    } else if (*sPattern2 == 0 || *sPattern2 == _T(';')) {
        return 1; // sPattern1 is more specific
    } else {
        if (*sPattern1 == _T('*')) {
            return -1; // pattern 1 accepts any char, thus sPattern2 is more specific
        } else if (*sPattern2 == _T('*')) {
            return 1; // sPattern1 is more specific
        }
        return 0;
    }
}

// calculate CRT table
void CalcCRCTable(unsigned int crc_table[256]) {
     for (int n = 0; n < 256; n++) {
       unsigned int c = (unsigned int) n;
       for (int k = 0; k < 8; k++) {
         if (c & 1)
           c = 0xedb88320L ^ (c >> 1);
         else
           c = c >> 1;
       }
       crc_table[n] = c;
     }
}

void* FindJPEGMarker(void* pJPEGStream, int nStreamLength, unsigned char nMarker) {
    uint8* pStream = (uint8*) pJPEGStream;
    if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
        return NULL; // not a JPEG
    }
    int nIndex = 2;
    do {
        if (pStream[nIndex] == 0xFF) {
            // block header found, skip padding bytes
            while (pStream[nIndex] == 0xFF && nIndex < nStreamLength) nIndex++;
            if (pStream[nIndex] == 0 || pStream[nIndex] == nMarker) {
                break; // 0xFF 0x00 is part of pixel block, break
            } else {
                // it's a block marker, read length of block and skip the block
                nIndex++;
                if (nIndex+1 < nStreamLength) {
                    nIndex += pStream[nIndex]*256 + pStream[nIndex+1];
                } else {
                    nIndex = nStreamLength;
                }
            }
        } else {
            break; // block with pixel data found, start hashing from here
        }
    } while (nIndex < nStreamLength);

    if (nMarker == 0 || (pStream[nIndex] == nMarker && pStream[nIndex-1] == 0xFF)) {
        return &(pStream[nIndex-1]); // place on marker start
    } else {
        return NULL;
    }
}

void* FindEXIFBlock(void* pJPEGStream, int nStreamLength) {
    uint8* pEXIFBlock = (uint8*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xE1);
    if (pEXIFBlock != NULL && strncmp((const char*)(pEXIFBlock + 4), "Exif", 4) != 0) {
        return NULL;
    }
    return pEXIFBlock;
}

__int64 CalculateJPEGFileHash(void* pJPEGStream, int nStreamLength) {
    uint8* pStream = (uint8*) pJPEGStream;
    void* pPixelStart = FindJPEGMarker(pJPEGStream, nStreamLength, 0);
    if (pPixelStart == NULL) {
        return 0;
    }
    int nIndex = (int)((uint8*)pPixelStart - (uint8*)pJPEGStream + 1);
    
    // take whole stream in case of inconsistency or if remaining part is too small
    if (nStreamLength - nIndex < 4) {
        nIndex = 0;
        assert(false);
    }

    // now we can calculate the hash over the compressed pixel data
    // do not look at every byte due to performance reasons
    const int nTotalLookups = 10000;
    int nIncrement = (nStreamLength - nIndex)/nTotalLookups;
    nIncrement = std::max(1, nIncrement);

    unsigned int crc_table[256];
    CalcCRCTable(crc_table);
    uint32 crcValue = 0xffffffff;
    unsigned int sumValue = 0;
    while (nIndex < nStreamLength) {
        sumValue += pStream[nIndex];
        crcValue = crc_table[(crcValue ^ pStream[nIndex]) & 0xff] ^ (crcValue >> 8);
        nIndex += nIncrement;
    }

    return ((__int64)crcValue << 32) + sumValue;
}

CString TryConvertFromUTF8(uint8* pComment, int nLengthInBytes) {
    wchar_t* pCommentUnicode = new wchar_t[nLengthInBytes + 1];
    char* pCommentBack = new char[nLengthInBytes + 1];
    CString result;
    int nCharsConverted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)pComment, nLengthInBytes, pCommentUnicode, nLengthInBytes);
    if (nCharsConverted > 0) {
        pCommentUnicode[nCharsConverted] = 0;
        if (::WideCharToMultiByte(CP_UTF8, 0, pCommentUnicode, -1, pCommentBack, nLengthInBytes + 1, NULL , NULL) > 0) {
            if (memcmp(pComment, pCommentBack, nLengthInBytes) == 0) {
                result = CString((LPCWSTR)pCommentUnicode);
            }
        }
    }
    delete[] pCommentUnicode;
    delete[] pCommentBack;
    return result;
}

CString GetJPEGComment(void* pJPEGStream, int nStreamLength) {
    uint8* pCommentSeg = (uint8*)FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE);
    if (pCommentSeg == NULL) {
        return CString("");
    }
    pCommentSeg += 2;
    int nCommentLen = pCommentSeg[0]*256 + pCommentSeg[1] - 2;
    if (nCommentLen <= 0) {
        return CString("");
    }

    uint8* pComment = &(pCommentSeg[2]);
    if (nCommentLen > 2 && (nCommentLen & 1) == 0) {
        if (pComment[0] == 0xFF && pComment[1] == 0xFE) {
            // UTF16 little endian
            return CString((LPCWSTR)&(pComment[2]), (nCommentLen - 2) / 2);
        } else if (pComment[0] == 0xFE && pComment[1] == 0xFF) {
            // UTF16 big endian -> cannot read
            return CString("");
        }
    }

    CString sConvertedFromUTF8 = TryConvertFromUTF8(pComment, nCommentLen);
    if (!sConvertedFromUTF8.IsEmpty()) {
        return (sConvertedFromUTF8.Find(_T("Intel(R) JPEG Library")) != -1) ? CString("") : sConvertedFromUTF8;
    }

    // check if this is a reasonable string - it must contain enough characters between a and z and A and Z
    if (nCommentLen < 7) {
        return CString(""); // cannot check for such short strings, do not use
    }
    int nGoodChars = 0;
    for (int i = 0; i < nCommentLen; i++) {
        uint8 ch = pComment[i];
        if (ch >= 'a' && ch <= 'z' ||  ch >= 'A' && ch <= 'Z' || ch == ' ' || ch == ',' || ch == '.' || ch >= '0' && ch <= '9') {
            nGoodChars++;
        }
    }
    // The Intel lib puts this useless comment into each JPEG it writes - filter this out as nobody is interested in that...
    if (nCommentLen > 20 && strstr((char*)pComment, "Intel(R) JPEG Library") != NULL) {
        return CString("");
    }

    return (nGoodChars > nCommentLen * 8 / 10) ? CString((LPCSTR)pComment, nCommentLen) : CString("");
}

double GetExactTickCount() {
    static __int64 nFrequency = -1;
    if (nFrequency == -1) {
        if (!::QueryPerformanceFrequency((LARGE_INTEGER*)&nFrequency)) {
            nFrequency = 0;
        }
    }
    if (nFrequency == 0) {
        return ::GetTickCount();
    } else {
        __int64 nCounter;
        ::QueryPerformanceCounter((LARGE_INTEGER*)&nCounter);
        return (1000*(double)nCounter)/nFrequency;
    }
}

int GetWindowCaptionSize() {
    return ::GetSystemMetrics(SM_CYCAPTION);
}

CSize GetTotalBorderSize() {
    const int SM_CXP_ADDEDBORDER = 92;  // in MSDN this is SM_CXPADDEDBORDER, but for some reason it's not always available depending on configuration
    int nBorderWidth = (::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2;
    int nBorderHeight = (::GetSystemMetrics(SM_CYSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2 + ::GetSystemMetrics(SM_CYCAPTION);
    return CSize(nBorderWidth, nBorderHeight);
}

CRect GetWindowRectMatchingImageSize(HWND hWnd, CSize minSize, CSize maxSize, double& dZoom, CJPEGImage* pImage, bool bForceCenterWindow, bool bKeepAspectRatio, bool bWindowBorderless, EAutoZoomMode eAutoZoomMode) {
    int nOrigWidth = (pImage == NULL) ? ::GetSystemMetrics(SM_CXSCREEN) / 2 : pImage->OrigWidth();
    int nOrigWidthUnzoomed = nOrigWidth;
    int nOrigHeight = (pImage == NULL) ? ::GetSystemMetrics(SM_CYSCREEN) / 2 : pImage->OrigHeight();
    if (dZoom == ZoomMax) {
        // NOTE: somewhat hacky, but the original code did not account for ZoomMax == DBL_MAX, causing overflows
        //       This workaround artificially calculates an image that is == the maximum allowed dimensions,
        //       so as to not overflow int or double
        //
        // when dZoom was unbounded to DBL_MAX in 1.1.44, doing any arithmetic would cause the nOrig* to overflow
        // So just set to scaled maximum allowed image dimension on one side
        // the math below will not overflow, and it stays within the bounds of an integer
        if (nOrigWidth == nOrigHeight) {
            // ratio is 1:1
            nOrigWidth = nOrigHeight = MAX_IMAGE_DIMENSION;
        } else if (nOrigWidth > nOrigHeight) {
            // wider than high
            nOrigHeight = (int)((double)nOrigHeight / nOrigWidth * MAX_IMAGE_DIMENSION);  // max * height/width ratio
            nOrigWidth = MAX_IMAGE_DIMENSION;
        } else {
            // higher than wide
            nOrigWidth = (int)((double)nOrigWidth / nOrigHeight * MAX_IMAGE_DIMENSION);  // max * width/height ratio
            nOrigHeight = MAX_IMAGE_DIMENSION;
        }
    } else if (dZoom > 0) {
        nOrigWidth = (int)(nOrigWidth * dZoom + 0.5);
        nOrigHeight = (int)(nOrigHeight * dZoom + 0.5);
    }

    CSize borderSize;
    if (!bWindowBorderless) {
        borderSize = GetTotalBorderSize();
    } else {
        borderSize = { 0, 0 };
    }

    int nRequiredWidth = borderSize.cx + nOrigWidth;
    int nRequiredHeight = borderSize.cy + nOrigHeight;
    CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
    if (bKeepAspectRatio && (nRequiredWidth > workingArea.Width() || nRequiredHeight > workingArea.Height())) {
        double dZoom;
        CSize imageRect = Helpers::GetImageRect(nOrigWidth, nOrigHeight, 
            std::min(maxSize.cx, workingArea.Width() - borderSize.cx),
            std::min(maxSize.cy, workingArea.Height() - borderSize.cy),
            false, false, false, eAutoZoomMode, dZoom);
        nRequiredWidth = imageRect.cx + borderSize.cx;
        nRequiredHeight = imageRect.cy + borderSize.cy;
    } else {
        nRequiredWidth = std::min<int>(maxSize.cx, std::min(workingArea.Width(), nRequiredWidth));
        nRequiredHeight = std::min<int>(maxSize.cy, std::min(workingArea.Height(), nRequiredHeight));
    }
    nRequiredWidth = std::max<int>(minSize.cx, nRequiredWidth);
    nRequiredHeight = std::max<int>(minSize.cy, nRequiredHeight);
    dZoom = ((double)(nRequiredWidth - borderSize.cx)) / nOrigWidthUnzoomed;
    CRect wndRect;
    ::GetWindowRect(hWnd, reinterpret_cast<LPRECT>(&wndRect));
    int nNewLeft = (wndRect.left + wndRect.right - nRequiredWidth) / 2;
    int nNewTop = (wndRect.top + wndRect.bottom - nRequiredHeight) / 2;
    nNewLeft = std::max<int>(workingArea.left, std::min<int>(workingArea.right - nRequiredWidth, nNewLeft));
    nNewTop = std::max<int>(workingArea.top, std::min<int>(workingArea.bottom - nRequiredHeight, nNewTop));

    if (!bForceCenterWindow) {
        return CRect(CPoint(nNewLeft, nNewTop), CSize(nRequiredWidth, nRequiredHeight));
    } else {
        return CRect(CPoint(workingArea.left + (workingArea.Width() - nRequiredWidth) / 2, workingArea.top + (workingArea.Height() - nRequiredHeight) / 2), CSize(nRequiredWidth, nRequiredHeight));
    }
}

bool CanDisplayImageWithoutResize(HWND hWnd, CJPEGImage* pImage) {
    if (pImage == NULL) {
        return true;
    }
    CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
    CSize borderSize = GetTotalBorderSize();
    return pImage->OrigWidth() + borderSize.cx <= workingArea.Width() && pImage->OrigHeight() + borderSize.cy <= workingArea.Height();
}

CSize GetMaxClientSize(HWND hWnd) {
    CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
    CSize borderSize = GetTotalBorderSize();
    return CSize(workingArea.Width() - borderSize.cx, workingArea.Height() - borderSize.cy);
}

EImageFormat GetImageFormat(LPCTSTR sFileName) {
    LPCTSTR sEnding = _tcsrchr(sFileName, _T('.'));
    if (sEnding != NULL) {
        sEnding += 1;
        if (_tcsicmp(sEnding, _T("JPG")) == 0 || _tcsicmp(sEnding, _T("JPEG")) == 0 || _tcsicmp(sEnding, _T("JFIF")) == 0) {
            return IF_JPEG;
        } else if (_tcsicmp(sEnding, _T("BMP")) == 0) {
            return IF_WindowsBMP;
        } else if (_tcsicmp(sEnding, _T("PNG")) == 0) {
            return IF_PNG;
        } else if (_tcsicmp(sEnding, _T("TIF")) == 0 || _tcsicmp(sEnding, _T("TIFF")) == 0) {
            return IF_TIFF;
        } else if (_tcsicmp(sEnding, _T("WEBP")) == 0) {
            return IF_WEBP;
        } else if (_tcsicmp(sEnding, _T("JXL")) == 0) {
            return IF_JXL;
        } else if (_tcsicmp(sEnding, _T("AVIF")) == 0) {
            return IF_AVIF;
        } else if (_tcsicmp(sEnding, _T("HEIF")) == 0 || _tcsicmp(sEnding, _T("HEIC")) == 0 || _tcsicmp(sEnding, _T("HIF")) == 0) {
            return IF_HEIF;
        } else if (_tcsicmp(sEnding, _T("TGA")) == 0) {
            return IF_TGA;
        } else if ((_tcsicmp(sEnding, _T("ZIP")) == 0) || (_tcsicmp(sEnding, _T("CBZ")) == 0)
            || (_tcsicmp(sEnding, _T("7Z")) == 0) || (_tcsicmp(sEnding, _T("CB7")) == 0)) {
            return IF_ZIP;
        } else if (_tcsicmp(sEnding, _T("QOI")) == 0) {
            return IF_QOI;
        } else if (_tcsicmp(sEnding, _T("PSD")) == 0) {
            return IF_PSD;
        }
    }
    return IF_Unknown;
}

// strstr() ignoring case
LPTSTR stristr(LPCTSTR szStringToBeSearched, LPCTSTR szSubstringToSearchFor) {
    LPCTSTR pPos = NULL;
    LPCTSTR szCopy1 = NULL;
    LPCTSTR szCopy2 = NULL;
     
    // verify parameters
    if ( szStringToBeSearched == NULL || szSubstringToSearchFor == NULL ) {
        return (LPTSTR)szStringToBeSearched;
    }
     
    // empty substring - return input (consistent with strstr)
    if ( _tcslen(szSubstringToSearchFor) == 0 ) {
        return (LPTSTR)szStringToBeSearched;
    }
     
    szCopy1 = _tcslwr(_tcsdup(szStringToBeSearched));
    szCopy2 = _tcslwr(_tcsdup(szSubstringToSearchFor));
     
    if ( szCopy1 == NULL || szCopy2 == NULL ) {
        // another option is to raise an exception here
        free((void*)szCopy1);
        free((void*)szCopy2);
        return NULL;
    }
     
    pPos = _tcsstr(szCopy1, szCopy2);
     
    if ( pPos != NULL ) {
        // map to the original string
        pPos = szStringToBeSearched + (pPos - szCopy1);
    }
     
    free((void*)szCopy1);
    free((void*)szCopy2);
     
    return (LPTSTR)pPos;
 } // stristr(...)

__int64 GetFileSize(LPCTSTR sPath) {
    HANDLE hFile = ::CreateFile(sPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return 0;
    }
    __int64 fileSize = 0;
    ::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
    ::CloseHandle(hFile);
    return fileSize;
}

__int64 GetFileSize(HANDLE hFile) {
    __int64 fileSize = 0;
    ::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
    return fileSize;
}

// Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
int GetFrameIndex(CJPEGImage* pImage, int iOffset, bool bSwitchFrame, bool & switchImage) {
    bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
    bool isAnimation = pImage != NULL && pImage->IsAnimation();
    bool isContainer = pImage != NULL && pImage->IsContainer();
    int nFrameIndex = 0;
    switchImage = true;
    if (isMultiFrame) {
        switchImage = false;
        nFrameIndex = pImage->FrameIndex() + iOffset;
        if (isAnimation) {
            if (bSwitchFrame) {
                if (nFrameIndex < 0) {
                    nFrameIndex = nFrameIndex + pImage->NumberOfFrames();
                } else if (nFrameIndex > pImage->NumberOfFrames() - 1) {
                    nFrameIndex = nFrameIndex - pImage->NumberOfFrames();
                }
            } else {
                switchImage = true;
                nFrameIndex = 0;
            }
        } else if (isContainer) {
            if (nFrameIndex < 0) {
                nFrameIndex = nFrameIndex + pImage->NumberOfFrames();
            } else if (nFrameIndex > pImage->NumberOfFrames() - 1) {
                nFrameIndex = nFrameIndex - pImage->NumberOfFrames();
            }
        } else {
            if (nFrameIndex < 0 || nFrameIndex >= pImage->NumberOfFrames()) {
                nFrameIndex = 0;
                switchImage = true;
            }
        }
    }
    if (bSwitchFrame && pImage == NULL) {
        switchImage = false; // never switch image when error during animation playing
    }

    return nFrameIndex;
}

int GetWindowsVersion() {
#pragma warning(push)
#pragma warning(disable:4996)
    OSVERSIONINFO osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osvi);
#pragma warning(pop)
    return osvi.dwMajorVersion * 100 + osvi.dwMinorVersion;
}

// Based on: https://github.com/sdneon/jpegview.git
    void BlendAlpha(uint32* pImage32, int nWidth, int nHeight, Helpers::ETransparencyMode nTransparencyMode)
    {
        COLORREF nTransparency = CSettingsProvider::This().ColorTransparency();
        if (nTransparencyMode != Helpers::TP_CHECKERBOARD) {
            if (nTransparencyMode == Helpers::TP_BLEND_INVERSE) {
                nTransparency = ~(nTransparency & 0xffffff);
            }
            for (int i = 0; i < nWidth * nHeight; ++i) {
                *pImage32 = AlphaBlendBackground(*pImage32, nTransparency);
                pImage32++;
            }
        } else {
            for (int y = 0; y < nHeight; ++y) {
                for (int x = 0; x < nWidth; ++x) {
                    *pImage32 = AlphaBlendBackground(*pImage32, nTransparency, true, x, y);
                    pImage32++;
                }
            }
        }
    }

    uint32_t GetTimeMs() {
        using namespace std::chrono;
        auto now = steady_clock::now();
        return static_cast<uint32_t>(duration_cast<milliseconds>(now.time_since_epoch()).count());
    }

} // namespace Helpers

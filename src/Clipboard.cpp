#include "Clipboard.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include <gdiplus.h>

// Hilfsfunktion: Wandelt Windows-Pfade (z. B. C:\Bilder\test.jpg) in file://-URIs um,
// die KDE/Wine verstehen.
static std::string PathToFileUri(LPCWSTR filename) {
    // Falls unter Wine ausgefuehrt, optional den echten Unix-Pfad ermitteln
    typedef char* (__cdecl* pfnWineGetUnixFileName)(LPCWSTR);
    static pfnWineGetUnixFileName pWineGetUnixFileName =
        (pfnWineGetUnixFileName)::GetProcAddress(::GetModuleHandle(L"kernel32.dll"), "wine_get_unix_file_name");

    std::string unixPath;
    if (pWineGetUnixFileName) {
        char* pPath = pWineGetUnixFileName(filename);
        if (pPath) {
            unixPath = pPath;
            ::HeapFree(::GetProcessHeap(), 0, pPath);
        }
    }

    // Fallback, falls nicht unter Wine oder Konvertierung fehlgeschlagen:
    // Erstelle Standard-URI aus Windows-Pfad (z.B. C:/Bilder/test.jpg -> file:///C:/Bilder/test.jpg)
    if (unixPath.empty()) {
        int size_needed = ::WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
        std::string utf8Path(size_needed - 1, 0);
        ::WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8Path[0], size_needed, NULL, NULL);

        for (auto& ch : utf8Path) {
            if (ch == '\\') ch = '/';
        }
        unixPath = "/" + utf8Path;
    }

    return "file://" + unixPath;
}

static void CopyOriginalFileNameToClipboard(LPCWSTR filename) {
    int fileNameLength = (int)wcslen(filename);
    DWORD fileNameLengthBytes = sizeof(wchar_t) * (fileNameLength + 1);
    DROPFILES df = {sizeof(DROPFILES), {0, 0}, 0, TRUE};
    HGLOBAL hMem = ::GlobalAlloc(GMEM_ZEROINIT|GMEM_MOVEABLE|GMEM_DDESHARE, sizeof(DROPFILES) + fileNameLengthBytes + sizeof(wchar_t)); // for double NULL char
    int offset = sizeof(DROPFILES) / sizeof(wchar_t);
    WCHAR *pGlobal = (WCHAR *) ::GlobalLock(hMem);
    ::CopyMemory(pGlobal, &df, sizeof(DROPFILES));
    ::CopyMemory(pGlobal + offset, filename, fileNameLengthBytes); // that's pGlobal + 20 bytes (the size of DROPFILES);
    pGlobal[offset + fileNameLength + 1] = 0; // add additional NULL character
    ::GlobalUnlock(hMem);
    ::SetClipboardData(CF_HDROP, hMem);
}

static void CopyFileNameTextToClipboard(LPCWSTR filename) {
    int fileNameLength = (int)wcslen(filename);
    DWORD fileNameLengthBytes = sizeof(wchar_t) * (fileNameLength + 1);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_ZEROINIT | GMEM_MOVEABLE | GMEM_DDESHARE, fileNameLengthBytes + sizeof(wchar_t)); // for double NULL char
    WCHAR* pGlobal = (WCHAR*) ::GlobalLock(hMem);
    ::CopyMemory(pGlobal, filename, fileNameLengthBytes);
    pGlobal[fileNameLength + 1] = 0; // add additional NULL character
    ::GlobalUnlock(hMem);
    ::SetClipboardData(CF_UNICODETEXT, hMem);
}

void CClipboard::CopyImageToClipboard(HWND hWnd, CJPEGImage * pImage, LPCTSTR fileName) {
    if (pImage == NULL || pImage->DIBPixelsLastProcessed(true) == NULL) {
        return;
    }

    pImage->EnableDimming(false);
    DoCopy(hWnd, pImage->DIBWidth(), pImage->DIBHeight(), pImage->DIBPixelsLastProcessed(true), pImage->IsClipboardImage() ? NULL : fileName);
    pImage->EnableDimming(true);
}

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, const CImageProcessingParams& procParams,
                                          EProcessingFlags eFlags, LPCTSTR fileName) {
    CopyFullImageToClipboard(hWnd, pImage, procParams, eFlags, CRect(0, 0, pImage->OrigWidth(), pImage->OrigHeight()), fileName);
}

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, const CImageProcessingParams& procParams,
                                          EProcessingFlags eFlags, CRect clipRect, LPCTSTR fileName) {
    if (pImage == NULL) {
        return;
    }

    clipRect.left = std::max<LONG>(0, clipRect.left);
    clipRect.top = std::max<LONG>(0, clipRect.top);
    clipRect.right = std::min<LONG>(pImage->OrigWidth(), clipRect.right);
    clipRect.bottom = std::min<LONG>(pImage->OrigHeight(), clipRect.bottom);

    pImage->EnableDimming(false);
    void* pDIB = pImage->GetDIB(pImage->OrigSize(), clipRect.Size(), clipRect.TopLeft(), procParams, eFlags);
    DoCopy(hWnd, clipRect.Width(), clipRect.Height(), pDIB, pImage->IsClipboardImage() ? NULL : fileName);
    pImage->EnableDimming(true);
}

void CClipboard::CopyPathToClipboard(HWND hWnd, CJPEGImage* pImage, LPCTSTR fileName) {
    if (pImage == NULL || pImage->DIBPixelsLastProcessed(true) == NULL) {
        return;
    }
    pImage->EnableDimming(false);
    DoCopyFileNameText(hWnd, fileName);
    pImage->EnableDimming(true);
}

void CClipboard::CutFileToClipboard(HWND hWnd, LPCTSTR fileName) {
    if (!::OpenClipboard(hWnd)) {
        return;
    }
    ::EmptyClipboard();

    // 1. Dateipfad als HDROP in die Zwischenablage schreiben
    CopyOriginalFileNameToClipboard(fileName);

    // 2. Preferred Drop Effect auf DROPEFFECT_MOVE setzen (Ausschneiden/Verschieben)
    UINT uFormat = ::RegisterClipboardFormat(TEXT("Preferred DropEffect"));
    if (uFormat != 0) {
        HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (hMem != NULL) {
            DWORD* pDropEffect = (DWORD*)::GlobalLock(hMem);
            if (pDropEffect != NULL) {
                *pDropEffect = DROPEFFECT_MOVE;
                ::GlobalUnlock(hMem);

                // Nach SetClipboardData uebernimmt das System die Speicherverwaltung von hMem
                if (::SetClipboardData(uFormat, hMem) == NULL) {
                    ::GlobalFree(hMem);
                }
            } else {
                ::GlobalFree(hMem);
            }
        }
    }

    // 2. Linux / KDE-spezifisches Format (application/x-kde-cutselection = "1")
    UINT uFormatKdeCut = ::RegisterClipboardFormat(L"application/x-kde-cutselection");
    if (uFormatKdeCut != 0) {
        HGLOBAL hMemKde = ::GlobalAlloc(GMEM_MOVEABLE, 1);
        if (hMemKde != NULL) {
            char* pData = (char*)::GlobalLock(hMemKde);
            if (pData != NULL) {
                pData[0] = '1';
                ::GlobalUnlock(hMemKde);
                if (::SetClipboardData(uFormatKdeCut, hMemKde) == NULL) ::GlobalFree(hMemKde);
            }
            else {
                ::GlobalFree(hMemKde);
            }
        }
    }

    // 3. Linux / Freedesktop Standard (x-special/gnome-copied-files)
    // Format der Nutzdaten: "cut\nfile:///pfad/zur/datei.jpg"
    UINT uFormatGnomeCut = ::RegisterClipboardFormat(L"x-special/gnome-copied-files");
    if (uFormatGnomeCut != 0) {
        std::string fileUri = PathToFileUri(fileName);
        std::string gnomePayload = "cut\n" + fileUri + "\n";

        HGLOBAL hMemGnome = ::GlobalAlloc(GMEM_MOVEABLE, gnomePayload.size());
        if (hMemGnome != NULL) {
            char* pData = (char*)::GlobalLock(hMemGnome);
            if (pData != NULL) {
                ::CopyMemory(pData, gnomePayload.c_str(), gnomePayload.size());
                ::GlobalUnlock(hMemGnome);
                if (::SetClipboardData(uFormatGnomeCut, hMemGnome) == NULL) ::GlobalFree(hMemGnome);
            }
            else {
                ::GlobalFree(hMemGnome);
            }
        }
    }

    ::CloseClipboard();
}

void CClipboard::DoCopy(HWND hWnd, int nWidth, int nHeight, const void* pSourceImageDIB32, LPCTSTR fileName) {
    if (!::OpenClipboard(hWnd)) {
        return;
    }
    ::EmptyClipboard();

    // get needed size of memory block
    uint32 nSizeLinePadded = Helpers::DoPadding(nWidth*3, 4);
    uint32 nSizeBytes = sizeof(BITMAPINFO) + nSizeLinePadded * nHeight;

    // Allocate memory
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, nSizeBytes);
    if (hMem == NULL) {
        ::CloseClipboard();
        return;
    }
    void* pMemory = ::GlobalLock(hMem);

    BITMAPINFO* pBMInfo = (BITMAPINFO*) pMemory;
    memset(pBMInfo, 0, sizeof(BITMAPINFO));

    pBMInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pBMInfo->bmiHeader.biWidth = nWidth;
    pBMInfo->bmiHeader.biHeight = nHeight;
    pBMInfo->bmiHeader.biPlanes = 1;
    pBMInfo->bmiHeader.biBitCount = 24;
    pBMInfo->bmiHeader.biCompression = BI_RGB;
    pBMInfo->bmiHeader.biXPelsPerMeter = 10000;
    pBMInfo->bmiHeader.biYPelsPerMeter = 10000;
    pBMInfo->bmiHeader.biClrUsed = 0;

    uint8* pDIBPixelsTarget = (uint8*)pMemory + sizeof(BITMAPINFO) - sizeof(RGBQUAD);
    CBasicProcessing::Convert32bppTo24bppDIB(nWidth, nHeight, pDIBPixelsTarget, pSourceImageDIB32, true);

    ::GlobalUnlock(hMem);
    ::SetClipboardData(CF_DIB, hMem);

    if (fileName != NULL) {
        CopyOriginalFileNameToClipboard(fileName);
    }

    ::CloseClipboard();
}

void CClipboard::DoCopyFileNameText(HWND hWnd, LPCTSTR fileName) {
    if (!::OpenClipboard(hWnd)) {
        return;
    }
    ::EmptyClipboard();

    CopyFileNameTextToClipboard(fileName);

    ::CloseClipboard();
}
// MainDlg.cpp : implementation of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#include "MainDlg.h"
#include "FileList.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "MultiMonitorSupport.h"
#include "HistogramCorr.h"
#include "Clipboard.h"
#include "ProcessParams.h"
#include "NLS.h"
#include "TimerEventIDs.h"
#include "ResizeFilter.h"
#include "ProcessingThreadPool.h"
#include "KeyMap.h"
#include "DirectoryWatcher.h"

#include <regex>        // used in regex matching language tags like [en], [de]

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMessageBox>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QRect>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QtGlobal>

namespace {

    const int MIN_WND_WIDTH = 320;
    const int MIN_WND_HEIGHT = 240;

    const int NUM_THREADS = 1; // number of readahead threads to use. Use "1" since the ImageLoaders aren't thread save and JxlReader/PngReaderer/WebpReaderWriter can't do out of order frame access!
    const int ZOOM_TIMEOUT = 50; // refinement done after this many milliseconds
    const int ZOOM_TEXT_TIMEOUT = 1000; // zoom label disappears after this many milliseconds

    const int ZOOM_TEXT_RECT_WIDTH = 70; // zoom label width
    const int ZOOM_TEXT_RECT_HEIGHT = 25; // zoom label height
    const int ZOOM_TEXT_RECT_OFFSET_X = 30; // zoom label offset from right border
    const int ZOOM_TEXT_RECT_OFFSET_Y = 20; // zoom label offset from bottom border

    const int TOAST_TEXT_RECT_OFFSET_X = 30;
    const int TOAST_TEXT_RECT_OFFSET_Y = 20;
    const int TOAST_TEXT_RECT_HEIGHT = 60;

    const int PAN_STEP = 48; // number of pixels to pan if pan with cursor keys (SHIFT+up/down/left/right)

    const int NO_REQUEST = 1; // used in GotoImage() method
    const int NO_REMOVE_KEY_MSG = 2; // used in GotoImage() method
    const int KEEP_PARAMETERS = 4; // used in GotoImage() method
    const int NO_UPDATE_WINDOW = 8; // used in GotoImage() method

    const CSize HUGE_SIZE = CSize(99999999, 99999999);

    HFONT hDefaultGUIFont = nullptr;
    HFONT hToastFont = nullptr;
    HFONT hDefaultZoomFactorFont = nullptr;

    typedef const char* (__cdecl* pwine_get_version)(void); // Define function pointer type for wine_get_version

    // Errors that occur during loading a file
    enum EFileLoadError {
        FileLoad_Ok = 0,
        FileLoad_LoadError = 1,
        FileLoad_SlideShowListInvalid = 2,
        FileLoad_NoFilesInDirectory = 3,

        // these can be combined with other error codes, so make sure they work with bit arithmetic
        FileLoad_OutOfMemory = 65536,
        FileLoad_ExceptionError = 32768
    };

    // Creates a font from the INI file description:
    // "Font face" fontSizePoints [bold]
    // Returns NULL if default font shall be used
    HFONT CreateFontFromINIDescription(HDC dc, const QString& sIniFileDescription)
    {
        QString sIniDesc = sIniFileDescription.trimmed();

        if (sIniDesc.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0 || sIniDesc.isEmpty()) {
            return nullptr;
        }

        QString sFontName;
        float fFontSize = 9.0f;
        bool isBold = false;

        // RegEx: Sucht optional nach "Font Name" oder FontName, gefolgt von der Schriftgröße und optional "bold"
        static const QRegularExpression rx(
            QStringLiteral("(?:\"([^\"]+)\"|(\\S+))\\s+(?:(\\d+(?:\\.\\d+)?)\\s*)?(bold)?"),
            QRegularExpression::CaseInsensitiveOption
        );

        QRegularExpressionMatch match = rx.match(sIniDesc);
        if (match.hasMatch()) {
            // Schriftname (Gruppe 1 bei Anführungszeichen, Gruppe 2 ohne)
            sFontName = !match.captured(1).isEmpty() ? match.captured(1) : match.captured(2);

            // Schriftgröße (Gruppe 3)
            if (!match.captured(3).isEmpty()) {
                bool ok = false;
                float parsedSize = match.captured(3).toFloat(&ok);
                if (ok && parsedSize > 0.0f) {
                    fFontSize = std::clamp(parsedSize, 7.0f, 30.0f);
                }
            }

            // Bold-Flag (Gruppe 4)
            isBold = !match.captured(4).isEmpty();
        } else {
            // Fallback: Nur ein Font-Name ohne Größenangabe
            sFontName = sIniDesc.trimmed().remove(QLatin1Char('"'));
        }

        if (sFontName.isEmpty()) {
            return nullptr;
        }

        const int nHeight = -static_cast<int>(0.5f + fFontSize * ::GetDeviceCaps(dc, LOGPIXELSY) / 72.0f);

        return ::CreateFontW(
            nHeight, 0, 0, 0,
            isBold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            VARIABLE_PITCH,
            reinterpret_cast<LPCWSTR>(sFontName.utf16())
        );
    }

    // Gets default image processing parameters as set in INI file
    CImageProcessingParams GetDefaultProcessingParams() {
        CSettingsProvider& sp = CSettingsProvider::This();
        return CImageProcessingParams(
            sp.Contrast(),
            sp.Gamma(),
            sp.Saturation(),
            0.0, // ColorCorrectionFactor
            0.5); // ContrastCorrectionFactor
    }

    // Gets hardcoded processing parameters, turning off all processing
    CImageProcessingParams GetNoProcessingParams() {
        return CImageProcessingParams(0.0, 1.0, 1.0, 0.0, 0.5);
    }

    // Gets default image processing flags as set in INI file
    EProcessingFlags GetDefaultProcessingFlags() {
        CSettingsProvider& sp = CSettingsProvider::This();
        EProcessingFlags eProcFlags = PFLAG_None;
        eProcFlags = SetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling, sp.HighQualityResampling());
        eProcFlags = SetProcessingFlag(eProcFlags, PFLAG_AutoContrast, sp.AutoContrastCorrection());
        return eProcFlags;
    }

    // Create processing flags from literal boolean values
    EProcessingFlags CreateProcessingFlags(bool bHQResampling, bool bAutoContrast, bool bKeepParams) {
        EProcessingFlags eProcFlags = PFLAG_None;
        eProcFlags = SetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling, bHQResampling);
        eProcFlags = SetProcessingFlag(eProcFlags, PFLAG_AutoContrast, bAutoContrast);
        eProcFlags = SetProcessingFlag(eProcFlags, PFLAG_KeepParams, bKeepParams);
        return eProcFlags;
    }

    // Initialize boolean processing values from flag bitfield
    void InitFromProcessingFlags(EProcessingFlags eFlags, bool& bHQResampling, bool& bAutoContrast) {
        bHQResampling = GetProcessingFlag(eFlags, PFLAG_HighQualityResampling);
        bAutoContrast = GetProcessingFlag(eFlags, PFLAG_AutoContrast);
    }

    // If dZoom > 0: Gets the virtual image size when applying the given zoom factor
    // If dZoom < 0: Gets the image size to fit the image to the given screen size, using the given auto zoom mode.
    //               dZoom is filled with the used zoom factor to resize to this new size in this case.
    // The returned size is limited to 65535 in cx and cy
    CSize GetVirtualImageSize(CSize originalImageSize, CSize screenSize, Helpers::EAutoZoomMode eAutoZoomMode, double & dZoom) {
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

		if (newSize.cx > 65535 || newSize.cy > 65535) {
			double dFac = 65535.0/std::max(newSize.cx,newSize.cy);
			dZoom = dZoom*dFac;
			newSize.cx = (int)(originalImageSize.cx * dZoom + 0.5);
			newSize.cy = (int)(originalImageSize.cy * dZoom + 0.5);
		}

		return newSize;
	}

    // Limits the given center based offsets so that the rectangle created by the offset and the given rectSize is totally inside of outerRect
    // outerRect has top,left coordinates 0/0
	CPoint LimitOffsets(const CPoint& offsets, const CSize & rectSize, const CSize & outerRect) {
		int nMaxOffsetX = (outerRect.cx - rectSize.cx)/2;
		nMaxOffsetX = std::max(0, nMaxOffsetX);
		int nMaxOffsetY = (outerRect.cy - rectSize.cy)/2;
		nMaxOffsetY = std::max(0, nMaxOffsetY);
		return CPoint(std::max<long>(-nMaxOffsetX, std::min<int>(+nMaxOffsetX, offsets.x)), std::max<long>(-nMaxOffsetY, std::min<int>(+nMaxOffsetY, offsets.y)));
	}


    // Gets a window rectangle (in screen coordinates) that fits the given image
    CRect GetWindowRectMatchingImageSize(HWND hWnd, CSize minSize, CSize maxSize, double& dZoom, CJPEGImage* pImage, bool bForceCenterWindow, bool bKeepAspectRatio, bool bWindowBorderless, Helpers::EAutoZoomMode eAutoZoomMode) {
		int nOrigWidth = (pImage == NULL) ? ::GetSystemMetrics(SM_CXSCREEN) / 2 : pImage->OrigWidth();
		int nOrigWidthUnzoomed = nOrigWidth;
		int nOrigHeight = (pImage == NULL) ? ::GetSystemMetrics(SM_CYSCREEN) / 2 : pImage->OrigHeight();
        if (dZoom == Helpers::ZoomMax) {
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
            borderSize = Helpers::GetTotalBorderSize();
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
	
    // Gets if the given image can be displayed without sampling down the image.
	bool CanDisplayImageWithoutResize(HWND hWnd, CJPEGImage* pImage) {
		if (pImage == NULL) {
			return true;
		}
		CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
        CSize borderSize = Helpers::GetTotalBorderSize();
		return pImage->OrigWidth() + borderSize.cx <= workingArea.Width() && pImage->OrigHeight() + borderSize.cy <= workingArea.Height();
	}

    // Gets the maximum client size for a framed window that fits into the working area of the screen the given window is placed on
	CSize GetMaxClientSize(HWND hWnd) {
		CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
        CSize borderSize = Helpers::GetTotalBorderSize();
		return CSize(workingArea.Width() - borderSize.cx, workingArea.Height() - borderSize.cy);
	}

    // Draws an error text for the given file loading error, and detailed load error code (bit combination of EFileLoadError codes)
    void DrawImageLoadErrorText(CDC& dc, const CRect& clientRect, const QString& failedFileName, int nFileLoadError, int nLoadErrorDetail) {
        const bool bOutOfMemory = (nLoadErrorDetail & FileLoad_OutOfMemory) != 0;
        const bool bExceptionError = (nLoadErrorDetail & FileLoad_ExceptionError) != 0;

        QString message;

        switch (nFileLoadError) {
            case FileLoad_LoadError:
                message = QStringLiteral("The file '%1' could not be read!").arg(failedFileName);
                break;

            case FileLoad_SlideShowListInvalid:
                message = QStringLiteral("The file '%1' does not contain a list of file names!").arg(failedFileName);
                break;

            case FileLoad_NoFilesInDirectory:
                message = QStringLiteral("The directory '%1' does not contain any image files!").arg(failedFileName) +
                        QLatin1Char('\n') +
                        QStringLiteral("Press any key to search the subdirectories for image files.");
                break;

            default:
                return;
        }

        if (bOutOfMemory) {
            message += QLatin1Char('\n');
            message += QStringLiteral("Reason:") + QLatin1Char(' ') +
                       QStringLiteral("Not enough memory available");
        }
        else if (nFileLoadError == FileLoad_LoadError) {
            const QString ext = QFileInfo(failedFileName).suffix();

            if (ext.compare(QStringLiteral("JXL"), Qt::CaseInsensitive) == 0 ||
                ext.compare(QStringLiteral("HEIF"), Qt::CaseInsensitive) == 0 ||
                ext.compare(QStringLiteral("HEIC"), Qt::CaseInsensitive) == 0 ||
                ext.compare(QStringLiteral("AVIF"), Qt::CaseInsensitive) == 0) {

                if (bExceptionError) {
                    message += QLatin1Char('\n');
                    message += QStringLiteral("Decoding this format requires the Microsoft Visual C++ Redistributable.");
                }
            }
        }

        CRect rectText(0, clientRect.Height()/2 - Helpers::ScaleWithDPI(40), clientRect.Width(), clientRect.Height());
        dc.DrawText(reinterpret_cast<LPCTSTR>(message.utf16()), -1, &rectText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        //QRect rectText(0, clientRect.height() / 2 - Helpers::ScaleWithDPI(40), clientRect.width(), clientRect.height());
        //painter.drawText(rectText, Qt::AlignHCenter | Qt::TextWordWrap, message);
    }

    void SelectDefaultGUIFont(HDC dc) {
        if (hDefaultGUIFont == nullptr) {
            hDefaultGUIFont = CreateFontFromINIDescription(dc, CSettingsProvider::This().DefaultGUIFont());
            if (hDefaultGUIFont == nullptr)
                hDefaultGUIFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
        }
        ::SelectObject(dc, hDefaultGUIFont);
    }

    void SelectDefaultToastFont(HDC dc) {
        if (hToastFont == nullptr) {
            hToastFont = CreateFontFromINIDescription(dc, QStringLiteral("\"Arial\" 12.0 bold"));
        }
        ::SelectObject(dc, hToastFont);
    }

    void SelectDefaultZoomFactorFont(HDC dc) {
        if (hDefaultZoomFactorFont == nullptr) {
            hDefaultZoomFactorFont = CreateFontFromINIDescription(dc, CSettingsProvider::This().ZoomFactorFont());
            if (hDefaultZoomFactorFont == nullptr)
                hDefaultZoomFactorFont = CreateFontFromINIDescription(dc, QStringLiteral("\"Arial\" 10.0 bold"));
        }
        ::SelectObject(dc, hDefaultZoomFactorFont);
    }

    void TranslateMenuStrings(HMENU hMenu, CKeyMap* pKeyMap) {
        int nMenuItemCount = ::GetMenuItemCount(hMenu);
        for (int i = 0; i < nMenuItemCount; i++) {
            const int TEXT_BUFF_LEN = 128;
            wchar_t menuText[TEXT_BUFF_LEN];
            ::GetMenuStringW(hMenu, i, menuText, TEXT_BUFF_LEN, MF_BYPOSITION);
            menuText[TEXT_BUFF_LEN - 1] = 0;

            QString menuTextStr = QString::fromWCharArray(menuText);
            QString sBaseMenuText = menuTextStr;
            QString sTabPart;

            int tabIdx = menuTextStr.indexOf(QChar('\t'));
            if (tabIdx != -1) {
                sBaseMenuText = menuTextStr.left(tabIdx);
                sTabPart = menuTextStr.mid(tabIdx + 1);
            }

            QString sKeyDesc;
            if (pKeyMap != NULL) {
                int nMenuItemId = ::GetMenuItemID(hMenu, i);
                sKeyDesc = pKeyMap->GetKeyStringForCommand(nMenuItemId);
            }

            QString sNewMenuText = CNLS::GetString(sBaseMenuText);

            bool hasTab = !sTabPart.isEmpty();
            if (sNewMenuText != sBaseMenuText || !sKeyDesc.isEmpty() || hasTab) {
                if (hasTab) {
                    sNewMenuText += QLatin1Char('\t');
                    if (sTabPart.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
                        // Hexadezimale Mappings auswerten (äquivalent zu _stscanf)
                        int nCommand = 0;
                        swscanf(reinterpret_cast<const wchar_t*>(sTabPart.utf16()), L"%x", &nCommand);
                        if (nCommand == 0xFFFF) {
                            // Exit JPEGView is special
                            QString shortCutExit = pKeyMap ? pKeyMap->GetKeyStringForCommand(0xFF) : QString();
                            if (shortCutExit.isEmpty() && pKeyMap) {
                                shortCutExit = pKeyMap->GetKeyStringForCommand(IDM_EXIT);
                            }
                            sNewMenuText += shortCutExit;
                        } else if (pKeyMap) {
                            sNewMenuText += pKeyMap->GetKeyStringForCommand(nCommand);
                        }
                    } else {
                        // Wenn nach dem \t kein Hex-Code steht, diesen Teil übersetzt anhängen
                        QString translatedTab = CNLS::GetString(sTabPart);
                        sNewMenuText += translatedTab;
                    }
                } else if (!sKeyDesc.isEmpty()) {
                    sNewMenuText += QLatin1Char('\t');
                    sNewMenuText += sKeyDesc;
                }

                MENUITEMINFO menuInfo{ 0 };
                menuInfo.cbSize = sizeof(MENUITEMINFO);
                menuInfo.fMask = MIIM_STRING;
                menuInfo.dwTypeData = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(sNewMenuText.utf16()));
                ::SetMenuItemInfoW(hMenu, i, TRUE, &menuInfo);
            }

            HMENU hSubMenu = ::GetSubMenu(hMenu, i);
            if (hSubMenu != NULL) {
                TranslateMenuStrings(hSubMenu, pKeyMap);
            }
        }
    }

    // Draws a text with a black outline to improve readability. nFormat is according to format in GDI DrawText method.
    void DrawTextBordered(CDC& dc, LPCTSTR sText, const CRect& rect, UINT nFormat) {
        int oldBkMode = dc.GetBkMode();
        if (oldBkMode != TRANSPARENT) {
            dc.SetBkMode(TRANSPARENT);
        }

        COLORREF oldColor = dc.SetTextColor(RGB(0, 0, 0));

        CRect textRect = rect;
        textRect.InflateRect(-1, -1);

        textRect.OffsetRect(-1, 0); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
        textRect.OffsetRect(2, 0); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
        textRect.OffsetRect(-1, -1); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
        textRect.OffsetRect(0, 2); dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);
        textRect.OffsetRect(0, -1);

        dc.SetTextColor(oldColor);
        dc.DrawText(sText, -1, &textRect, nFormat | DT_NOPREFIX);

        if (oldBkMode != TRANSPARENT) {
            dc.SetBkMode(oldBkMode);
        }
    }

    // Draws a 32 bit DIB centered in the given target area, filling the remaining area with the given brush
    // The bmInfo struct will be initialized by this method and does not need to be preinitialized.
    // Return value is the top, left coordinate of the painted DIB in the target area
    CPoint DrawDIB32bppWithBlackBorders(CDC& dc, BITMAPINFO& bmInfo, void* pDIBData, HBRUSH backBrush, const CRect& targetArea, CSize dibSize, CPoint offset) {
        memset(&bmInfo, 0, sizeof(BITMAPINFO));
        bmInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmInfo.bmiHeader.biWidth = dibSize.cx;
        bmInfo.bmiHeader.biHeight = -dibSize.cy;
        bmInfo.bmiHeader.biPlanes = 1;
        bmInfo.bmiHeader.biBitCount = 32;
        bmInfo.bmiHeader.biCompression = BI_RGB;
        int xDest = (targetArea.Width() - dibSize.cx) / 2 + offset.x;
        int yDest = (targetArea.Height() - dibSize.cy) / 2 + offset.y;

        // remaining client area is painted black
        if (xDest > 0) {
            CRect r(0, 0, xDest, targetArea.Height());
            dc.FillRect(&r, backBrush);
        }
        if (xDest + dibSize.cx < targetArea.Width()) {
            CRect r(xDest + dibSize.cx, 0, targetArea.Width(), targetArea.Height());
            dc.FillRect(&r, backBrush);
        }
        if (yDest > 0) {
            CRect r(xDest, 0, xDest + dibSize.cx, yDest);
            dc.FillRect(&r, backBrush);
        }
        if (yDest + dibSize.cy < targetArea.Height()) {
            CRect r(xDest, yDest + dibSize.cy, xDest + dibSize.cx, targetArea.Height());
            dc.FillRect(&r, backBrush);
        }

        dc.SetDIBitsToDevice(xDest, yDest, dibSize.cx, dibSize.cy, 0, 0, 0, dibSize.cy, pDIBData,
                             &bmInfo, DIB_RGB_COLORS);

        return CPoint(xDest, yDest);
    }

} // namespace

//////////////////////////////////////////////////////////////////////////////////////////////
// Public
//////////////////////////////////////////////////////////////////////////////////////////////

CMainDlg::CMainDlg(LPCTSTR sStartupFile) {
    m_sStartupFile = QString::fromWCharArray(sStartupFile);

    CSettingsProvider& sp = CSettingsProvider::This();

    CResizeFilterCache::This(); // Access before multiple threads are created

    // Read the string table for the requested language if one is present
    CNLS::ReadStringTable(CNLS::GetStringTableFileName(sp.Language()));

    m_pImageProcParams = new CImageProcessingParams(GetDefaultProcessingParams());
    InitFromProcessingFlags(GetDefaultProcessingFlags(), m_bHQResampling, m_bAutoContrast);

    // Initialize second parameter set using hard coded values, turning off all corrections
    m_pImageProcParams2 = new CImageProcessingParams(GetNoProcessingParams()); 
    m_eProcessingFlags2 = PFLAG_HighQualityResampling;

    m_pImageProcParamsKept = new CImageProcessingParams();
    m_eProcessingFlagsKept = PFLAG_HighQualityResampling;
    m_dZoomKept = -1;
    m_offsetKept = CPoint(0, 0);
    m_bCurrentImageInParamDB = false;

    m_bKeepParams = sp.KeepParams();
    m_eAutoZoomModeWindowed = sp.AutoZoomMode();
    m_eAutoZoomModeFullscreen = sp.AutoZoomModeFullscreen();

    m_eTransitionEffect = sp.SlideShowTransitionEffect();
    m_nTransitionTime = sp.SlideShowEffectTimeMs();

    CHistogramCorr::SetContrastCorrectionStrength((float)sp.AutoContrastAmount());
    CHistogramCorr::SetBrightnessCorrectionStrength((float)sp.AutoBrightnessAmount());


    m_pFileList = NULL;
    m_pJPEGProvider = NULL;
    m_pCurrentImage = NULL;
    m_bOutOfMemoryLastImage = false;
    m_bExceptionErrorLastImage = false;
    m_nLastLoadError = FileLoad_Ok;

    m_bAutoExit = false;
    m_nLastSlideShowImageTickCount = 0;
    m_dMovieFPS = 1.0;
    m_nAutoStartSlideShow = false;
    m_eForcedSorting = Helpers::FS_Undefined;

    m_eProcFlagsBeforeMovie = PFLAG_None;
    m_nRotation = 0;
    m_nUserRotation = 0;
    m_dZoomAtResizeStart = 1.0;
    m_bZoomMode = false;
    m_bZoomModeOnLeftMouse = false;
    m_bUserZoom = false;
    m_bUserPan = false;
    m_bMovieMode = false;
    m_bProcFlagsTouched = false;
    m_bInTrackPopupMenu = false;
    m_bResizeForNewImage = false;
    m_dZoom = -1.0;
    m_dStartZoom = -1.0;
    m_dRealizedZoom = -1.0;
    m_dZoomMult = -1.0;
    m_bDragging = false;
    m_bDoDragging = false;
    m_offsets = CPoint(0, 0);
    m_DIBOffsets = CPoint(0, 0);
    m_nCapturedX = m_nCapturedY = 0;
    m_nMouseX = m_nMouseY = 0;
    m_bAutoFitWndToImage = sp.DefaultWndToImage();
    m_bFullScreenMode = (sp.ShowFullScreen() && !sp.AutoFullScreen());
    m_bLockPaint = true;
    m_nCurrentTimeout = 0;
    m_startMouse.x = m_startMouse.y = -1;
    m_virtualImageSize = CSize(-1, -1);
    m_bInZooming = false;
    m_bTemporaryLowQ = false;
    m_bShowZoomFactor = false;
    m_bSpanVirtualDesktop = false;
    m_bPanMouseCursorSet = false;
    m_storedWindowPlacement.length = sizeof(WINDOWPLACEMENT);
    m_nMonitor = 0;
    m_monitorRect = CRect(0, 0, 0, 0);
    m_windowRectOnClose = CRect(0, 0, 0, 0);
    m_bMouseOn = false;
    m_bKeepParametersBeforeAnimation = false;
    m_bIsAnimationPlaying = false;
    m_nLastAnimationOffset = 0;
    m_nExpectedNextAnimationTickCount = 0;
    m_isUserFitToScreen = false;
    m_autoZoomFitToScreen = Helpers::ZM_FillScreen;
    m_bWindowBorderless = sp.WindowBorderlessOnStartup();  // unlike AlwaysOnTop, this is set early on initialize as it affects calculations of the window size, position, etc
    m_bAlwaysOnTop = false;  // default normal window.  this will be set to true when AlwaysOnTop is toggled if set to startup in INI
    m_bSelectZoom = false;  // this value is set when LButtonDown happens, to be read by LButtonUp

    m_pKeyMap = new CKeyMap(); // routine to load the keymap, it's not as simple as just loading one file anymore, but all logic handled by CKeyMap

    m_eTransparencyMode = sp.TransparencyMode();
    m_bUseEmbeddedColorProfiles = sp.UseEmbeddedColorProfiles();

    /*############################################*/
    /* Custom variables of the linear scaling mod */
    /*############################################*/

    m_hmodDwmapi = NULL;
    m_bDWMenabled = FALSE;
    m_DynDwmFlush = 0;

    m_bIsRunningInWine = isRunningInWine();
    
    m_storedWindowPlacement2.length = sizeof(WINDOWPLACEMENT);
    memset(&m_storedWindowPlacement2, 0, sizeof(WINDOWPLACEMENT));
    m_bShowInfo = false;
    m_bPanTimerActive = false;
    m_nBookModePageHeight = sp.BookModePageHeight();
    m_bAllowSmoothPanning = sp.AllowSmoothPanning();
    m_offsets_custom = CPoint(0, 0);
    m_customAnimationFrameTime = 1000;
    
    m_eSortModeBeforeRandom = Helpers::FS_FileName;
}

CMainDlg::~CMainDlg() {
    delete m_pDirectoryWatcher;
    delete m_pFileList;
    if (m_pJPEGProvider != NULL) delete m_pJPEGProvider;
    delete m_pImageProcParams;
    delete m_pImageProcParamsKept;
    delete m_pKeyMap;

    for (auto it = s_iconCache.begin(); it != s_iconCache.end(); ++it) {
        if (it->hBig != nullptr) {
            ::DestroyIcon(it->hBig);
        }
        if (it->hSmall != nullptr) {
            ::DestroyIcon(it->hSmall);
        }
    }
    s_iconCache.clear();

// DWM api is used for vsync in smooth scrolling implementation
    if (m_hmodDwmapi) {
        FreeLibrary(m_hmodDwmapi);
    }
}

LRESULT CMainDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    //     UpdateWindowTitle();
    // There is no point doing a full UpdateWindowTitle() at this point since we don't know any file path yet, so just do SetWindowText() instead
    this->SetWindowText(_T("JPEGView"));

    // set the scaling of the screen (DPI) compared to 96 DPI (design value)
    CPaintDC dc(this->m_hWnd);
    Helpers::DPIScalingFactor = ::GetDeviceCaps(dc, LOGPIXELSX)/96.0f;

    ::SetClassLongPtr(m_hWnd, GCLP_HCURSOR, NULL);

    HRESULT hresult = !S_OK;
    m_hmodDwmapi = LoadLibrary(_T("Dwmapi"));
    if (m_hmodDwmapi) {
        MyDwmIsCompositionEnabledType DynDwmIsCompositionEnabled = (MyDwmIsCompositionEnabledType)GetProcAddress(m_hmodDwmapi, "DwmIsCompositionEnabled");
        if (DynDwmIsCompositionEnabled) {
            hresult = DynDwmIsCompositionEnabled(&m_bDWMenabled);
            if (hresult != S_OK)
                ::OutputDebugStringW(_T("[JpegView] Dwmapi library found, but DynDwmIsCompositionEnabled call failed"));
        }

        if (m_bDWMenabled == TRUE) {
            m_DynDwmFlush = (MyDwmFlushType)GetProcAddress(m_hmodDwmapi, "DwmFlush");
            if (!m_DynDwmFlush)
                ::OutputDebugStringW(_T("[JpegView] Dwmapi library found, but DynDwmFlush isn't available"));
        }
    } else {
        ::OutputDebugStringW(_T("[JpegView] Dwmapi library not found"));
    }

    m_pDirectoryWatcher = new CDirectoryWatcher(m_hWnd);

    // determine the monitor rectangle and client rectangle
    CSettingsProvider& sp = CSettingsProvider::This();
    m_nMonitor = sp.DisplayMonitor();
    m_monitorRect = CMultiMonitorSupport::GetMonitorRect(m_nMonitor);
    // move to correct monitor - but no redraw as Windows does not like that this early
    ::SetWindowPos(m_hWnd, HWND_TOP, m_monitorRect.left, m_monitorRect.top, m_monitorRect.Width(), m_monitorRect.Height(),
        SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE | SWP_NOREDRAW);



    int nFrameIndex = 0;    // Will stay zero if no bookmarks are found.

    // [GF] Same code as in OpenFile(). Todo: combine into function?
    if (!m_sStartupFile.isEmpty()) {
        if (IsBookModeFile(m_sStartupFile)) {
            m_eAutoZoomModeFullscreen = Helpers::ZM_BookMode;
            m_eAutoZoomModeWindowed = Helpers::ZM_BookMode;
            if (sp.BookModeLaunchFullscreen()) {
                m_bFullScreenMode = true;
            }

            QString sBookmark = LoadBookmark(m_sStartupFile);
            if (!sBookmark.isEmpty()) {
                bool isNumeric = false;
                // Wandelt den String in ein int um und prüft gleichzeitig, ob er ausschließlich eine Zahl ist
                const int frameNum = sBookmark.toInt(&isNumeric);

                if (isNumeric) {
                    // Bookmark is a pure number, so wasn't derived from a file name but from a container's FrameIndex()
                    nFrameIndex = frameNum - 1;
                } else {
                    // Bookmark point to an image file. Check if exits and ask the user if the launch that instead.
                    const QString parentDir = QFileInfo(m_sStartupFile).path();
                    const QString sBookmarkPath = QDir(parentDir).filePath(sBookmark);

                    if (QFileInfo::exists(sBookmarkPath)) {
                        // m_sStartupFile = sBookmarkPath;
                    }
                }
            }
        }
    }

    m_clientRect = m_bFullScreenMode ? m_monitorRect : CMultiMonitorSupport::GetDefaultClientRectInWindowMode(sp.AutoFullScreen());

// Note: With AutoFullScreen, at this point m_clientRect will be monitorRect even when m_bFullScreenMode==0

    // set icons (for toolbar)
    HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), 
        IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED);
    SetIcon(hIcon, TRUE);
    HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), 
        IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
    SetIcon(hIconSmall, FALSE);

    // turn on/off mouse coursor
    m_bMouseOn = !m_bFullScreenMode;
    ::ShowCursor(m_bMouseOn);

    // intitialize list of files to show with startup file (and folder)
    m_pFileList = new CFileList(m_sStartupFile,
                                *m_pDirectoryWatcher,
                                (m_eForcedSorting == Helpers::FS_Undefined) ? sp.Sorting() : m_eForcedSorting,
                                sp.IsSortedAscending(),
                                sp.WrapAroundFolder(),
                                0,
                                m_eForcedSorting != Helpers::FS_Undefined);

    m_pFileList->SetNavigationMode(sp.Navigation());

    // create thread pool for processing requests on multiple CPU cores
    CProcessingThreadPool::This().CreateThreadPoolThreads();


    int nReadAheadBuffers = CSettingsProvider::This().CacheRange()*2 + 1;    // The +1 is the current image
    
    // create JPEG provider and request first image - do no processing yet if not in fullscreen mode (as we do not know the size yet)
    m_pJPEGProvider = new CJPEGProvider(m_hWnd, NUM_THREADS, nReadAheadBuffers);

    m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList,
                                                    CJPEGProvider::FORWARD,
                                                    m_pFileList->Current(),
                                                    nFrameIndex,
                                                    CreateProcessParams(!m_bFullScreenMode),
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        StartAnimation();
    }

    m_nLastLoadError = GetLoadErrorAfterOpenFile();
    CheckIfApplyAutoFitWndToImage(true);
    AfterNewImageLoaded(true, true); // synchronize to per image parameters

    if (!m_bFullScreenMode) {
        // Window mode, set correct window size
        SetCurrentWindowStyle();
        if (!IsAdjustWindowToImage()) {
            CRect windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
            this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
        } else {
            AdjustWindowToImage(true);
        }
        if (sp.DefaultMaximized()) {
            this->ShowWindow(SW_MAXIMIZE);
        }
    } else {
        PrefetchDIB(m_monitorRect);
        SetWindowLong(GWL_STYLE, WS_VISIBLE);
        SetWindowPos(HWND_TOP, &m_monitorRect, SWP_NOZORDER);
        SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
    }

    if (CSettingsProvider::This().WindowAlwaysOnTopOnStartup()) {
        // if set by default for startup ... it is false by default, toggle = true
        ToggleAlwaysOnTop();
    }

    m_bLockPaint = false;

    if (m_nAutoStartSlideShow > 0) {
        StartMovieMode(1.0 / m_nAutoStartSlideShow);
    }

    StartLowQTimer(ZOOM_TIMEOUT);    // Start out in low quality scaling to get something on the screen asap. Feels more responsive with huge images.
    
    this->Invalidate(FALSE);

    this->DragAcceptFiles();

    return TRUE;
}

LRESULT CMainDlg::OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    static bool s_bFirst = true;

    if (m_bLockPaint) {
        return 0;
    }

    if (m_DynDwmFlush) {
        m_DynDwmFlush();    // Waits for vsync
    }

    CPaintDC dc(m_hWnd);
    m_dRealizedZoom = 1.0;

    this->GetClientRect(&m_clientRect);

    CBrush backBrush;
    backBrush.CreateSolidBrush(CSettingsProvider::This().ColorBackground());

    if (m_pCurrentImage == NULL) {
        dc.FillRect(&m_clientRect, backBrush);
    } else {
        // do this as very first - may changes size of image
        m_pCurrentImage->VerifyRotation(CRotationParams(m_pCurrentImage->GetRotationParams(), m_nRotation));

        // Find out zoom multiplier if not yet known
        if (m_dZoomMult < 0.0) {
            m_dZoomMult = GetZoomMultiplier(m_pCurrentImage, m_clientRect);
        }

        // find out the new vitual image size and the size of the bitmap to request
        CSize newSize = GetVirtualImageSize(m_pCurrentImage->OrigSize(),
            m_clientRect.Size(),
            IsAdjustWindowToImage() ? Helpers::ZM_FitToScreenNoZoom : (m_isUserFitToScreen ? m_autoZoomFitToScreen : GetAutoZoomMode()),
            m_dZoom);

        m_virtualImageSize = newSize;
        m_dRealizedZoom = (double)newSize.cx / m_pCurrentImage->OrigSize().cx;
        CPoint unlimitedOffsets = m_offsets;
        m_offsets = LimitOffsets(m_offsets, m_clientRect.Size(), newSize);
        m_DIBOffsets = m_bZoomMode ? static_cast<CPoint>(unlimitedOffsets - m_offsets) : CPoint(0, 0);

        // Clip to client rectangle and request the DIB
        CSize clippedSize(std::min<long>(m_clientRect.Width(), newSize.cx), std::min<long>(m_clientRect.Height(), newSize.cy));
        CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, m_offsets);

        void* pDIBData;
        pDIBData = m_pCurrentImage->GetDIB(newSize,
                                           clippedSize,
                                           offsetsInImage,
                                           *m_pImageProcParams,
                                           CreateProcessingFlags(m_bHQResampling && !m_bTemporaryLowQ && !m_bZoomMode, m_bAutoContrast, false));

        // Paint the DIB
        if (pDIBData != NULL) {
            BITMAPINFO bmInfo{ 0 };
            DrawDIB32bppWithBlackBorders(dc, bmInfo, pDIBData, backBrush, m_clientRect, clippedSize, m_DIBOffsets);
        }

        if (m_bZoomMode) m_offsets = unlimitedOffsets;

        if (m_bShowInfo) {
            CSettingsProvider& sp = CSettingsProvider::This();
            int iNumThreads = sp.NumberOfCoresToUse();
            Helpers::CPUType cpu = sp.AlgorithmImplementation();
            EFilterType filter = sp.DownsamplingFilter();
            TCHAR sCPU[64];
            TCHAR sFilter[64];
    
            if (cpu == Helpers::CPU_Unknown)
                swprintf(sCPU,64,TEXT("%s"), TEXT("Unknown"));
            else if (cpu == Helpers::CPU_Generic)
                swprintf(sCPU,64,TEXT("%s"), TEXT("Generic"));
            else if (cpu == Helpers::CPU_MMX)
                swprintf(sCPU,64,TEXT("%s"), TEXT("MMX"));
            else if (cpu == Helpers::CPU_SSE)
                swprintf(sCPU,64,TEXT("%s"), TEXT("SSE2 (128 bit)"));
            else if (cpu == Helpers::CPU_AVX2)
                swprintf(sCPU,64,TEXT("%s"), TEXT("AVX2 (256 bit)"));
    
            if (filter == Filter_Downsampling_None)
                swprintf(sFilter,64,TEXT("%s"), TEXT("None"));
            else if (filter == Filter_Downsampling_Hermite)
                swprintf(sFilter,64,TEXT("%s"), TEXT("Hermite"));
            else if (filter == Filter_Downsampling_Catrom)
                swprintf(sFilter,64,TEXT("%s"), TEXT("Catrom"));
            else if (filter == Filter_Downsampling_Mitchell)
                swprintf(sFilter,64,TEXT("%s"), TEXT("Mitchell"));
            else if (filter == Filter_Downsampling_Lanczos2)
                swprintf(sFilter,64,TEXT("%s"), TEXT("Lanczos2"));

            unsigned int iRealWidth = static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigWidth()));
            unsigned int iRealHeight = static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigHeight()));

            QString infoText;
            QString sFullPath = CurrentFileName(false);

            if (!sFullPath.isEmpty()) {
                QFileInfo fileInfo(sFullPath);
				QString sFileSize = FormatFileSize(fileInfo.size());
					
                if (m_pCurrentImage->NumberOfFrames() > 1) {
                    if (m_bIsAnimationPlaying) {
                        infoText = QString(
                                       "%1\n"
                                       "Frame %2 of %3 (%4 ms)\n"
                                       "File Size:   %5\n"
                                       "Image Size:  %6 x %7\n"
                                       "Zoomed Size: %8 x %9   (Zoom Factor: %10)\n"
                                       "Window Size: %11 x %12\n\n"
                                       "CPU-Threads: %13\n"
                                       "CPU-Algorithm: %14\n"
                                       "Downsampling-Filter: %15\n\n"
                                       "Loading Image:  %16 ms\n"
                                       "Scaling Image:  %17 ms"
                                       )
                                       .arg(QDir::toNativeSeparators(sFullPath))
                                       .arg(m_pCurrentImage->FrameIndex() + 1)
                                       .arg(m_pCurrentImage->NumberOfFrames())
                                       .arg(m_pCurrentImage->FrameTimeMs())
                                       .arg(sFileSize)
                                       .arg(m_pCurrentImage->OrigWidth())
                                       .arg(m_pCurrentImage->OrigHeight())
                                       .arg(iRealWidth)
                                       .arg(iRealHeight)
                                       .arg(m_dZoom)
                                       .arg(m_clientRect.Width())
                                       .arg(m_clientRect.Height())
                                       .arg(iNumThreads)
                                       .arg(sCPU)
                                       .arg(sFilter)
                                       .arg(m_pCurrentImage->GetFileLoadDelayMs())
                                       .arg(m_pCurrentImage->GetDIBLoadDelayMs());
                    } else {
                        infoText = QString(
                                       "%1\n"
                                       "Frame %2 of %3\n"
                                       "File Size:   %4\n"
                                       "Image Size:  %5 x %6\n"
                                       "Zoomed Size: %7 x %8   (Zoom Factor: %9)\n"
                                       "Window Size: %10 x %11\n\n"
                                       "CPU-Threads: %12\n"
                                       "CPU-Algorithm: %13\n"
                                       "Downsampling-Filter: %14\n\n"
                                       "Loading Image:  %15 ms\n"
                                       "Scaling Image:  %16 ms"
                                       )
                                       .arg(QDir::toNativeSeparators(sFullPath))
                                       .arg(m_pCurrentImage->FrameIndex() + 1)
                                       .arg(m_pCurrentImage->NumberOfFrames())
                                       .arg(sFileSize)
                                       .arg(m_pCurrentImage->OrigWidth())
                                       .arg(m_pCurrentImage->OrigHeight())
                                       .arg(iRealWidth)
                                       .arg(iRealHeight)
                                       .arg(m_dZoom)
                                       .arg(m_clientRect.Width())
                                       .arg(m_clientRect.Height())
                                       .arg(iNumThreads)
                                       .arg(sCPU)
                                       .arg(sFilter)
                                       .arg(m_pCurrentImage->GetFileLoadDelayMs())
                                       .arg(m_pCurrentImage->GetDIBLoadDelayMs());
                    }
                } else {
                    infoText = QString(
                                   "%1\n"
                                   "File Size:   %2\n"
                                   "Image Size:  %3 x %4\n"
                                   "Zoomed Size: %5 x %6   (Zoom Factor: %7)\n"
                                   "Window Size: %8 x %9\n\n"
                                   "CPU-Threads: %10\n"
                                   "CPU-Algorithm: %11\n"
                                   "Downsampling-Filter: %12\n\n"
                                   "Loading Image:  %13 ms\n"
                                   "Scaling Image:  %14 ms"
                                   )
                                   .arg(QDir::toNativeSeparators(sFullPath))
                                   .arg(sFileSize)
                                   .arg(m_pCurrentImage->OrigWidth())
                                   .arg(m_pCurrentImage->OrigHeight())
                                   .arg(iRealWidth)
                                   .arg(iRealHeight)
                                   .arg(m_dZoom)
                                   .arg(m_clientRect.Width())
                                   .arg(m_clientRect.Height())
                                   .arg(iNumThreads)
                                   .arg(sCPU)
                                   .arg(sFilter)
                                   .arg(m_pCurrentImage->GetFileLoadDelayMs())
                                   .arg(m_pCurrentImage->GetDIBLoadDelayMs());
                }
            } else {
                infoText = QString(
                               "\n\n"
                               "Image Size:  %1 x %2\n"
                               "Zoomed Size: %3 x %4   (Zoom Factor: %5)\n"
                               "Window Size: %6 x %7\n\n"
                               "CPU-Threads: %8\n"
                               "CPU-Algorithm: %9\n"
                               "Downsampling-Filter: %10\n\n"
                               "Loading Image:  %11 ms\n"
                               "Scaling Image:  %12 ms"
                               )
                               .arg(m_pCurrentImage->OrigWidth())
                               .arg(m_pCurrentImage->OrigHeight())
                               .arg(iRealWidth)
                               .arg(iRealHeight)
                               .arg(m_dZoom)
                               .arg(m_clientRect.Width())
                               .arg(m_clientRect.Height())
                               .arg(iNumThreads)
                               .arg(sCPU)
                               .arg(sFilter)
                               .arg(m_pCurrentImage->GetFileLoadDelayMs())
                               .arg(m_pCurrentImage->GetDIBLoadDelayMs());
            }

            HFONT hFont = CreateFont(25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, TEXT("CONSOLAS"));

            dc.SelectFont(hFont);
            dc.SetBkColor(RGB(0, 0, 0));
            dc.SetTextColor(RGB(255, 255, 255));
            dc.SetBkMode(OPAQUE); // OPAQUE / TRANSPARENT

            ::DrawTextW(dc, reinterpret_cast<LPCWSTR>(infoText.utf16()), infoText.length(), &m_clientRect, DT_LEFT | DT_EXTERNALLEADING | DT_WORDBREAK | DT_NOPREFIX);

            // Variant with outlined text
            //dc.SetBkMode(TRANSPARENT);    // OPAQUE / TRANSPARENT
            //DrawTextBordered(dc, infoText, &m_clientRect, DT_LEFT | DT_EXTERNALLEADING | DT_WORDBREAK | DT_NOPREFIX);
            
            DeleteObject(hFont);
        }
    }

    // Display errors and warnings
    DisplayErrors(m_pCurrentImage, m_clientRect, dc);

    // Show current zoom factor
    if (m_bInZooming || m_bShowZoomFactor) {
        dc.SetTextColor(CSettingsProvider::This().ColorGUI());
        SelectDefaultZoomFactorFont(dc);
        DrawTextBordered(dc, formatOutputZoomFactor(m_dZoom).toStdWString().c_str(), GetZoomTextRect(), DT_RIGHT);
    }

    if (!m_strToast.isEmpty())
    {
        dc.SetTextColor(CSettingsProvider::This().ColorGUI());
        SelectDefaultToastFont(dc);
        DrawTextBordered(dc, reinterpret_cast<LPCTSTR>(m_strToast.utf16()), GetToastTextRect(), DT_RIGHT);
    }

    SetCursorForMoveSection();

    return 0;
}

void CMainDlg::PaintToDC(CDC& dc) {
    COLORREF backColor = CSettingsProvider::This().ColorBackground();
    if (backColor == 0)
        backColor = RGB(0, 0, 1); // these f**ing nVidia drivers have a bug when blending pure black
    CBrush backBrush;
    backBrush.CreateSolidBrush(backColor);
    m_dRealizedZoom = 1.0;

    CJPEGImage* pCurrentImage = GetCurrentImage();
    if (pCurrentImage == NULL) {
        dc.FillRect(&m_clientRect, backBrush);
    } else {
        // do this as very first - may changes size of image
        pCurrentImage->VerifyRotation(CRotationParams(pCurrentImage->GetRotationParams(), GetRotation()));

        // find out the new virtual image size and the size of the bitmap to request
        double dZoom = m_dZoom;
        CSize newSize = GetVirtualImageSize(pCurrentImage->OrigSize(), m_clientRect.Size(), GetAutoZoomMode(), dZoom);
        CPoint offsets = LimitOffsets(GetOffsets(), m_clientRect.Size(), newSize);
        m_dRealizedZoom = (double)newSize.cx / m_pCurrentImage->OrigSize().cx;

        // Clip to client rectangle and request the DIB
        CSize clippedSize(std::min<long>(m_clientRect.Width(), newSize.cx), std::min<long>(m_clientRect.Height(), newSize.cy));
        CPoint offsetsInImage = pCurrentImage->ConvertOffset(newSize, clippedSize, offsets);

        void* pDIBData = pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage, 
                *GetImageProcessingParams(),
                CreateDefaultProcessingFlags());
        if (pDIBData != NULL) {
            BITMAPINFO bmInfo{ 0 };
            DrawDIB32bppWithBlackBorders(dc, bmInfo, pDIBData, backBrush, m_clientRect, clippedSize, CPoint(0, 0));
        }

        DisplayErrors(pCurrentImage, m_clientRect, dc);
    }
}

void CMainDlg::DisplayErrors(CJPEGImage* pCurrentImage, const CRect& clientRect, CDC& dc) {
    int oldBkMode = dc.GetBkMode();
    if (oldBkMode != TRANSPARENT) {
        dc.SetBkMode(TRANSPARENT);
    }
        
    dc.SetTextColor(CSettingsProvider::This().ColorGUI());
    if (m_sStartupFile.isEmpty() && m_pCurrentImage == NULL) {
        CRect rectText(0, clientRect.Height()/2 - Helpers::ScaleWithDPI(40), clientRect.Width(), clientRect.Height());
        QString message = QStringLiteral("No image loaded!\n\nPress ESC to exit...");
        dc.DrawText(reinterpret_cast<LPCWSTR>(message.utf16()), -1, &rectText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    } else if (pCurrentImage == NULL) {
        QString sFileName;
        if (m_nLastLoadError == FileLoad_SlideShowListInvalid) {
            sFileName = m_sStartupFile;
        } else if (m_nLastLoadError == FileLoad_NoFilesInDirectory) {
            sFileName = m_pFileList->CurrentDirectory();
        } else {
            sFileName = CurrentFileName(false);
        }

        DrawImageLoadErrorText(dc, clientRect, sFileName, m_nLastLoadError,
            (m_bOutOfMemoryLastImage ? FileLoad_OutOfMemory : 0) | (m_bExceptionErrorLastImage ? FileLoad_ExceptionError : 0));
    }

    if (oldBkMode != TRANSPARENT) {
        dc.SetBkMode(oldBkMode);
    }
}

LRESULT CMainDlg::OnSize(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    if (wParam == 1) {    // minimize -> should be ignored
        return 0;
    }

    Helpers::EAutoZoomMode eAutoZoomMode = GetAutoZoomMode();
    bool bFillCrop = (eAutoZoomMode == Helpers::ZM_FillScreenNoZoom || eAutoZoomMode == Helpers::ZM_FillScreen);
    bool bAllowZoomIn = (eAutoZoomMode == Helpers::ZM_FitToScreen || eAutoZoomMode == Helpers::ZM_FillScreen);
    bool bAdjustWindowSize = false;
    
    float dZoomForFit = GetZoomFactorForFitToScreen(bFillCrop, bAllowZoomIn);
    bool bKeepFitToScreen = !m_bResizeForNewImage && (MaxRatio(m_dZoom, dZoomForFit) < 1.01);
    this->GetClientRect(&m_clientRect);
    this->Invalidate(FALSE);

    m_nMouseX = m_nMouseY = -1;

    // keep fit to screen
    if (bKeepFitToScreen) {
        dZoomForFit = GetZoomFactorForFitToScreen(bFillCrop, bAllowZoomIn);    // Calling again, since now m_clientRect is up to date.
        if (MaxRatio(m_dZoom, dZoomForFit) >= 1.00001) {
            ResetZoomToFitScreen(bFillCrop, bAllowZoomIn, bAdjustWindowSize);
        }
    }
    return 0;
}

LRESULT CMainDlg::OnGetMinMaxInfo(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    if (m_pJPEGProvider != NULL) {
        MINMAXINFO* pMinMaxInfo = (MINMAXINFO*) lParam;
        CSize minimalSize = CSettingsProvider::This().MinimalWindowSize();
        pMinMaxInfo->ptMinTrackSize = CPoint(std::max<long>(0, minimalSize.cx), std::max<long>(0, minimalSize.cy));
        return 1;
    } else {
        return 0;
    }
}

// Note: Reacts to WM_COPYDATA message.
// If the message contains a file path, we can either open it and send back KEY_MAGIC, or ignore it and return NULL.
// The hidden assumption being that if the WM_COPYDATA came from newly launched instance, that instance will,
// upon receiving KEY_MAGIC, dutifully close itself, since we answered it "I got this" by returning KEY_MAGIC.
// If we return NULL, the new instance is free to do as it pleases and show the image itself or whatever.
#include <QString>
#include <QFileInfo>
#include <QtGlobal>

LRESULT CMainDlg::OnAnotherInstanceStarted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE;
    auto* pData = reinterpret_cast<COPYDATASTRUCT*>(lParam);

    if (pData != nullptr) {
        if (pData->dwData == KEY_MAGIC && pData->cbData > 0) {
            bool bSameImageFolder = false;

            // Puffer aus COPYDATASTRUCT in QString umwandeln (Länge in Anzahl wchar_t/TCHAR)
            const auto* pWCharData = static_cast<const wchar_t*>(pData->lpData);
            const int charCount = static_cast<int>((pData->cbData / sizeof(wchar_t)) - 1);
            const QString sCopyDataFile = QString::fromWCharArray(pWCharData, charCount);

            const QString sCurrentFile = CurrentFileName(false);

            if (!sCurrentFile.isEmpty()) {
                if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                    if (sCopyDataFile.compare(sCurrentFile, Qt::CaseInsensitive) == 0) {
                        // Gleiches Comic-Archiv. Nichts tun, aber der neuen Instanz mitteilen,
                        // dass wir es verarbeiten (verhindert doppeltes Öffnen desselben Buchs).
                        bHandled = TRUE;
                        this->ShowWindow(SW_RESTORE);
                        SetForegroundWindow(m_hWnd);
                        return KEY_MAGIC;
                    } else {
                        // Anderes Comic-Archiv: Nicht verarbeiten, damit die andere Instanz es öffnet.
                        return 0;
                    }
                } else {
                    // Verzeichnisse mit QFileInfo vergleichen (vollständig ohne PathRemoveFileSpec / TCHAR)
                    const QString copyDataPath = QFileInfo(sCopyDataFile).path();
                    const QString currentPath  = QFileInfo(sCurrentFile).path();

                    bSameImageFolder = (copyDataPath.compare(currentPath, Qt::CaseInsensitive) == 0);
                }
            }

            const bool bNoCurrentFile = sCurrentFile.isEmpty();
            const bool bSingleFullScreen = m_bFullScreenMode && CSettingsProvider::This().SingleFullScreenInstance();
            const bool bSingleInstanceAlways = (CSettingsProvider::This().SingleInstanceMode() == Helpers::SI_Always);
            const bool bSingleInstancePerFolder = (CSettingsProvider::This().SingleInstanceMode() == Helpers::SI_PerFolder) && bSameImageFolder;

            if (bNoCurrentFile || bSingleFullScreen || bSingleInstanceAlways || bSingleInstancePerFolder) {
                m_sStartupFile = sCopyDataFile;

                ::PostMessage(m_hWnd, WM_LOAD_FILE_ASYNCH, 0, KEY_MAGIC);
                bHandled = TRUE;
                this->ShowWindow(SW_RESTORE);
                SetForegroundWindow(m_hWnd);
                return KEY_MAGIC;
            }
        }
    }
    return 0;
}

LRESULT CMainDlg::OnLoadFileAsynch(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
    bHandled = lParam == KEY_MAGIC;
    if (lParam == KEY_MAGIC && ::IsWindowEnabled(m_hWnd)) {
        StopMovieMode();
        StopAnimation();
        MouseOn();
        OpenFile(m_sStartupFile, false);
    }
    return 0;
}

LRESULT CMainDlg::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
    GetWindowRect(m_windowRectOnClose);
    bHandled = FALSE;
    return 0;
}

LRESULT CMainDlg::OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    this->SetCapture();
    CPoint pointClicked(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

    bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

    bool bDraggingRequired = m_virtualImageSize.cx > m_clientRect.Width() || m_virtualImageSize.cy > m_clientRect.Height();

    if (!m_bZoomModeOnLeftMouse && HandleMouseButtonByKeymap(VK_LBUTTON)) {
        return 0;
    }
    bool bZoomMode = (bShift || m_bZoomModeOnLeftMouse) && !bCtrl;
    if (bZoomMode) {
        m_bZoomMode = true;
        m_dStartZoom = m_dZoom;
        m_nCapturedX = m_nMouseX; m_nCapturedY = m_nMouseY;
    } else if (bDraggingRequired) {
        StartDragging(pointClicked.x, pointClicked.y, false);
    } 

    SetCursorForMoveSection();

    return 0;
}

LRESULT CMainDlg::OnLButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    if (m_bZoomMode) {
        m_bZoomMode = false;
        AdjustWindowToImage(false);
        Invalidate(FALSE);
    } else if (m_bDragging) {
        EndDragging();
    }
    ::ReleaseCapture();
    return 0;
}


// based on https://www.codeproject.com/Articles/18400/How-to-move-a-dialog-which-does-not-have-a-caption
// https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-nchittest
LRESULT CMainDlg::OnNCHitTest(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bool bAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    bool bLButton = ::GetAsyncKeyState(MK_LBUTTON) < 0;

    // only move when alt is held down, double click causes this to expand as well
    if (!m_bFullScreenMode && bAlt && ::DefWindowProc(m_hWnd, uMsg, wParam, lParam) == HTCLIENT && bLButton) {
        // don't allow intercepting if we're in full screen mode
        // (which is really just the window repositioned so the titlebar falls off the screen)
        return HTCAPTION;
    }

    bHandled = FALSE;  // if not moving window, considered unhandled, or else all the mouse button code stops working
    return 0;
}

LRESULT CMainDlg::OnNCLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
    m_dZoomAtResizeStart = m_dZoom;
    bHandled = FALSE; // do not handle message, this would block correct processing by OS
    return 0;
}

LRESULT CMainDlg::OnRButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
    bHandled = HandleMouseButtonByKeymap(VK_RBUTTON);
    return 0;
}

LRESULT CMainDlg::OnRButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
    bHandled = HandleMouseButtonByKeymap(VK_RBUTTON, false);
    return 0;
}

LRESULT CMainDlg::OnMButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    this->SetCapture();
    if (HandleMouseButtonByKeymap(VK_MBUTTON)) {
        return 0;
    }
    StartDragging(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false);
    return 0;
}

LRESULT CMainDlg::OnMButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    EndDragging();
    ::ReleaseCapture();
    return 0;
}

LRESULT CMainDlg::OnLButtonDblClk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    if (m_pCurrentImage != NULL) {
        if (HandleMouseButtonByKeymap(VK_LBUTTONDBLCLK)) {
            return 0;
        }
        double dZoom = -1.0;
        CSize sizeAutoZoom = GetVirtualImageSize(m_pCurrentImage->OrigSize(),
            m_clientRect.Size(), IsAdjustWindowToImage() ? Helpers::ZM_FitToScreenNoZoom : GetAutoZoomMode(), dZoom);
        if (sizeAutoZoom != m_virtualImageSize) {
            ExecuteCommand(GetAutoZoomMode() * 10 + IDM_AUTO_ZOOM_NONE);
        } else {
            ResetZoomTo100Percents(true);
        }
    }
    return 0;
}

LRESULT CMainDlg::OnXButtonDown(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {

    if (HandleMouseButtonByKeymap((GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2)) {
        return 0;
    }
    bool bExchangeXButtons = CSettingsProvider::This().ExchangeXButtons();
    if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) {
        GotoImage(bExchangeXButtons ? POS_Next : POS_Previous);
    } else {
        GotoImage(bExchangeXButtons ? POS_Previous : POS_Next);
    }

    return 0;
}

LRESULT CMainDlg::OnMouseMove(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    // Turn mouse pointer on when mouse has moved some distance
    m_nMouseX = GET_X_LPARAM(lParam);
    m_nMouseY = GET_Y_LPARAM(lParam);
    if (!m_bDragging) {
        if (m_startMouse.x == -1 && m_startMouse.y == -1) {
            m_startMouse.x = m_nMouseX;
            m_startMouse.y = m_nMouseY;
        } else {
            if (abs(m_nMouseX - m_startMouse.x) + abs(m_nMouseY - m_startMouse.y) > 50) {
                MouseOn();
            }
        }
    }

    // Do dragging when needed, else pass event to panel manager and zoom navigator
    bool bMouseCursorSet = false;
    if (m_bZoomMode) {
        int nDX = m_nMouseX - m_nCapturedX;
        double dFactor = 1 + pow(nDX * nDX, 0.8) / 1500;
        if (nDX < 0) dFactor = 1.0/dFactor;
        PerformZoom(m_dStartZoom * dFactor, false, true, false);
    } else if (m_bDragging) {
        DoDragging();
    }
    if (!m_bPanMouseCursorSet && !bMouseCursorSet) {
        ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
    }

    return 0;
}

LRESULT CMainDlg::OnMouseWheel(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int nDelta = GET_WHEEL_DELTA_WPARAM(wParam);

    if (CSettingsProvider::This().NavigateWithMouseWheel()) {
        if (!bCtrl && !bShift) {
            // Smart handling:
            // Pan vertically (if image is higher than window)
            // OR go to previous/next image (if image is smaller).
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                        if (PerformPan(0, -PAN_STEP, false) == true) {
                            this->Invalidate(FALSE);
                        }
                    } else {
                        unsigned int iRealHeight =
                            static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigHeight()));

                        if (iRealHeight > m_clientRect.Height()) {
                            if (PerformPan(0, -PAN_STEP, false) == true) {
                                this->Invalidate(FALSE);
                            }
                        } else {
                            GotoImage(POS_Next);
                        }
                    }
                }
                else if (nDelta > 0) {
                    if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                        if (PerformPan(0, PAN_STEP, false) == true) {
                            this->Invalidate(FALSE);
                        }
                    } else {
                        uint iRealHeight =
                            static_cast<uint>(m_dZoom * (m_pCurrentImage->OrigHeight()));

                        if (iRealHeight > m_clientRect.Height()) {
                            if (PerformPan(0, PAN_STEP, false) == true) {
                                this->Invalidate(FALSE);
                            }
                        } else {
                            GotoImage(POS_Previous);
                        }
                    }
                }
            } else {
                // No current image, e.g. after a corrupt/0-byte image failed to load.
                // Still allow mouse-wheel navigation to continue.
                if (nDelta < 0) {
                    GotoImage(POS_Next);
                } else if (nDelta > 0) {
                    GotoImage(POS_Previous);
                }
            }
        } else if (bCtrl && bShift) { // Zoom
            if (m_dZoom > 0) {
                PerformZoom(
                    CSettingsProvider::This().MouseWheelZoomSpeed() *
                        double(nDelta) / WHEEL_DELTA,
                    true,
                    m_bMouseOn,
                    true
                );
            }
        } else if (bShift) { // Pan horizontally
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (PerformPan(-PAN_STEP, 0, false) == true) {
                        this->Invalidate(FALSE);
                    }
                } else if (nDelta > 0) {
                    if (PerformPan(PAN_STEP, 0, false) == true) {
                        this->Invalidate(FALSE);
                    }
                }
            }
        } else if (bCtrl) { // Pan vertically
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (PerformPan(0, -PAN_STEP, false) == true) {
                        this->Invalidate(FALSE);
                    }
                } else if (nDelta > 0) {
                    if (PerformPan(0, PAN_STEP, false) == true) {
                        this->Invalidate(FALSE);
                    }
                }
            }
        }
    } else if (m_dZoom > 0) {
        PerformZoom(
            CSettingsProvider::This().MouseWheelZoomSpeed() * double(nDelta) / WHEEL_DELTA,
            true,
            m_bMouseOn,
            true
        );
    }

    return 0;
}

LRESULT CMainDlg::OnKeyDown(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
    bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool bAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

    //bool bHandled = false;
    if (!bCtrl && wParam != VK_ESCAPE && m_nLastLoadError == FileLoad_NoFilesInDirectory && !m_sStartupFile.isEmpty()) {
        // search in subfolders if initial directory has no images
        //bHandled = true;
        m_pFileList->SetNavigationMode(Helpers::NM_LoopSubDirectories);
        GotoImage(POS_Next);
    } else if (wParam == VK_F1) {
        //bHandled = true;
        ExecuteCommand(IDM_HELP);
    } else {
        int nCommand = m_pKeyMap->GetCommandIdForKey((int)wParam, bAlt, bCtrl, bShift);
        if (nCommand > 0) {
            if (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN || nCommand == IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT) {
                if ((nCommand == IDM_PAN_UP && wParam != VK_UP) || (nCommand == IDM_PAN_DOWN &&  wParam != VK_DOWN) || (nCommand == IDM_PAN_LEFT &&  wParam != VK_LEFT) || (nCommand == IDM_PAN_RIGHT &&  wParam != VK_RIGHT)) {
                    // Allow smooth panning only for these particular keys. The scrolling timer will wait for their release to know when to stop scrolling.
                    m_bAllowSmoothPanning = false;
                }
                if (m_bAllowSmoothPanning && (lParam & 0xc0000000)) {
                    // Ignore key repeat events (lParam & 0xc0000000) for the panning keys.
                    return 1;
                }
            }

            //bHandled = true;
            ExecuteCommand(nCommand);
        }
    }

    return 1;
}

LRESULT CMainDlg::OnSysKeyDown(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (wParam == VK_F4) {
        CleanupAndTerminate();
    } else {
        int nCommand = m_pKeyMap->GetCommandIdForKey((int)wParam, true, false, bShift);
        if (nCommand > 0) {
            ExecuteCommand(nCommand);
        }
    }
    return 1;
}


LRESULT CMainDlg::OnGetDlgCode(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    // need to request key messages, else the dialog proc eats them all up
    return DLGC_WANTALLKEYS;
}

LRESULT CMainDlg::OnImageLoadCompleted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
    // route to JPEG provider
    m_pJPEGProvider->OnImageLoadCompleted((int)lParam);
    return 0;
}

LRESULT CMainDlg::OnDisplayedFileChangedOnDisk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged() && m_pCurrentImage != NULL &&
        m_pFileList != NULL && m_pFileList->CanOpenCurrentFileForReading()) {
        ExecuteCommand(IDM_RELOAD);
    }
    return 0;
}

LRESULT CMainDlg::OnActiveDirectoryFilelistChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged()) {
        if (m_pFileList != NULL && m_pFileList->CurrentFileExists()) {
            m_pFileList->Reload(NULL, false);
            Invalidate(FALSE);
        }

    if (m_pCurrentImage != NULL)
        GotoImage(POS_Current);
    }
    return 0;
}

LRESULT CMainDlg::OnDropFiles(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    HDROP hDrop = (HDROP) wParam;
    if (hDrop != NULL) {
        const int BUFF_SIZE = 512;
        TCHAR buff[BUFF_SIZE];
        if (::DragQueryFile(hDrop, 0, (LPTSTR) &buff, BUFF_SIZE - 1) > 0) {
            if (::GetFileAttributes(buff) & FILE_ATTRIBUTE_DIRECTORY) {
                _tcsncat_s(buff, BUFF_SIZE, _T("\\"), BUFF_SIZE);
            }
            OpenFile(QString::fromWCharArray(buff), false);
        }
        ::DragFinish(hDrop);
    }
    return 0;
}

LRESULT CMainDlg::OnTimer(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    if (wParam == ANIMATION_TIMER_EVENT_ID || (wParam == SLIDESHOW_TIMER_EVENT_ID && m_nCurrentTimeout > 0)) {
        // Remove all timer messages for slideshow and animation events that accumulated in the queue
        MSG msg;
        while (::PeekMessage(&msg, this->m_hWnd, WM_TIMER, WM_TIMER, PM_REMOVE)) {
            if (msg.wParam != SLIDESHOW_TIMER_EVENT_ID && msg.wParam != ANIMATION_TIMER_EVENT_ID) {
                BOOL bNotUsed;
                OnTimer(WM_TIMER, msg.wParam, msg.lParam, bNotUsed);
            }
            if (msg.wParam == SLIDESHOW_TIMER_EVENT_ID && wParam == ANIMATION_TIMER_EVENT_ID) {
                // if there are queued slideshow timer events and we process an animation event, the slideshow event has preceedence
                wParam = SLIDESHOW_TIMER_EVENT_ID;
            }
        }

        int nRealDisplayTimeMs = ::GetTickCount() - m_nLastSlideShowImageTickCount;
        if (m_nCurrentTimeout > 250 && wParam == SLIDESHOW_TIMER_EVENT_ID) {
            if (m_nCurrentTimeout - nRealDisplayTimeMs > 100) {
                // restart timer
                ::Sleep(m_nCurrentTimeout - nRealDisplayTimeMs);
                ::KillTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID);
                ::SetTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID, m_nCurrentTimeout, NULL);
            }
        }
        GotoImage((wParam == ANIMATION_TIMER_EVENT_ID) ? POS_NextFrame : POS_NextSlideShow, NO_REMOVE_KEY_MSG);
        if (wParam == SLIDESHOW_TIMER_EVENT_ID && UseSlideShowTransitionEffect()) {
            AnimateTransition();
        }
        if (wParam != ANIMATION_TIMER_EVENT_ID) {
            m_nLastSlideShowImageTickCount = ::GetTickCount();
        }
    } else if (wParam == ZOOM_TIMER_EVENT_ID) {
        ::KillTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID);
        if (m_bTemporaryLowQ || m_bInZooming) {
            ::SetTimer(this->m_hWnd, ZOOM_TEXT_TIMER_EVENT_ID, ZOOM_TEXT_TIMEOUT, NULL);
            if (m_bInZooming) m_bShowZoomFactor = true;
            m_bTemporaryLowQ = false;
            m_bInZooming = false;
            if (m_bHQResampling && m_pCurrentImage != NULL) {
                this->Invalidate(FALSE);
            }
        }
    } else if (wParam == ZOOM_TEXT_TIMER_EVENT_ID) {
        m_bShowZoomFactor = false;
        ::KillTimer(this->m_hWnd, ZOOM_TEXT_TIMER_EVENT_ID);
        this->InvalidateRect(GetZoomTextRect(), FALSE);
    } else if (wParam == TOAST_EXPIRY_TIMER_EVENT_ID) {
        m_strToast = "";
        this->InvalidateRect(GetToastTextRect(), FALSE);
    } else if (wParam == PAN_TIMER_EVENT_ID) {
        if (m_pCurrentImage != NULL) {
            ::KillTimer(this->m_hWnd, PAN_TIMER_EVENT_ID);
            //::timeBeginPeriod(1);

            bool bUp = 0;
            bool bDown = 0;
            bool bLeft = 0;
            bool bRight = 0;

            static int PanXbase = 0;
            static int PanYbase = 0;

            INT BorderDistanceY = 0;
            int PanX = 0;
            int PanY = 0;
            INT iRealHeight = 0;
            INT iRealWidth = 0;
            bool DoStopPan = 1;

            if (m_DynDwmFlush) {
                while (m_bPanTimerActive == true) {
                    bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
                    bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
                    bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
                    bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

                    if ((bUp == false) && (bDown == false) && (bLeft == false) && (bRight == false)) {
                        //::timeEndPeriod(1);
                        m_bPanTimerActive = false;
                        break;
                    } else {
                        iRealHeight = INT (m_dZoom * (m_pCurrentImage->OrigHeight()));
                        iRealWidth = INT (m_dZoom * (m_pCurrentImage->OrigWidth()));

                        if ((iRealHeight > m_clientRect.Height()) && (iRealWidth <= m_clientRect.Width()) && (GetAutoZoomMode() == Helpers::ZM_BookMode)) {
                            if (((bLeft == true) && (bRight == false)) || ((bUp == true) && (bDown == false)))
                                PanYbase = 10;
                            else if (((bLeft == false) && (bRight == true)) || ((bUp == false) && (bDown == true)))
                                PanYbase = -10;
                            else
                                PanYbase = 0;
                        } else {
                            if ((bUp == true) && (bDown == false))
                                PanYbase = 10;
                            else if ((bUp == false) && (bDown == true))
                                PanYbase = -10;
                            else
                                PanYbase = 0;

                            if ((bLeft == true) && (bRight == false))
                                PanXbase = 10;
                            else if (( bLeft == false) && (bRight == true))
                                PanXbase = -10;
                            else
                                PanXbase = 0;
                        }

                        if ((PanXbase != 0) && (PanYbase != 0)) {    // diagonal movement
                            PanX = (int)(PanXbase * 0.7);
                            PanY = (int)(PanYbase * 0.7);
                        } else {
                            PanX = PanXbase;
                            PanY = PanYbase;
                        }
                            
                        if (PerformPan(PanX,PanY,false) == true) {
                            //this->Invalidate(FALSE);
                            this->UpdateWindow();    // force wait until actually redrawn

                            MSG msg;
                            while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

                            Sleep(1);
                        } else {
                            MSG msg;
                            while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

                            Sleep(10);
                        }
                    }
                }
            } else {
                UINT timestart = Helpers::GetTimeMs();
                UINT timenow = 0;
                
                UINT modu_old = 4294967295;
                UINT modu = 0;
                
                while (m_bPanTimerActive == true) {
                    bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
                    bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
                    bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
                    bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

                    if ((bUp == false) && (bDown == false)  && (bLeft == false)  && (bRight == false)) {
                        //::timeEndPeriod(1);
                        m_bPanTimerActive = false;
                        break;
                    } else {
                        timenow = Helpers::GetTimeMs();
                        modu = ((timenow-timestart)*100) % 1668;     // 1663 on intel
                        INT BorderDistanceY = 0;

                        if (modu < modu_old) {
                            if (GetAutoZoomMode() != Helpers::ZM_BookMode) {
                                if ((bUp == true) && (bDown == false))
                                    PanY = 10;
                                else if ((bUp == false) && (bDown == true))
                                    PanY = -10;

                                if (( bLeft == true) && (bRight == false))
                                    PanX = 10;
                                else if (( bLeft == false) && (bRight == true))
                                    PanX = -10;
                            } else {
                                iRealHeight = INT (m_dZoom * (m_pCurrentImage->OrigHeight()));
                                iRealWidth = INT (m_dZoom * (m_pCurrentImage->OrigWidth()));

                                if ((iRealHeight > m_clientRect.Height()) && (iRealWidth <= m_clientRect.Width())) {
                                    if ((bLeft == true) && (bRight == false))
                                        PanY = 10;
                                    else if ((bUp == true) && (bDown == false))
                                        PanY = 10;

                                    if ((bLeft == false) && (bRight == true))
                                        PanY = -10;
                                    else if ((bUp == false) && (bDown == true))
                                        PanY = -10;

                                    if (m_offsets.y>0)
                                        BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)-m_offsets.y;    // Top
                                    else
                                        BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)+m_offsets.y;    // Bottom
                                    
                                    if (BorderDistanceY<=100) {
                                        if (BorderDistanceY<6)
                                            PanY = (int)(PanY*0.1);
                                        else if (BorderDistanceY<8)
                                            PanY = (int)(PanY*0.2);
                                        else if (BorderDistanceY<12)
                                            PanY = (int)(PanY*0.3);
                                        else if (BorderDistanceY<17)
                                            PanY = (int)(PanY*0.4);
                                        else if (BorderDistanceY<24)
                                            PanY = (int)(PanY*0.5);
                                        else if (BorderDistanceY<34)
                                            PanY = (int)(PanY*0.6);
                                        else if (BorderDistanceY<50)
                                            PanY = (int)(PanY*0.7);
                                        else if (BorderDistanceY<70)
                                            PanY = (int)(PanY*0.8);
                                        else
                                            PanY = (int)(PanY*0.9);
                                    }
                                } else {
                                    if ((bUp == true) && (bDown == false))
                                        PanY = 10;
                                    else if ((bUp == false) && (bDown == true))
                                        PanY = -10;

                                    if (( bLeft == true) && (bRight == false))
                                        PanX = 10;
                                    else if (( bLeft == false) && (bRight == true))
                                        PanX = -10;
                                }
                            }
                                
                            if ((PanX != 0) && (PanY != 0)) {    // diagonal movement
                                PanX = (int)(PanX * 0.7);
                                PanY = (int)(PanY * 0.7);
                            }

                            if (PerformPan(PanX,PanY,false) == true) {
                                //this->Invalidate(FALSE);
                                this->UpdateWindow();    // force wait until actually redrawn
                            }

                            MSG msg;
                            while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));    // Empty message queue at 60 Hz
                        }

                        modu_old = modu;
                        Sleep(1);
                    }
                }
            }
        }
    }

    return 0;
}

LRESULT CMainDlg::OnContextMenu(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
    // [GF] If there is already a menu being shown, cancel it and open a new one
    if (m_bInTrackPopupMenu == true) {
        ::SendMessage(this->m_hWnd, WM_CANCELMODE, 0, 0);
        ::Sleep(10);
        POINT MousePos;
        ::GetCursorPos(&MousePos);
        ::PostMessage(this->m_hWnd, WM_CONTEXTMENU, 0, (MousePos.x) | (MousePos.y << 16));
        return 1;
    }

    int nX = GET_X_LPARAM(lParam);
    int nY = GET_Y_LPARAM(lParam);

    MouseOn();

    HMENU hMenu = ::LoadMenu(_Module.m_hInst, _T("PopupMenu"));
    if (hMenu == NULL) return 1;

    HMENU hMenuTrackPopup = ::GetSubMenu(hMenu, 0);
    TranslateMenuStrings(hMenuTrackPopup, m_pKeyMap);
    
    if (m_bAutoContrast) ::CheckMenuItem(hMenuTrackPopup, IDM_AUTO_CORRECTION, MF_CHECKED);

    HMENU hMenuNavigation = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_FOLDERS);
    ::CheckMenuItem(hMenuNavigation,  m_pFileList->GetNavigationMode()*10 + IDM_LOOP_FOLDER, MF_CHECKED);
    
    HMENU hMenuOrdering = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_SORTMODE);
    ::CheckMenuItem(hMenuOrdering,  
        (m_pFileList->GetSorting() == Helpers::FS_LastModTime) ? IDM_SORT_MOD_DATE :
        (m_pFileList->GetSorting() == Helpers::FS_FileName) ? IDM_SORT_NAME :
        (m_pFileList->GetSorting() == Helpers::FS_Random) ? IDM_SORT_RANDOM : IDM_SORT_SIZE
        , MF_CHECKED);
    ::CheckMenuItem(hMenuOrdering, m_pFileList->IsSortedAscending() ? IDM_SORT_ASCENDING : IDM_SORT_DESCENDING, MF_CHECKED);
    if (m_pFileList->GetSorting() == Helpers::FS_Random) {
        ::EnableMenuItem(hMenuOrdering, IDM_SORT_ASCENDING, MF_BYCOMMAND | MF_GRAYED);
        ::EnableMenuItem(hMenuOrdering, IDM_SORT_DESCENDING, MF_BYCOMMAND | MF_GRAYED);
    }
    
    HMENU hMenuMovie = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_MOVIE);
    if (m_bMovieMode) {
        ::EnableMenuItem(hMenuTrackPopup, IDM_RELOAD_FULL, MF_BYCOMMAND | MF_GRAYED);
    } else {
        ::EnableMenuItem(hMenuMovie, IDM_STOP_MOVIE, MF_BYCOMMAND | MF_GRAYED);
    }
    ::CheckMenuItem(hMenuMovie, m_eTransitionEffect + IDM_EFFECT_NONE, MF_CHECKED);
    int nIndex = (m_nTransitionTime < 180) ? 0 : (m_nTransitionTime < 375) ? 1 : (m_nTransitionTime < 750) ? 2 : (m_nTransitionTime < 1500) ? 3 : 4;
    ::CheckMenuItem(hMenuMovie, nIndex + IDM_EFFECTTIME_VERY_FAST, MF_CHECKED);
    
    ::EnableMenuItem(hMenuMovie, IDM_SLIDESHOW_START, MF_BYCOMMAND | MF_GRAYED);
    ::EnableMenuItem(hMenuMovie, IDM_MOVIE_START_FPS, MF_BYCOMMAND | MF_GRAYED);
    
    HMENU hMenuZoom = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_WINDOW);
    if (m_bSpanVirtualDesktop) ::CheckMenuItem(hMenuZoom,  IDM_SPAN_SCREENS, MF_CHECKED);
    if (m_bFullScreenMode) ::CheckMenuItem(hMenuZoom,  IDM_FULL_SCREEN_MODE, MF_CHECKED);
    if (m_bWindowBorderless) ::CheckMenuItem(hMenuZoom, IDM_HIDE_TITLE_BAR, MF_CHECKED);
    if (m_bAlwaysOnTop) ::CheckMenuItem(hMenuZoom, IDM_ALWAYS_ON_TOP, MF_CHECKED);
    if (IsAdjustWindowToImage() && IsImageExactlyFittingWindow()) ::CheckMenuItem(hMenuZoom, IDM_FIT_WINDOW_TO_IMAGE, MF_CHECKED);
    if (m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_FIT_WINDOW_TO_IMAGE, MF_BYCOMMAND | MF_GRAYED);
    if (!m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_SPAN_SCREENS, MF_BYCOMMAND | MF_GRAYED);
    if (m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_HIDE_TITLE_BAR, MF_BYCOMMAND | MF_GRAYED);
    
    HMENU hMenuAutoZoomMode = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_AUTOZOOMMODE);
    ::CheckMenuItem(hMenuAutoZoomMode, GetAutoZoomMode() * 10 + IDM_AUTO_ZOOM_NONE, MF_CHECKED);

    if (m_pCurrentImage == NULL) {
        ::EnableMenuItem(hMenuTrackPopup, IDM_RELOAD_FULL, MF_BYCOMMAND | MF_GRAYED);
        //::EnableMenuItem(hMenuTrackPopup, IDM_EXPLORE, MF_BYCOMMAND | MF_GRAYED);  // can still show path to an image which could not be loaded.  If file doesn't exist, nothing happens anyways
        ::EnableMenuItem(hMenuTrackPopup, IDM_COPY, MF_BYCOMMAND | MF_GRAYED);
        ::EnableMenuItem(hMenuTrackPopup, IDM_COPY_FULL, MF_BYCOMMAND | MF_GRAYED);
        ::EnableMenuItem(hMenuTrackPopup, IDM_COPY_PATH, MF_BYCOMMAND | MF_GRAYED);
        ::EnableMenuItem(hMenuTrackPopup, SUBMENU_POS_WINDOW, MF_BYPOSITION  | MF_GRAYED);
        ::EnableMenuItem(hMenuTrackPopup, SUBMENU_POS_TRANSFORM, MF_BYPOSITION  | MF_GRAYED);
    }

    int nMenuCmd = TrackPopupMenu(CPoint(nX, nY), hMenuTrackPopup);
    ExecuteCommand(nMenuCmd);

    ::DestroyMenu(hMenu);
    return 1;
}

// Make the text edit control for renaming image colored black/white
LRESULT CMainDlg::OnCtlColorEdit(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
    HDC hDC = (HDC) wParam;
    ::SetTextColor(hDC, RGB(255, 255, 255));
    ::SetBkColor(hDC, RGB(0, 0, 0));
    return (LRESULT)::GetStockObject(BLACK_BRUSH);
}

LRESULT CMainDlg::OnEraseBackground(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& bHandled) {
    // prevent erasing background
    bHandled = TRUE;
    return TRUE;
}

LRESULT CMainDlg::OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
    // NOP
    return 0;
}

LRESULT CMainDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
    CleanupAndTerminate();
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void CMainDlg::ExecuteCommand(int nCommand) {
    CSettingsProvider& sp = CSettingsProvider::This();
    switch (nCommand) {
        case IDM_MINIMIZE:
            this->ShowWindow(SW_MINIMIZE);
            break;
        case IDM_EXPLORE:
            qDebug() << "IDM_EXPLORE";
            ExploreFile();
            break;
        case IDM_SETTINGS_DIALOG:
            qDebug() << "IDM_SETTINGS_DIALOG";
            sp.ShowSettingsDialog();
            break;
        case IDM_RELOAD:
            ReloadImage(false);
            break;
        case IDM_RELOAD_FULL:
            if (m_pFileList != NULL && m_pFileList->CurrentFileExists())
                {
                m_pFileList->Reload(NULL);
                this->Invalidate(FALSE);
                }

            ReloadImage(false);
            break;
        case IDM_COPY:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyImageToClipboard(m_pCurrentImage, m_pFileList->Current());
            }
            break;
        case IDM_COPY_FULL:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyFullImageToClipboard(m_pCurrentImage, *m_pImageProcParams, CreateDefaultProcessingFlags(), m_pFileList->Current());
                this->Invalidate(FALSE);
            }
            break;
        case IDM_COPY_PATH:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyPathToClipboard(m_pCurrentImage, m_pFileList->Current());
            }
            break;
        case IDM_CUT_FILE:
            if (m_pCurrentImage != NULL) {
                CClipboard::CutFileToClipboard(m_pFileList->Current());
            }
            break;
        case IDM_DELETE_FILE:
            MouseOn();
            if (m_pCurrentImage != NULL && m_pFileList != NULL) {
                QString currentFileName = CurrentFileName(false);
                if (currentFileName.isEmpty()) {
                    break;
                }

                bool allowDelete = false;

                Helpers::EDeleteConfirmation confirmationPolicy = CSettingsProvider::This().DeleteConfirmation();
                if (confirmationPolicy == Helpers::DC_Never) {
                    allowDelete = true;
                } else {
                    // Need to ask the user

                    // Qt Core (funktioniert auch ohne QApplication!)
                    QFileInfo fileInfo(currentFileName);
                    QString fileSizeStr = FormatFileSize(fileInfo.size());

                    // Nachrichtentext mit QString zusammenbauen
                    QString message = QString("%1\n\n%2\n\n%3 %4")
                                          .arg("Do you really want to permanently delete this file?")
                                          .arg(currentFileName)
                                          .arg("Size:")
                                          .arg(fileSizeStr);

                    // Todo: used 'this' as parent after switching to QWidget derived main window class
                    auto ret = QMessageBox::question(nullptr,
                                                     CNLS::GetString("Confirm"),
                                                     message,
                                                     QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                                     QMessageBox::Cancel); // Default-Button

                    if (ret == QMessageBox::Yes) {
                        allowDelete = true;
                    }
                }

                if (allowDelete) {
                    CFileList* fileListOfDeletedImage = m_pFileList;
                    GotoImage(POS_AwayFromCurrent, NO_REQUEST);
                    if (QFile::remove(currentFileName)) {
                        fileListOfDeletedImage->Reload(NULL, false);
                        m_pFileList->DeleteHistory(true);
                        Invalidate();
                        GotoImage(POS_Current);
                    } else {
                        Invalidate();
                        if (m_pFileList->Current().isEmpty()) {
                            GotoImage(POS_First);
                        } else {
                            GotoImage(POS_Previous);
                        }
                    }
                }
            }
            break;
        case IDM_NEXTFRAME:
            GotoImage(POS_NextFrame);
            break;
        case IDM_NEXTFILE:
            GotoImage(POS_NextFile);
            break;
        case IDM_PREVFILE:
            GotoImage(POS_PreviousFile);
            break;
        case IDM_NEXT:
            GotoImage(POS_Next);
            break;
        case IDM_PREV:
            GotoImage(POS_Previous);
            break;
        case IDM_FIRST:
            SetToast(QStringLiteral("Jump: First image"));
            GotoImage(POS_First);
            break;
        case IDM_LAST:
            SetToast(QStringLiteral("Jump: Last image"));
            GotoImage(POS_Last);
            break;
        case IDM_PLAYPAUSE:
            if (m_bIsAnimationPlaying) {
                SetToast(QStringLiteral("Animation Paused"));
                StopAnimation(); // stop any running animation
            } else if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
                SetToast(QStringLiteral("Animation Continued"));
                StartAnimation();
            }
            break;
        case IDM_PLAYPAUSENEXT:
            if (m_bIsAnimationPlaying) {
                SetToast(QStringLiteral("Animation Paused"));
                StopAnimation(); // stop any running animation
            } else if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
                SetToast(QStringLiteral("Animation Continued"));
                StartAnimation();
            } else {
                GotoImage(POS_Next);
            }
            break;
        case IDM_LOOP_FOLDER:
        case IDM_LOOP_RECURSIVELY:
        case IDM_LOOP_SIBLINGS:
            m_pFileList->SetNavigationMode(
                (nCommand == IDM_LOOP_FOLDER) ? Helpers::NM_LoopDirectory :
                (nCommand == IDM_LOOP_RECURSIVELY) ? Helpers::NM_LoopSubDirectories : 
                Helpers::NM_LoopSameDirectoryLevel);
            break;
        case IDM_SORT_NAME:
        case IDM_SORT_MOD_DATE:
        case IDM_SORT_SIZE:
        case IDM_SORT_RANDOM:
            {
                Helpers::ESorting nSortMode = m_pFileList->GetSorting();
                bool bSortedAscending = m_pFileList->IsSortedAscending();
                
                if (nCommand != IDM_SORT_RANDOM) {
                    m_eSortModeBeforeRandom = nSortMode;
                }

                nSortMode = (nCommand == IDM_SORT_MOD_DATE) ? Helpers::FS_LastModTime :
                            (nCommand == IDM_SORT_RANDOM) ? Helpers::FS_Random :
                            (nCommand == IDM_SORT_SIZE) ? Helpers::FS_FileSize : Helpers::FS_FileName;
                
                ToastSortingMode(nSortMode, bSortedAscending);
                m_pFileList->SetSorting(nSortMode, bSortedAscending);
            }
            break;
        case IDM_SORT_ASCENDING:
        case IDM_SORT_DESCENDING:
            m_pFileList->SetSorting(m_pFileList->GetSorting(), nCommand == IDM_SORT_ASCENDING);
            break;
        case IDM_STOP_MOVIE:
            SetToast(QStringLiteral("Slideshow Paused"));
            StopMovieMode();
            break;
        case IDM_SLIDESHOW_RESUME:
            {
            const int timerSec = static_cast<int>(1.0 / m_dMovieFPS);
            SetToast(QStringLiteral("Slideshow Resumed (Timer %1 s)").arg(timerSec));
            StartMovieMode(m_dMovieFPS);
            }
            break;
        case IDM_SLIDESHOW_1:
        case IDM_SLIDESHOW_2:
        case IDM_SLIDESHOW_3:
        case IDM_SLIDESHOW_4:
        case IDM_SLIDESHOW_5:
        case IDM_SLIDESHOW_6:
        case IDM_SLIDESHOW_7:
        case IDM_SLIDESHOW_8:
        case IDM_SLIDESHOW_9:
        case IDM_SLIDESHOW_10:
        case IDM_SLIDESHOW_20:
            {
            int nValue = nCommand - IDM_SLIDESHOW_START;
            SetToast(QStringLiteral("Starting Slideshow (Timer %d s)").arg(nValue));
            StartMovieMode(1.0/nValue);
            }
            break;
        case IDM_EFFECT_NONE:
        case IDM_EFFECT_BLEND:
        case IDM_EFFECT_SLIDE_RL:
        case IDM_EFFECT_SLIDE_LR:
        case IDM_EFFECT_SLIDE_TB:
        case IDM_EFFECT_SLIDE_BT:
        case IDM_EFFECT_ROLL_RL:
        case IDM_EFFECT_ROLL_LR:
        case IDM_EFFECT_ROLL_TB:
        case IDM_EFFECT_ROLL_BT:
        case IDM_EFFECT_SCROLL_RL:
        case IDM_EFFECT_SCROLL_LR:
        case IDM_EFFECT_SCROLL_TB:
        case IDM_EFFECT_SCROLL_BT:
            m_eTransitionEffect = (Helpers::ETransitionEffect)(nCommand - IDM_EFFECT_NONE);
            break;
        case IDM_EFFECTTIME_VERY_FAST:
        case IDM_EFFECTTIME_FAST:
        case IDM_EFFECTTIME_NORMAL:
        case IDM_EFFECTTIME_SLOW:
        case IDM_EFFECTTIME_VERY_SLOW:
            m_nTransitionTime = 125 * (1 << (nCommand - IDM_EFFECTTIME_VERY_FAST));
            break;
        case IDM_MOVIE_5_FPS:
        case IDM_MOVIE_10_FPS:
        case IDM_MOVIE_15_FPS:
        case IDM_MOVIE_20_FPS:
        case IDM_MOVIE_25_FPS:
        case IDM_MOVIE_30_FPS:
        case IDM_MOVIE_50_FPS:
        case IDM_MOVIE_60_FPS:
        case IDM_MOVIE_100_FPS:
            {
                QString toastText;
                const int nValue = nCommand - IDM_MOVIE_START_FPS;

                if (m_pCurrentImage != nullptr && m_pCurrentImage->IsAnimation()) {
                    const int targetFrameTime = 1000 / nValue;

                    if (m_customAnimationFrameTime == targetFrameTime) {
                        m_customAnimationFrameTime = 1000;
                        const int defaultFps = 1000 / m_pCurrentImage->FrameTimeMs();
                        toastText = QStringLiteral("Default Playback Speed: %1 fps").arg(defaultFps);
                    } else {
                        m_customAnimationFrameTime = targetFrameTime;
                        toastText = QStringLiteral("Custom Playback Speed: %1 fps").arg(nValue);
                    }
                    SetToast(toastText);
                } else {
                    toastText = QStringLiteral("Starting Playback (%1 fps)").arg(nValue);
                    SetToast(toastText);
                    StartMovieMode(nValue);
                }
            }
            break;
        case IDM_ROTATE_90:
        case IDM_ROTATE_270:
            if (m_pCurrentImage != NULL) {
                uint32 nRotationDelta = (nCommand == IDM_ROTATE_90) ? 90 : 270;
                m_nRotation = (m_nRotation + nRotationDelta) % 360;
                m_nUserRotation = (m_nUserRotation + nRotationDelta) % 360;
                m_pCurrentImage->Rotate(nRotationDelta);
                if (!IsAdjustWindowToImage()) m_dZoom = -1;
                m_bUserZoom = false;
                m_bUserPan = false;
                AdjustWindowToImage(false);
                this->Invalidate(FALSE);
            }
            break;
        case IDM_MIRROR_H:
        case IDM_MIRROR_V:
            if (m_pCurrentImage != NULL) {
                m_pCurrentImage->Mirror(nCommand == IDM_MIRROR_H);
                this->Invalidate(FALSE);
            }
            break;
        case IDM_AUTO_CORRECTION:
            m_bAutoContrast = !m_bAutoContrast;
            SetToast(m_bAutoContrast ? QStringLiteral("Auto Contrast: ON") : QStringLiteral("Auto Contrast: OFF"));
            this->Invalidate(FALSE);
            break;
        case IDM_KEEP_PARAMETERS:
            m_bKeepParams = !m_bKeepParams;
            if (m_bKeepParams) {
                *m_pImageProcParamsKept = *m_pImageProcParams;
                m_eProcessingFlagsKept = CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, m_bKeepParams);
                m_dZoomKept = (m_bUserZoom || IsAdjustWindowToImage()) ? m_dZoom : -1;
                m_offsetKept = m_bUserPan ? m_offsets : CPoint(0, 0);
            }
            break;
        case IDM_SAVE_PARAMETERS:
            SaveParameters();
            break;
        case IDM_FIT_TO_SCREEN:
        case IDM_FIT_TO_SCREEN_NO_ENLARGE:
            ResetZoomToFitScreen(false, nCommand == IDM_FIT_TO_SCREEN, true);
            break;
        case IDM_FILL_WITH_CROP:
            ResetZoomToFitScreen(true, true, true);
            break;
        case IDM_TOGGLE_AUTO_ZOOM_FIT:
            {
            // This command is meant to be used with keyboard shortcuts.
            // It toggles the AutoZoomMode between FitToScreen and None,
            // or, in case of BookMode, between FitToScreen and BookMode.
            // This is meant as an alternative to IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS,
            // which changes the view size only for the current image.
                bool bIsBookModeFile = false;
                Helpers::EAutoZoomMode eAutoZoomMode = Helpers::ZM_FitToScreen;
                QString sFilePath = CurrentFileName(false);

                if (!sFilePath.isEmpty()) {
                    if (IsBookModeFile(sFilePath)) {
                        bIsBookModeFile = true;
                    }
                }

                m_dZoom = -1.0;
                m_offsets = CPoint(0, 0);

                if (bIsBookModeFile == true) {
                    if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                        eAutoZoomMode = Helpers::ZM_FitToScreen;
                    } else {
                        eAutoZoomMode = Helpers::ZM_BookMode;
                    }
                } else {
                    if (GetAutoZoomMode() == Helpers::ZM_FitToScreen) {
                        eAutoZoomMode = Helpers::ZM_None;
                        m_dZoom = 1.0;
                    } else {
                        eAutoZoomMode = Helpers::ZM_FitToScreen;
                    }
                }

                if (m_eAutoZoomModeFullscreen == m_eAutoZoomModeWindowed)
                    m_eAutoZoomModeFullscreen = m_eAutoZoomModeWindowed = eAutoZoomMode;
                else if (m_bFullScreenMode)
                    m_eAutoZoomModeFullscreen = eAutoZoomMode;
                else
                    m_eAutoZoomModeWindowed = eAutoZoomMode;


                m_bInZooming = true;
                StartLowQTimer(ZOOM_TIMEOUT);
                this->Invalidate(FALSE);
                AdjustWindowToImage(false);
            }
            break;
        case IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS:
        case IDM_TOGGLE_FILL_WITH_CROP_100_PERCENTS:
            m_bShowZoomFactor = true;
            if (fabs(m_dZoom - 1) < 0.01) {
                ResetZoomToFitScreen(nCommand == IDM_TOGGLE_FILL_WITH_CROP_100_PERCENTS, true, true);
            } else {
                ResetZoomTo100Percents(m_bMouseOn);
            }
            break;
        case IDM_SPAN_SCREENS:
            if (m_bFullScreenMode && CMultiMonitorSupport::IsMultiMonitorSystem()) {
                m_dZoom = -1.0;
                this->Invalidate(FALSE);
                if (m_bSpanVirtualDesktop) {
                    this->SetWindowPlacement(&m_storedWindowPlacement);
                } else {
                    this->GetWindowPlacement(&m_storedWindowPlacement);
                    CRect rectAllScreens = CMultiMonitorSupport::GetVirtualDesktop();
                    this->SetWindowPos(HWND_TOP, &rectAllScreens, SWP_NOZORDER);
                }
                m_bSpanVirtualDesktop = !m_bSpanVirtualDesktop;
                this->GetClientRect(&m_clientRect);
            }
            break;
        case IDM_FULL_SCREEN_MODE:
            m_bFullScreenMode = !m_bFullScreenMode;
            m_dZoomAtResizeStart = 1.0;
            if (!m_bFullScreenMode) {
                CRect windowRect;

                // restore hidden title bar if enabled
                SetCurrentWindowStyle();

                HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), 
                    IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
                SetIcon(hIconSmall, FALSE);
                CRect defaultWindowRect = CMultiMonitorSupport::GetDefaultWindowRect();
                double dZoom = -1;
                windowRect = sp.ExplicitWindowRect() ?
                    defaultWindowRect :
                    GetWindowRectMatchingImageSize(
                        m_hWnd,
                        CSize(MIN_WND_WIDTH, MIN_WND_HEIGHT),
                        defaultWindowRect.Size(),
                        dZoom, m_pCurrentImage, false, true, m_bWindowBorderless, GetAutoZoomMode());
                this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
                UpdateWindowTitle(true);    // force update of title bar icon
                this->MouseOn();
                m_bSpanVirtualDesktop = false;
            } else {
                if (!IsZoomed() && sp.ExplicitWindowRect()) {
                    // Save the old window rect to be able to restore it
                    CRect rect;
                    GetWindowRect(&rect);
                    CMultiMonitorSupport::SetDefaultWindowRect(rect);
                }
                HMONITOR hMonitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO monitorInfo;
                monitorInfo.cbSize = sizeof(MONITORINFO);
                if (::GetMonitorInfo(hMonitor, &monitorInfo)) {
                    CRect monitorRect(&(monitorInfo.rcMonitor));
                    this->SetWindowLongW(GWL_STYLE, WS_VISIBLE);
                    this->SetWindowPos(HWND_TOP, monitorRect.left, monitorRect.top, monitorRect.Width(), monitorRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
                }
                this->MouseOn();
            }
            m_dZoom = -1;
            StartLowQTimer(ZOOM_TIMEOUT);
            this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
            break;
        case IDM_HIDE_TITLE_BAR:
            if (!m_bFullScreenMode) {
                // only available when full screen mode is not active

                m_bWindowBorderless = !m_bWindowBorderless;
                SetCurrentWindowStyle();

                // get the size of the border to shift the window pos downwards
                int windowCaptionHeight = ::GetSystemMetrics(SM_CYCAPTION);
                double dZoom = -1;
                CRect windowRect = GetWindowRectMatchingImageSize(
                    m_hWnd, CSize(MIN_WND_WIDTH, MIN_WND_HEIGHT), HUGE_SIZE, dZoom, m_pCurrentImage, false, true, m_bWindowBorderless, GetAutoZoomMode());

                // don't try to adjust for an image that isn't loaded!
                if (m_pCurrentImage != NULL)
                {
                    // this is the new top to move it to so that the experience seems seamless
                    int newTop;
                    int t = m_pCurrentImage->OrigHeight();

                    // these are experimental values figured out through trial and error
                    // it appears if the caption size is odd, and just using /2,
                    // it causes the window to shift up one pixel at a time when going between borderless and not borderless repeatedly
                    // in other cases, it shifts downwards depending on rounding errors resizing the window and image... hard to hunt down but it's as good as it can get right now
                    if (windowCaptionHeight % 2 == 0) {
                        newTop = m_bWindowBorderless ?
                            windowRect.top + (windowCaptionHeight / 2) :
                            windowRect.top - (windowCaptionHeight / 2);
                    } else {
                        newTop = m_bWindowBorderless ?
                            windowRect.top + (windowCaptionHeight / 2) :
                            windowRect.top - (windowCaptionHeight / 2) + 1;
                    }

                    // tell the window the Frame has changed, not sure if it makes a difference
                    this->SetWindowPos(HWND_TOP, windowRect.left, newTop, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);

                    // don't auto adjust unless auto is selected in options
                    if (IsAdjustWindowToImage() && !(m_bAutoFitWndToImage && !IsImageExactlyFittingWindow())) {
                        AdjustWindowToImage(false);
                        this->Invalidate(FALSE);
                    }

                    StartLowQTimer(ZOOM_TIMEOUT);  // trigger a redraw as if zoom changed (might not be necessary)
                }
            }

            break;
        case IDM_ALWAYS_ON_TOP:
            ToggleAlwaysOnTop();
            SetToast(m_bAlwaysOnTop? QStringLiteral("Always on Top: enabled") : QStringLiteral("Always on Top: disabled"));
            break;
        case IDM_FIT_WINDOW_TO_IMAGE:
            // Note: If auto fit is on but the window size does not match the image size (due to manual window resizing), restore window to image
            if (!(m_bAutoFitWndToImage && !IsImageExactlyFittingWindow()))
                m_bAutoFitWndToImage = !m_bAutoFitWndToImage;
            AdjustWindowToImage(false);
            break;
        case IDM_ZOOM_400:
            PerformZoom(4.0, false, m_bMouseOn, true);
            break;
        case IDM_ZOOM_200:
            PerformZoom(2.0, false, m_bMouseOn, true);
            break;
        case IDM_ZOOM_100:
            ResetZoomTo100Percents(m_bMouseOn);
            break;
        case IDM_ZOOM_50:
            PerformZoom(0.5, false, m_bMouseOn, true);
            break;
        case IDM_ZOOM_25:
            PerformZoom(0.25, false, m_bMouseOn, true);
            break;
        case IDM_ZOOM_INC:
        case IDM_ZOOM_DEC:
            PerformZoom((nCommand == IDM_ZOOM_INC) ? 1 : -1, true, m_bMouseOn, true);
            break;
        case IDM_ZOOM_MODE:
            m_bZoomModeOnLeftMouse = !m_bZoomModeOnLeftMouse;
            break;
        case IDM_AUTO_ZOOM_NONE:
        case IDM_AUTO_ZOOM_FIT_NO_ZOOM:
        case IDM_AUTO_ZOOM_FILL_NO_ZOOM:
        case IDM_AUTO_ZOOM_FIT:
        case IDM_AUTO_ZOOM_FILL:
        case IDM_AUTO_ZOOM_BOOK_MODE:
            {
                Helpers::EAutoZoomMode eAutoZoomMode = (Helpers::EAutoZoomMode)((nCommand - IDM_AUTO_ZOOM_NONE) / 10);
                if (m_eAutoZoomModeFullscreen == m_eAutoZoomModeWindowed)
                    m_eAutoZoomModeFullscreen = m_eAutoZoomModeWindowed = eAutoZoomMode;
                else if (m_bFullScreenMode)
                    m_eAutoZoomModeFullscreen = eAutoZoomMode;
                else
                    m_eAutoZoomModeWindowed = eAutoZoomMode;
                m_dZoom = -1.0;
                m_offsets = CPoint(0, 0);
                this->Invalidate(FALSE);
                AdjustWindowToImage(false);
                break;
            }
        case IDM_EDIT_USER_CONFIG:
            EditINIFile(false);
            break;
        case IDM_EXIT:
            CleanupAndTerminate();
            break;
        case IDM_DEFAULT_ESC:
            if (m_bMovieMode) {
                if (m_bAutoExit) {
                    CleanupAndTerminate();
                } else {
                    SetToast(QStringLiteral("Slideshow Paused"));

                    StopMovieMode(); // stop any running movie/slideshow
                }
            } else if (m_bIsAnimationPlaying) {
                if (m_bAutoExit) {
                    CleanupAndTerminate();
                } else {
                    SetToast(QStringLiteral("Animation Paused"));
                    StopAnimation(); // stop any running animation
                }
            } else if (m_bShowInfo == true) {
                m_bShowInfo = false;
                this->Invalidate(FALSE);
            } else if (m_bFullScreenMode) {
                ExecuteCommand(IDM_FULL_SCREEN_MODE);
            } else {
                CleanupAndTerminate();
            }
            break;
        case IDM_CONTRAST_CORRECTION_INC:
        case IDM_CONTRAST_CORRECTION_DEC:
            if (m_bAutoContrast) {
                double dInc = (nCommand == IDM_CONTRAST_CORRECTION_INC) ? 0.05 : -0.05;
                m_pImageProcParams->ContrastCorrectionFactor = std::max(0.0, std::min(1.0, m_pImageProcParams->ContrastCorrectionFactor + dInc));
                this->Invalidate(FALSE);
            }
            break;
        case IDM_COLOR_CORRECTION_INC:
        case IDM_COLOR_CORRECTION_DEC:
            if (m_bAutoContrast) {
                double dInc = (nCommand == IDM_COLOR_CORRECTION_INC) ? 0.05 : -0.05;
                m_pImageProcParams->ColorCorrectionFactor = std::max(-0.5, std::min(0.5, m_pImageProcParams->ColorCorrectionFactor + dInc));
                this->Invalidate(FALSE);
            }
            break;
        case IDM_TOGGLE_TRANSPARENCY:
            if (m_eTransparencyMode == Helpers::TP_BLEND)
            {
                m_eTransparencyMode = Helpers::TP_CHECKERBOARD;
                SetToast(QStringLiteral("Transparency: Checkerboard"));
            }
            else if (m_eTransparencyMode == Helpers::TP_CHECKERBOARD)
            {
                m_eTransparencyMode = Helpers::TP_BLEND_INVERSE;
                SetToast(QStringLiteral("Transparency: Inverse Blend"));
            }
            else //if (m_eTransparencyMode == Helpers::TP_BLEND_INVERSE)
            {
                m_eTransparencyMode = Helpers::TP_BLEND;
                SetToast(QStringLiteral("Transparency: Blend"));
            }
            m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
            m_pJPEGProvider->ClearAllRequests();
            m_pCurrentImage = NULL;
            ReloadImage(true);
            break;
        case IDM_TOGGLE_USE_EMBEDDED_COLOR_PROFILES:
            m_bUseEmbeddedColorProfiles = !m_bUseEmbeddedColorProfiles;
            CSettingsProvider::This().SetUseEmbeddedColorProfiles(m_bUseEmbeddedColorProfiles);
            if (m_bUseEmbeddedColorProfiles == false) {
                SetToast(QStringLiteral("Embedded Color Profiles: ignored"));
            } else {
                SetToast(QStringLiteral("Embedded Color Profiles: used"));
            }
            m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
            m_pJPEGProvider->ClearAllRequests();
            m_pCurrentImage = NULL;
            ReloadImage(true);
            break;
        case IDM_TOGGLE_RESAMPLING_QUALITY:
            m_bHQResampling = !m_bHQResampling;
            SetToast(m_bHQResampling? QStringLiteral("Resampling: enabled"): QStringLiteral("Resampling: disabled"));
            this->Invalidate(FALSE);
            break;
        case IDM_TOGGLE_MONITOR:
            ToggleMonitor();
            break;
        case IDM_EXCHANGE_PROC_PARAMS:
            ExchangeProcessingParams();
            break;
        case IDM_PAN_UP:
        case IDM_PAN_DOWN:
        case IDM_PAN_RIGHT:
        case IDM_PAN_LEFT:
            if (sp.SmartPanningKeys() == false) {
                if (m_pCurrentImage != NULL) {
                    int iRealHeight = (int) (m_dZoom * (m_pCurrentImage->OrigHeight()));
                    int iRealWidth = (int) (m_dZoom * (m_pCurrentImage->OrigWidth()));

                    if (((iRealHeight > m_clientRect.Height()) && (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN))
                     || (( iRealWidth >  m_clientRect.Width()) && (nCommand == IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                ::SetTimer(this->m_hWnd, PAN_TIMER_EVENT_ID, 10, NULL);
                            }
                        } else {
                            PerformPan((nCommand == IDM_PAN_LEFT) ? PAN_STEP : (nCommand == IDM_PAN_RIGHT) ? -PAN_STEP : 0,
                                (nCommand == IDM_PAN_UP) ? PAN_STEP : (nCommand == IDM_PAN_DOWN) ? -PAN_STEP : 0, false);
                        }                    
                    } else {
                        PerformPan((nCommand == IDM_PAN_LEFT) ? PAN_STEP : (nCommand == IDM_PAN_RIGHT) ? -PAN_STEP : 0,
                            (nCommand == IDM_PAN_UP) ? PAN_STEP : (nCommand == IDM_PAN_DOWN) ? -PAN_STEP : 0, false);
                    }
                } else {
                    PerformPan((nCommand == IDM_PAN_LEFT) ? PAN_STEP : (nCommand == IDM_PAN_RIGHT) ? -PAN_STEP : 0,
                        (nCommand == IDM_PAN_UP) ? PAN_STEP : (nCommand == IDM_PAN_DOWN) ? -PAN_STEP : 0, false);
                }
            } else {
                if (m_pCurrentImage != NULL) {
                    int iRealHeight = (int) (m_dZoom * (m_pCurrentImage->OrigHeight()));
                    int iRealWidth = (int) (m_dZoom * (m_pCurrentImage->OrigWidth()));

                    if (((iRealHeight > m_clientRect.Height()) && (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN))
                     || (( iRealWidth >  m_clientRect.Width()) && (nCommand == IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                ::SetTimer(this->m_hWnd, PAN_TIMER_EVENT_ID, 10, NULL);
                            }
                        } else {
                            PerformPan((nCommand == IDM_PAN_LEFT) ? PAN_STEP : (nCommand == IDM_PAN_RIGHT) ? -PAN_STEP : 0,
                                (nCommand == IDM_PAN_UP) ? PAN_STEP : (nCommand == IDM_PAN_DOWN) ? -PAN_STEP : 0, false);
                        }                    
                    } else {
                        if (GetAutoZoomMode() != Helpers::ZM_BookMode) {
                            if (nCommand == IDM_PAN_RIGHT)
                                GotoImage(POS_Next);
                            else if (nCommand == IDM_PAN_LEFT)
                                GotoImage(POS_Previous);
                        } else {
                            if (iRealHeight > m_clientRect.Height()) {
                                if (m_bPanTimerActive == false) {
                                    if ((m_offsets.y >= ((iRealHeight-m_clientRect.Height())/2)) && (nCommand == IDM_PAN_LEFT)) {    // Upper Border!
                                        GotoImage(POS_Previous);
                                    } else if ((-m_offsets.y >= ((iRealHeight-m_clientRect.Height())/2)) && (nCommand == IDM_PAN_RIGHT)) {    //Lower Border!
                                        GotoImage(POS_Next);
                                    } else {
                                        if (m_bAllowSmoothPanning) {
                                            m_bPanTimerActive = true;
                                            ::SetTimer(this->m_hWnd, PAN_TIMER_EVENT_ID, 10, NULL);
                                        } else {
                                            PerformPan(0, ((nCommand == IDM_PAN_UP) || (nCommand == IDM_PAN_LEFT)) ? PAN_STEP : ((nCommand == IDM_PAN_DOWN) || (nCommand == IDM_PAN_RIGHT)) ? -PAN_STEP : 0, false);
                                        }
                                    }
                                }
                            } else {
                                if (m_bPanTimerActive == false) {
                                    if (nCommand == IDM_PAN_RIGHT)
                                        GotoImage(POS_Next);
                                    else if (nCommand == IDM_PAN_LEFT)
                                        GotoImage(POS_Previous);
                                }
                            }
                        }
                    }
                } else {
                    // No image. No dimensions. Panning impossible. GotoImage
                    if (nCommand == IDM_PAN_RIGHT)
                        GotoImage(POS_Next);
                    else if (nCommand == IDM_PAN_LEFT)
                        GotoImage(POS_Previous);
                }
            }
            break;
        case IDM_CONTEXT_MENU:
            {
            BOOL bNotUsed;
            /*
            OnContextMenu() expects screen coordinates, but m_nMouseX/m_nMouseY contain client area coordinates!
            When OnContextMenu() gets called after WM_CONTEXTMENU from the Message Handler, it will receive screen
            coordinates in LPARAM from the beginning, but when using m_nMouseX/m_nMouseY, we need to convert first.
            */
            int MouseCoordScreen = (m_nMouseX & 0xFFFF) | ((m_nMouseY & 0xFFFF) << 16);
            ::ClientToScreen(this->m_hWnd, (LPPOINT)&MouseCoordScreen);
            OnContextMenu(0, 0, MouseCoordScreen, bNotUsed);
            }
            break;
        case IDM_TOGGLE_SORT_RANDOM:
            if (m_pFileList != NULL) {
                Helpers::ESorting nSortMode = m_pFileList->GetSorting();
                bool bSortedAscending = m_pFileList->IsSortedAscending();
                
                if (nSortMode != Helpers::FS_Random) {
                    m_eSortModeBeforeRandom = nSortMode;
                    nSortMode = Helpers::FS_Random;
                } else if (nSortMode == Helpers::FS_Random) {
                    nSortMode = m_eSortModeBeforeRandom;
                }

                ToastSortingMode(nSortMode, bSortedAscending);
                m_pFileList->SetSorting(nSortMode, bSortedAscending);
            }
            break;
        case IDM_TOGGLE_INFO_OVERLAY:
            m_bShowInfo = !m_bShowInfo;
            this->Invalidate(FALSE);
            break;
        case IDM_CHANGE_FOLDER_LANGUAGE:
            ChangeFolderLanguage();
            break;
        case IDM_OPEN_DEFAULT_EDITOR:
            OpenDefaultEditor();
            break;
    }
}

void CMainDlg::ChangeFolderLanguage() {
    const QString sCurrentFileName = CurrentFileName(false);
    if (sCurrentFileName.isEmpty()) {
        return;
    }

    const QFileInfo fileInfo(sCurrentFileName);
    const QString fileNameNoExt = fileInfo.completeBaseName(); // Dateiname ohne Endung
    const QDir parentDir = fileInfo.dir();                    // Übergeordneter Ordner

    QDir parentDir2 = parentDir;
    parentDir2.cdUp();                                        // Ein Verzeichnis höher

    const QString parentDirName = parentDir.dirName();

    // 2. Sprach-Tags
    static const QList<QString> langTags = { QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("ch"), QStringLiteral("ko") };

    // Regex: Erkennt Muster wie "Ordnername [de]"
    static const QRegularExpression rx(QStringLiteral(R"(^(.+)\s+\[(en|de|ch|ko)\]$)"));
    const QRegularExpressionMatch match = rx.match(parentDirName);

    QString baseFolder;
    QString currentLang;

    if (match.hasMatch()) {
        baseFolder = match.captured(1);
        currentLang = match.captured(2);
    } else {
        baseFolder = parentDirName;
    }

    // 3. Relevante Sprachen/Ordner in Suchreihenfolge ermitteln
    QList<QString> searchLangs;

    if (!currentLang.isEmpty()) {
        // Fall A: Ordner hat bereits ein Sprach-Tag -> Nächste Sprachen rotieren
        const int currentIndex = langTags.indexOf(currentLang);
        if (currentIndex != -1) {
            for (int i = 1; i <= langTags.size(); ++i) {
                searchLangs.append(langTags[(currentIndex + i) % langTags.size()]);
            }
        }
        // Zuletzt den Ordner OHNE Tag prüfen
        searchLangs.append(QString());
    } else {
        // Fall B: Ordner hat kein Tag -> Versuche der Reihe nach [en], [de], [ch], [ko]
        searchLangs = langTags;
    }

    // 4. Zielordner suchen
    QString targetFileBasePath;

    for (const QString& lang : std::as_const(searchLangs)) {
        const QString testFolderName = lang.isEmpty()
            ? baseFolder
            : QStringLiteral("%1 [%2]").arg(baseFolder, lang);

        const QDir testDir(parentDir2.filePath(testFolderName));

        if (testDir.exists()) {
            targetFileBasePath = testDir.filePath(fileNameNoExt);
            break;
        }
    }

    // 5. Bilddatei mit passender Endung suchen
    if (!targetFileBasePath.isEmpty()) {
        static constexpr std::array<QLatin1StringView, 4> extensions = {
            QLatin1StringView(".jpg"),
            QLatin1StringView(".png"),
            QLatin1StringView(".jpeg"),
            QLatin1StringView(".bmp")
        };

        for (const auto& ext : extensions) {
            const QString candidatePath = targetFileBasePath + ext;
            if (QFileInfo::exists(candidatePath)) {
                OpenFile(candidatePath, false);
                break;
            }
        }
    }
}

void CMainDlg::OpenDefaultEditor() {
    const QString sCurrentFileName = CurrentFileName(false);
    if (sCurrentFileName.isEmpty()) {
        return;
    }

    if (m_bFullScreenMode) {
        m_bFullScreenMode = !m_bFullScreenMode;
        CRect windowRect;
        this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (::IsRectEmpty(&(m_storedWindowPlacement2.rcNormalPosition))) {
            // never set to window mode before, use default position
            windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
            this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
        } else {
            m_storedWindowPlacement2.flags = this->SetWindowPlacement(&m_storedWindowPlacement2);
        }
        this->MouseOn();

        m_dZoom = -1;
        m_bInZooming = true;
        StartLowQTimer(ZOOM_TIMEOUT);
        this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
    }

    if (m_bIsRunningInWine) {
        QString sUnixPath = GetUnixPathFromWine(sCurrentFileName);

        if (!sUnixPath.isEmpty()) {
            const QString sParams = QStringLiteral("/unix /usr/bin/gimp \"%1\"").arg(sUnixPath);
            ::ShellExecuteW(m_hWnd, nullptr, L"start.exe", reinterpret_cast<LPCWSTR>(sParams.utf16()), nullptr, SW_SHOW );
        }
    }
    else {
        ::ShellExecuteW(m_hWnd, _T("edit"), reinterpret_cast<LPCWSTR>(sCurrentFileName.utf16()), NULL, NULL, SW_SHOW);
    }
}

// Setting window styles have gotten out of hand with the addition of no title bar
// instead of each call trying to figure out the logic, consolidate it to one function
LONG CMainDlg::SetCurrentWindowStyle() {
    if (!m_bWindowBorderless) {
        return this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    } else {
        return this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) & ~WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE);  // lose resizing
        // just doing (& ~WS_CAPTION) leads to having a sliver of white bar on top but allows for resizing
    }
}

void CMainDlg::ExploreFile() {
    const QString sCurrentFileName = CurrentFileName(false);
    if (sCurrentFileName.isEmpty()) {
        return;
    }
#if defined(Q_OS_WIN)
    if (m_bIsRunningInWine) {
        const QString sUnixPath = GetUnixPathFromWine(sCurrentFileName);

        if (!sUnixPath.isEmpty()) {
            QString sParams;

            if (IsLinuxBinaryAvailable(QStringLiteral("~/.local/bin/mkFolderWidget"))) {
                sParams = QStringLiteral("/unix /bin/sh -c \"mkFolderWidget -f \\\"%1\\\"\"").arg(sUnixPath);
                ::ShellExecute(m_hWnd, nullptr, TEXT("start.exe"), reinterpret_cast<LPCWSTR>(sParams.utf16()), nullptr, SW_SHOW);
            }
            else if (IsLinuxBinaryAvailable(QStringLiteral("/usr/bin/dolphin"))) {
                sParams = QStringLiteral("/unix /usr/bin/dolphin --new-window --select \"%1\"").arg(sUnixPath);
                ::ShellExecute(m_hWnd, nullptr, TEXT("start.exe"), reinterpret_cast<LPCWSTR>(sParams.utf16()), nullptr, SW_SHOW);
            }
            else {
                // Ordnerpfad mit QFileInfo::path() ermitteln (erzielt denselben Effekt wie ReverseFind('\\') + Left)
                const QString sFolderPath = QFileInfo(sCurrentFileName).path();

                ::ShellExecute(m_hWnd, nullptr, TEXT("winebrowser.exe"), reinterpret_cast<LPCWSTR>(sFolderPath.utf16()), nullptr, SW_SHOW);
            }
        }
    } else {
        QString nativePath = QDir::toNativeSeparators(sCurrentFileName);
        QString params = QStringLiteral("/select,\"%1\"").arg(nativePath);

        ::ShellExecuteW(
            m_hWnd,
            L"open",
            L"explorer.exe",
            reinterpret_cast<LPCWSTR>(params.utf16()),
            nullptr,
            SW_SHOW
            );
    }
#elif defined(Q_OS_LINUX)
    // --- Nativer Linux-Build (ohne Wine / reines Qt6) ---
    // Hier wird direkt auf Linux-Befehle oder QDesktopServices zurückgegriffen
    if (QFileInfo(sCurrentFileName).exists()) {
        // Beispiel für nativen Linux-Dateimanager (z. B. Dolphin mit --select)
        QStringList args;
        args << QStringLiteral("--select") << sCurrentFileName;
        QProcess::startDetached(QStringLiteral("dolphin"), args);
    }
#endif
}

void CMainDlg::OpenFile(const QString& fileName, bool bAfterStartup) {
    StopMovieMode();
    StopAnimation();

    // [GF] Save Bookmark of currently open file
    SaveBookmark();
    
    // recreate file list based on image opened
    Helpers::ESorting eOldSorting = m_pFileList->GetSorting();
    bool bOldAscending = m_pFileList->IsSortedAscending();
    delete m_pFileList;

    // [GF] Recognize Book type sources & load bookmark and adjust zoom mode accordingly.
    int nFrameIndex = 0;
    if (!fileName.isEmpty()) {
        if (IsBookModeFile(fileName)) {
            m_eAutoZoomModeFullscreen = Helpers::ZM_BookMode;
            m_eAutoZoomModeWindowed = Helpers::ZM_BookMode;

            const QString sBookmark = LoadBookmark(fileName);
            if (!sBookmark.isEmpty()) {
                bool isNumeric = false;
                // Wandelt den String in ein int um und prüft gleichzeitig, ob er ausschließlich eine Zahl ist
                const int frameNum = sBookmark.toInt(&isNumeric);

                if (isNumeric) {
                    // Bookmark is a pure number, so wasn't derived from a file name but from a container's FrameIndex()
                    nFrameIndex = frameNum - 1;
                } else {
                    // Bookmark point to an image file. Check if exits and ask the user if the launch that instead.
                    const QString parentDir = QFileInfo(fileName).path();
                    const QString sBookmarkPath = QDir(parentDir).filePath(sBookmark);

                    if (QFileInfo::exists(sBookmarkPath)) {
                        //sFileName = sBookmarkPath;
                    }
                }
            }
        } else {
            CSettingsProvider& sp = CSettingsProvider::This();
            m_eAutoZoomModeWindowed = sp.AutoZoomMode();
            m_eAutoZoomModeFullscreen = sp.AutoZoomModeFullscreen();
        }
    }

    m_sStartupFile = fileName;

    m_pFileList = new CFileList(m_sStartupFile,
                                *m_pDirectoryWatcher,
                                eOldSorting,
                                bOldAscending,
                                CSettingsProvider::This().WrapAroundFolder());

    // free current image and all read ahead images
    InitParametersForNewImage();
    m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
    m_pJPEGProvider->ClearAllRequests();

    m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList,
                                                    CJPEGProvider::FORWARD,
                                                    m_pFileList->Current(),
                                                    nFrameIndex,
                                                    CreateProcessParams(!m_bFullScreenMode && (bAfterStartup || IsAdjustWindowToImage())),
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    m_nLastLoadError = GetLoadErrorAfterOpenFile();
    if (bAfterStartup) CheckIfApplyAutoFitWndToImage(false);
    AfterNewImageLoaded(true, false);
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        StartAnimation();
    }
    m_startMouse.x = m_startMouse.y = -1;
    MouseOff();
    this->Invalidate(FALSE);
}

void CMainDlg::StartDragging(int nX, int nY, bool bDragWithZoomNavigator) {
    m_startMouse.x = m_startMouse.y = -1;
    m_bDragging = true;
    m_nCapturedX = nX;
    m_nCapturedY = nY;
    SetCursorForMoveSection();
}

void CMainDlg::DoDragging() {
    if (m_bDragging && m_pCurrentImage != NULL) {
        int nXDelta = m_nMouseX - m_nCapturedX;
        int nYDelta = m_nMouseY - m_nCapturedY;
        if (!m_bDoDragging && (nXDelta != 0 || nYDelta != 0)) {
            m_bDoDragging = true;
        }

        if (PerformPan(nXDelta, nYDelta, false)) {
            m_nCapturedX = m_nMouseX;
            m_nCapturedY = m_nMouseY;
        }
    }
}

void CMainDlg::EndDragging() {
    m_bDragging = false;
    m_bDoDragging = false;
    if (m_pCurrentImage != NULL) {
        m_pCurrentImage->VerifyDIBPixelsCreated();
    }
    if (m_DIBOffsets != CPoint(0, 0)) {
        Invalidate(FALSE);
    }
    SetCursorForMoveSection();
}

void CMainDlg::GotoImage(EImagePosition ePos) {
    GotoImage(ePos, 0);
}

void CMainDlg::GotoImage(EImagePosition ePos, int nFlags) {
//* Debugging */    TCHAR debugtext[1024];
//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos1"), ePos, nFlags);
//* Debugging */    ::OutputDebugStringW(debugtext);

    // Timer handling for slideshows
    if (ePos == POS_Next || ePos == POS_NextSlideShow) {
        if (m_nCurrentTimeout > 0) {
            StartSlideShowTimer(m_nCurrentTimeout);
        }
        StopAnimation();
        m_customAnimationFrameTime = 1000;

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos1a: (ePos == POS_Next || ePos == POS_NextSlideShow)"), ePos, nFlags);
//* Debugging */    ::OutputDebugStringW(debugtext);

    } else if (ePos != POS_NextFrame) {
        StopMovieMode();
        StopAnimation();
        m_customAnimationFrameTime = 1000;

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos1b: (ePos != POS_NextFrame)"), ePos, nFlags);
//* Debugging */    ::OutputDebugStringW(debugtext);
    }

    // solving a specific "edge" case, pun intended
    // (doesn't happen with first image, because we would be at frame 0 already)
    // if wraparound = false,
    // if we are at the last image,
    // if we're at the last index of a multi-framed image (not animation)
    // don't wrap around to index 0
    bool bNoWrapAroundEdgeFrame = !CSettingsProvider::This().WrapAroundFolder() &&
        (m_pCurrentImage == NULL ? false : m_pCurrentImage->NumberOfFrames() > 1 && !m_pCurrentImage->IsAnimation());

    int nFrameIndex = 0;
    bool bCheckIfSameImage = true;
    m_pFileList->SetCheckpoint();
    CFileList* pOldFileList = m_pFileList;
    int nOldFrameIndex = (m_pCurrentImage == NULL) ? 0 : m_pCurrentImage->FrameIndex();
    CJPEGProvider::EReadAheadDirection eDirection = CJPEGProvider::FORWARD;

    switch (ePos) {
        case POS_First:
            if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                nFrameIndex = 0;
            } else {
                m_pFileList->First();
            }
            break;
        case POS_Last:
            if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                nFrameIndex = m_pCurrentImage->NumberOfFrames() - 1;
            } else {
                m_pFileList->Last();
            }
            break;
        case POS_Next:
        case POS_NextFrame:
            {
                bool bGotoNextImage = true;
                nFrameIndex = Helpers::GetFrameIndex(m_pCurrentImage, +1, ePos == POS_NextFrame, bGotoNextImage);
                if (bGotoNextImage)
                    m_pFileList = m_pFileList->Next();
                else
                    bNoWrapAroundEdgeFrame = false; // the "next" operation didn't request going to the next image (next frame index is not current one)
                break;
            }
        case POS_NextFile:
        case POS_NextSlideShow:
            {
                if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                    SaveBookmark();
                }

                m_pFileList = m_pFileList->Next();

                const QString sFileName = m_pFileList->Current();
                if ((!sFileName.isEmpty()) && IsBookModeFile(sFileName)) {
                    const QString sBookmark = LoadBookmark(sFileName);
                    if (!sBookmark.isEmpty()) {
                        bool isNumeric = false;
                        // Wandelt den String in ein int um und prüft gleichzeitig, ob er ausschließlich eine Zahl ist
                        const int frameNum = sBookmark.toInt(&isNumeric);

                        if (isNumeric) {
                            // Bookmark ist eine reine Zahl (FrameIndex)
                            nFrameIndex = frameNum - 1;
                        }
                    }
                }
            }
            break;
        case POS_PreviousFile:
            {
                if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                    SaveBookmark();
                }
                
                m_pFileList = m_pFileList->Prev();
                eDirection = CJPEGProvider::BACKWARD;

                const QString sFileName = m_pFileList->Current();
                if ((!sFileName.isEmpty()) && IsBookModeFile(sFileName)) {
                    const QString sBookmark = LoadBookmark(sFileName);
                    if (!sBookmark.isEmpty()) {
                        bool isNumeric = false;
                        // Wandelt den String in ein int um und prüft gleichzeitig, ob er ausschließlich eine Zahl ist
                        const int frameNum = sBookmark.toInt(&isNumeric);

                        if (isNumeric) {
                            // Bookmark ist eine reine Zahl (FrameIndex)
                            nFrameIndex = frameNum - 1;
                        }
                    }
                }
            }
            break;
        case POS_Previous:
            {
                bool bGotoPrevImage;
                nFrameIndex = Helpers::GetFrameIndex(m_pCurrentImage, -1, false, bGotoPrevImage);
                if (bGotoPrevImage) m_pFileList = m_pFileList->Prev();
                eDirection = CJPEGProvider::BACKWARD;
                bNoWrapAroundEdgeFrame = false; // previous image at edge is not a wraparound
                break;
            }
        case POS_Current:
            if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                nFrameIndex = m_pCurrentImage->FrameIndex();
            }
            bCheckIfSameImage = false; // do something even when not moving iterator on filelist
            break;
        case POS_AwayFromCurrent:
            m_pFileList = m_pFileList->AwayFromCurrent();
            break;
    }

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos4  m_offsets.x=%d  m_offsets.y=%d  nOldFrameIndex=%d  nFrameIndex=%d"), ePos, nFlags, m_offsets.x, m_offsets.y, nOldFrameIndex, nFrameIndex);
//* Debugging */    ::OutputDebugStringW(debugtext);

    if (bCheckIfSameImage && (m_pFileList == pOldFileList && (nOldFrameIndex == nFrameIndex || bNoWrapAroundEdgeFrame) && !m_pFileList->ChangedSinceCheckpoint())) {
        if (m_bMovieMode && m_bAutoExit) {
            CleanupAndTerminate();
        } else {
//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos4b  bCheckIfSameImage=%d  bNoWrapAroundEdgeFrame=%d  m_pFileList->ChangedSinceCheckpoint()=%d"), ePos, nFlags, bCheckIfSameImage, bNoWrapAroundEdgeFrame, m_pFileList->ChangedSinceCheckpoint());
//* Debugging */    ::OutputDebugStringW(debugtext);
            return; // not placed on a new image, don't do anything
        }
    }

    if (ePos != POS_Current && ePos != POS_NextFrame && ePos != POS_AwayFromCurrent) {
        MouseOff();
    }

    if (nFlags & KEEP_PARAMETERS) {
        if (!(m_bUserZoom || IsAdjustWindowToImage())) {
            m_dZoom = -1;
        }
    } else {
        InitParametersForNewImage();
    }

    m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
    // Clear from cache only for POS_Current POS_AwayFromCurrent, which are involved in reloading on file/directory changes and deletion.
    if (ePos == POS_Current || ePos == POS_AwayFromCurrent) {
        // When (ePos == POS_AwayFromCurrent), it will additionally releaseLockedFile in m_pWorkThreads
        m_pJPEGProvider->ClearRequest(m_pCurrentImage, ePos == POS_AwayFromCurrent);
    }
    m_pCurrentImage = NULL;

    // do not perform a new image request if flagged
    if (nFlags & NO_REQUEST) {
        return;
    }

    CProcessParams procParams = CreateProcessParams(false);
    if (nFlags & KEEP_PARAMETERS) {
        procParams.ProcFlags = SetProcessingFlag(procParams.ProcFlags, PFLAG_KeepParams, true);
    }

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage()->RequestImage()  nFrameIndex=%d of %s"), nFrameIndex, m_pFileList->Current());
//* Debugging */    ::OutputDebugStringW(debugtext);

    m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList,
                                                    (ePos == POS_AwayFromCurrent) ? CJPEGProvider::NONE : eDirection,
                                                    m_pFileList->Current(),
                                                    nFrameIndex,
                                                    procParams,
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    m_nLastLoadError = (m_pCurrentImage == NULL) ? ((m_pFileList->Current().isEmpty()) ? FileLoad_NoFilesInDirectory : FileLoad_LoadError) : FileLoad_Ok;

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos6  m_offsets.x=%d  m_offsets.y=%d  nOldFrameIndex=%d  nFrameIndex=%d of %s"), ePos, nFlags, m_offsets.x, m_offsets.y, nOldFrameIndex, nFrameIndex, m_pFileList->Current());
//* Debugging */    ::OutputDebugStringW(debugtext);

    bool bSynchronize = (nFlags & KEEP_PARAMETERS) == 0;

    AfterNewImageLoaded(bSynchronize, false, ePos == POS_Previous);

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos7  m_offsets.x=%d  m_offsets.y=%d  nOldFrameIndex=%d  nFrameIndex=%d of %s"), ePos, nFlags, m_offsets.x, m_offsets.y, nOldFrameIndex, nFrameIndex, m_pFileList->Current());
//* Debugging */    ::OutputDebugStringW(debugtext);

    // If current image is animated, start playback
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        if (m_bIsAnimationPlaying) {
            AdjustAnimationFrameTime();
        } else {
            StartAnimation();
        }
    }

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage(ePos=%d, nFlags=%d) Pos8  m_offsets.x=%d  m_offsets.y=%d"), ePos, nFlags, m_offsets.x, m_offsets.y);
//* Debugging */    ::OutputDebugStringW(debugtext);

    if (((nFlags & NO_UPDATE_WINDOW) == 0) && !(ePos == POS_NextSlideShow && UseSlideShowTransitionEffect())) {
        this->Invalidate(FALSE);
        // this will force to wait until really redrawn, preventing to process images but do not show them
        this->UpdateWindow();
    }

//* Debugging */    swprintf(debugtext,1024,TEXT("GotoImage() after UpdateWindow()  nFrameIndex=%d of %s"), nFrameIndex, m_pFileList->Current());
//* Debugging */    ::OutputDebugStringW(debugtext);

    // remove key messages accumulated so far
    if (!(nFlags & NO_REMOVE_KEY_MSG)) {
        MSG msg;
        while (::PeekMessage(&msg, this->m_hWnd, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
    }
}

void CMainDlg::ReloadImage(bool keepParameters, bool updateWindow) {
    int flags = keepParameters ? KEEP_PARAMETERS : 0;
    GotoImage(CMainDlg::POS_Current, updateWindow ? flags : flags | NO_UPDATE_WINDOW);
}

// returns true on success, false if nothing was done
bool CMainDlg::PerformZoom(double dValue, bool bExponent, bool bZoomToMouse, bool bAdjustWindowToImage) {
    double dOldZoom = m_dZoom;
    m_bUserZoom = true;
    m_isUserFitToScreen = false;
    if (bExponent) {
        m_dZoom = m_dZoom * pow(m_dZoomMult, dValue);
    } else {
        m_dZoom = dValue;
    }

    if (m_pCurrentImage == NULL) {
        m_dZoom = std::max(Helpers::ZoomMin, std::min(Helpers::ZoomMax, m_dZoom));
        return true;
    }

    double dZoomMin = std::max(0.0001, std::min(Helpers::ZoomMin, GetZoomFactorForFitToScreen(false, false) * 0.5));
    m_dZoom = std::max(dZoomMin, std::min(Helpers::ZoomMax, m_dZoom));

    if (MaxRatio(m_dZoom, 1.0) < 1.05) { // Special case 1: Image zoomed size within 5% of image real size -> Use real size
        m_dZoom = 1.0;
    } else { // Special case 2: Image zoomed size close to ZoomToWindow size -> Use ZoomToWindow size
        double dZoomWindow = GetZoomFactorForFitToScreen(false, false);
        if (MaxRatio(m_dZoom, dZoomWindow) < 1.05) {
            m_dZoom = dZoomWindow;
        }
    }

    // only pause on some percent if enabled
    double pauseAtZoom = CSettingsProvider::This().ZoomPauseFactor();
    if (pauseAtZoom != 0) {
        // snap to zoom factor... aka if within 1% of the set zoom factor, snap exactly to it
        // skip it if the zoom factor is 100% since it's already checked above - save one expensive calculation of abs()
        if (pauseAtZoom != 1 && (MaxRatio(m_dZoom, pauseAtZoom) < 1.01)) {
            m_dZoom = pauseAtZoom;
        }

        if ((dOldZoom - pauseAtZoom) * (m_dZoom - pauseAtZoom) <= 0 && m_bInZooming && !m_bZoomMode) {
            // make a stop at 100 % (or whatever % is configured)
            m_dZoom = pauseAtZoom;
        }
    }

    // Never create images more than 65535 pixels wide or high - the basic processing cannot handle it
    int nOldXSize = (int)(m_pCurrentImage->OrigWidth() * dOldZoom + 0.5);
    int nOldYSize = (int)(m_pCurrentImage->OrigHeight() * dOldZoom + 0.5);
    int nNewXSize = (int)(m_pCurrentImage->OrigWidth() * m_dZoom + 0.5);
    int nNewYSize = (int)(m_pCurrentImage->OrigHeight() * m_dZoom + 0.5);
    if (nNewXSize > 65535 || nNewYSize > 65535) {
        double dFac = 65535.0/std::max(nNewXSize, nNewYSize);
        m_dZoom = m_dZoom*dFac;
        nNewXSize = (int)(m_pCurrentImage->OrigWidth() * m_dZoom + 0.5);
        nNewYSize = (int)(m_pCurrentImage->OrigHeight() * m_dZoom + 0.5);
    }

    // because we've increased/decreased to the maximum zoom allowed,
    // only actually perform the zoom if the old and new dimensions differ
    // the float arithmetic doesn't always come up with the same answer, but the new sizes can be directly compared
    if (nNewXSize == nOldXSize && nNewYSize == nOldYSize) {
        // because there's rounding errors, it is possible to get stuck zooming out from fractional zoom,
        // due to the new/old size being the same, but the zoom factor still changing
        // hence, only reset the zoom value IF when zooming out (aka, it is getting smaller)
        //
        // when zooming in, do not set the old zoom value so that it can "escape" the initial few steps where
        // the zoom ratio has changed, but the size is still the same, due to rounding errors
        if (bExponent && dValue < 0) {
            // zooming out
            // NOTE: this gets triggered on pauseAtZoom
            m_dZoom = dOldZoom;  // restore previous zoom value
        }
        // zooming in does not require restoring the previous zoom value
        // because the code which checks the 65535 will re-scale the zoom factor to ensure it never exceeds 65535

        return false; // then do nothing
    }


    if (bZoomToMouse) {
        // zoom to mouse
        int nCenterX = m_bZoomMode ? m_nCapturedX : m_nMouseX;
        int nCenterY = m_bZoomMode ? m_nCapturedY : m_nMouseY;
        int nOldX = nOldXSize/2 - m_clientRect.Width()/2 + nCenterX - m_offsets.x;
        int nOldY = nOldYSize/2 - m_clientRect.Height()/2 + nCenterY - m_offsets.y;
        double dFac = m_dZoom/dOldZoom;
        m_offsets.x = Helpers::RoundToInt(nNewXSize/2 - m_clientRect.Width()/2 + nCenterX - nOldX*dFac);
        m_offsets.y = Helpers::RoundToInt(nNewYSize/2 - m_clientRect.Height()/2 + nCenterY - nOldY*dFac);
    } else {
        // zoom to center
        m_offsets.x = Helpers::RoundToInt(m_offsets.x*m_dZoom/dOldZoom);
        m_offsets.y = Helpers::RoundToInt(m_offsets.y*m_dZoom/dOldZoom);
    }

    m_bInZooming = true;
    StartLowQTimer(ZOOM_TIMEOUT);
    if (fabs(dOldZoom - m_dZoom) > 0.0001 || m_bZoomMode) {
        this->Invalidate(FALSE);
        if (bAdjustWindowToImage) {
            AdjustWindowToImage(false);
        }

        // Recalculate the new m_nBookModePageHeight based on the current zoom m_dZoom
        // Todo: Save changes to user ini
        if ((GetAutoZoomMode() == Helpers::ZM_BookMode) && (m_bZoomMode == false)) {
            int nImageW = m_pCurrentImage->OrigWidth();
            int nImageH = m_pCurrentImage->OrigHeight();
            int nWinW = m_clientRect.Width();
            int nWinH = m_clientRect.Height();

            double dImageAR = (double)nImageW/nImageH;


            if (dImageAR < 1.0) {        // single page
                if (nImageW * m_dZoom > nWinW) {        // wider than window
                    m_nBookModePageHeight = 9999;
                } else if (nImageH * m_dZoom < nWinH) { // shorter then window height -> limit to 100
                    m_nBookModePageHeight = 100;
                } else {
                    m_nBookModePageHeight = (int)((nImageH * m_dZoom * 100) / nWinH);
                }

                CSettingsProvider::This().SetBookModePageHeight(m_nBookModePageHeight);
            }
        }
    }
    return true;
}

bool CMainDlg::PerformPan(int dx, int dy, bool bAbsolute) {
    if ((m_virtualImageSize.cx > 0 && m_virtualImageSize.cx > m_clientRect.Width()) ||
        (m_virtualImageSize.cy > 0 && m_virtualImageSize.cy > m_clientRect.Height())) {
        if (bAbsolute) {
            m_offsets = CPoint(dx, dy);
        } else {
            m_offsets = CPoint(m_offsets.x + dx, m_offsets.y + dy);
        }
        m_bUserPan = true;

        this->Invalidate(FALSE);
        return true;
    }
    return false;
}

double CMainDlg::GetZoomFactorForFitToScreen(bool bFillWithCrop, bool bAllowEnlarge) {
    if (m_pCurrentImage != NULL) {
        double dZoom;
        Helpers::GetImageRect(m_pCurrentImage->OrigWidth(), m_pCurrentImage->OrigHeight(), 
            m_clientRect.Width(), m_clientRect.Height(), bAllowEnlarge, bFillWithCrop, false, GetAutoZoomMode(), dZoom);
        double dZoomMax = bAllowEnlarge ? Helpers::ZoomMax : std::max(1.0, m_dZoomAtResizeStart);
        return std::max(0.0001, std::min(dZoomMax, dZoom));
    } else {
        return 1.0;
    }
}

void CMainDlg::ResetZoomToFitScreen(bool bFillWithCrop, bool bAllowEnlarge, bool bAdjustWindowSize) {
    m_isUserFitToScreen = false;
    if (m_pCurrentImage != NULL) {
        if (bAdjustWindowSize && !m_bFullScreenMode && !IsZoomed() && m_bAutoFitWndToImage) {
            m_dZoom = bAllowEnlarge ? Helpers::ZoomMax : 1;
            CRect wndRect = GetWindowRectMatchingImageSize(m_hWnd, CSize(MIN_WND_WIDTH, MIN_WND_HEIGHT), HUGE_SIZE, m_dZoom, m_pCurrentImage, false, true, m_bWindowBorderless, GetAutoZoomMode());
            if (m_dZoom <= 1) {
                m_dZoom = -1;
            }
            m_bResizeForNewImage = true;
            this->SetWindowPos(HWND_TOP, wndRect.left, wndRect.top, wndRect.Width(), wndRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
            this->GetClientRect(&m_clientRect);
            m_bResizeForNewImage = false;
            this->Invalidate(FALSE);
        } else {
            double dOldZoom = m_dZoom;
            m_dZoom = GetZoomFactorForFitToScreen(bFillWithCrop, bAllowEnlarge);
            if (bAllowEnlarge && !IsAdjustWindowToImage()) {
                m_isUserFitToScreen = true;
                m_dZoom = -1;
                m_autoZoomFitToScreen = bFillWithCrop ? Helpers::ZM_FillScreen : Helpers::ZM_FitToScreen;
            }
            m_bUserZoom = m_dZoom > 1.0;
            m_bUserPan = false;
            if (fabs(dOldZoom - m_dZoom) > 0.01) {
                //m_bInZooming = true;    // Disabled, since this would trigger a display of the Zoom Factor text at ANY change in zoom (fullscreen, onsize() etc.)
                StartLowQTimer(ZOOM_TIMEOUT);
                this->Invalidate(FALSE);
            }
        }
    }
}

void CMainDlg::ResetZoomTo100Percents(bool bZoomToMouse) {
    if (m_pCurrentImage != NULL && fabs(m_dZoom - 1) > 0.01) {
        // the current design (unless changed) cursor always shows in windowed mode, so always zoom to cursor when not fullscreen
        PerformZoom(1.0, false, bZoomToMouse || !m_bFullScreenMode, true);
    }
}

CProcessParams CMainDlg::CreateProcessParams(bool bNoProcessingAfterLoad) {
    int nClientWidth = m_clientRect.Width();
    int nClientHeight = m_clientRect.Height();
    if (nClientWidth == 0 && nClientHeight == 0) {
        CRect rect = CMultiMonitorSupport::GetMonitorRect(m_hWnd);
        nClientWidth = rect.Width();
        nClientHeight = rect.Height();
    }
    Helpers::EAutoZoomMode eAutoZoomMode = GetAutoZoomMode();
    if (IsAdjustWindowToImage() && !bNoProcessingAfterLoad) {
        CSize maxClientSize = GetMaxClientSize(m_hWnd);
        nClientWidth = maxClientSize.cx;
        nClientHeight = maxClientSize.cy;
        eAutoZoomMode = Helpers::ZM_FitToScreenNoZoom;
    }
    if (m_bKeepParams) {
        *m_pImageProcParamsKept = *m_pImageProcParams;
        m_eProcessingFlagsKept = CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, m_bKeepParams);
        m_dZoomKept = (m_bUserZoom || IsAdjustWindowToImage()) ? m_dZoom : -1;
        m_offsetKept = m_bUserPan ? m_offsets : CPoint(0, 0);
        if (m_isUserFitToScreen && !IsAdjustWindowToImage()) { eAutoZoomMode = m_autoZoomFitToScreen; }

        return CProcessParams(nClientWidth, nClientHeight,
            CMultiMonitorSupport::GetMonitorRect(m_hWnd).Size(),
            CRotationParams(0),
            m_nUserRotation,
            m_dZoomKept,
            eAutoZoomMode,
            m_offsetKept,
            m_eTransparencyMode,
            *m_pImageProcParamsKept, 
            SetProcessingFlag(m_eProcessingFlagsKept, PFLAG_NoProcessingAfterLoad, bNoProcessingAfterLoad));
    } else {
        m_isUserFitToScreen = false;
        CSettingsProvider& sp = CSettingsProvider::This();
        return CProcessParams(nClientWidth, nClientHeight, 
            CMultiMonitorSupport::GetMonitorRect(m_hWnd).Size(),
            CRotationParams(0), 0, -1, eAutoZoomMode, CPoint(0, 0), m_eTransparencyMode,
            GetDefaultProcessingParams(),
            SetProcessingFlag(GetDefaultProcessingFlags(), PFLAG_NoProcessingAfterLoad, bNoProcessingAfterLoad));
    }
}

void CMainDlg::ResetParamsToDefault() {
    CSettingsProvider& sp = CSettingsProvider::This();
    m_nRotation = 0;
    m_nUserRotation = 0;
    m_dZoom = m_dZoomMult = -1.0;
    m_offsets = CPoint(0, 0);
    m_bUserZoom = false;
    m_bUserPan = false;
    *m_pImageProcParams = GetDefaultProcessingParams();
    // [GF] Do not reset these back to their INI defaults. If the user has intentionally changed these in the current instance, these values should be respected and used instead.
    //InitFromProcessingFlags(GetDefaultProcessingFlags(), m_bHQResampling, m_bAutoContrast);
}

void CMainDlg::StartSlideShowTimer(int nMilliSeconds) {
    m_nCurrentTimeout = nMilliSeconds;
    ::SetTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID, nMilliSeconds, NULL);
    m_nLastSlideShowImageTickCount = ::GetTickCount();
}

void CMainDlg::StopSlideShowTimer(void) {
    if (m_nCurrentTimeout > 0) {
        m_nCurrentTimeout = 0;
        ::KillTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID);
    }
}

void CMainDlg::StartMovieMode(double dFPS) {
    // if more than this number of frames are requested per seconds, it is considered to be a movie
    const double cdFPSMovie = 4.9;

    m_dMovieFPS = dFPS;

    // Save processing flags at the time movie mode starts
    if (!m_bMovieMode) {
        m_eProcFlagsBeforeMovie = CreateDefaultProcessingFlags(m_bKeepParams);
    }
    // Keep parameters during movie mode
    if (!m_bKeepParams && dFPS > cdFPSMovie) {
        ExecuteCommand(IDM_KEEP_PARAMETERS);
    }
    // Turn off high quality resamping and auto corrections when requested to play many frames per second
    if (dFPS > cdFPSMovie) {
        m_bProcFlagsTouched = true;
        m_bHQResampling = false;
        m_bAutoContrast = false;
    }
    m_bMovieMode = true;
    StartSlideShowTimer(Helpers::RoundToInt(1000.0/dFPS));
    AfterNewImageLoaded(false, false);
    Invalidate(FALSE);
}

void CMainDlg::StopMovieMode() {
    if (m_bMovieMode) {
        // undo changes done on processing parameters due to movie mode
        if (!GetProcessingFlag(m_eProcFlagsBeforeMovie, PFLAG_KeepParams)) {
            if (m_bKeepParams) {
                ExecuteCommand(IDM_KEEP_PARAMETERS);
            }
        }
        if (m_bProcFlagsTouched) {
            if (GetProcessingFlag(m_eProcFlagsBeforeMovie, PFLAG_AutoContrast)) {
                m_bAutoContrast = true;
            }
            if (GetProcessingFlag(m_eProcFlagsBeforeMovie, PFLAG_HighQualityResampling)) {
                m_bHQResampling = true;
            }
        }
        m_bMovieMode = false;
        m_bProcFlagsTouched = false;
        StopSlideShowTimer();
        AfterNewImageLoaded(false, false);
        this->Invalidate(FALSE);
    }
}

void CMainDlg::StartLowQTimer(int nTimeout) {
    m_bTemporaryLowQ = true;
    ::KillTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID);
    ::SetTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID, nTimeout, NULL);
}

void CMainDlg::MouseOff() {
    if (m_bMouseOn) {
        if (m_nMouseY < m_clientRect.bottom && !m_bInTrackPopupMenu) {
            if (m_bFullScreenMode) {
                // cursor only hides when in full screen mode
                while (::ShowCursor(FALSE) >= 0);
            }
            m_startMouse.x = m_startMouse.y = -1;
            m_bMouseOn = false;
        }
    }
}

void CMainDlg::MouseOn() {
    if (!m_bMouseOn) {
        ::ShowCursor(TRUE);
        m_bMouseOn = true;
    }
}

void CMainDlg::InitParametersForNewImage() {
    if (!m_bKeepParams) {
        ResetParamsToDefault();
    } else if (!(m_bUserZoom || IsAdjustWindowToImage())) {
        m_dZoom = -1;
    }

    *m_pImageProcParams2 = GetNoProcessingParams();
    m_eProcessingFlags2 = PFLAG_HighQualityResampling;

    if (!m_bIsAnimationPlaying) {
        m_bInZooming = m_bTemporaryLowQ = m_bShowZoomFactor = false;
    }
}

void CMainDlg::ExchangeProcessingParams() {
    CImageProcessingParams tempParams = *m_pImageProcParams;
    EProcessingFlags tempFlags = CreateDefaultProcessingFlags();
    *m_pImageProcParams = *m_pImageProcParams2;
    InitFromProcessingFlags(m_eProcessingFlags2, m_bHQResampling, m_bAutoContrast);
    *m_pImageProcParams2 = tempParams;
    m_eProcessingFlags2 = tempFlags;
    this->Invalidate(FALSE);
}

void CMainDlg::AfterNewImageLoaded(bool bSynchronize, bool bAfterStartup, bool bMoveBack) {
    UpdateWindowTitle();
    m_pDirectoryWatcher->SetCurrentFile(CurrentFileName(false));
    if (bSynchronize) {
        // after loading an image, the per image processing parameters must be synchronized with
        // the current processing parameters
        bool bLastWasInParamDB = m_bCurrentImageInParamDB;
        m_bCurrentImageInParamDB = false;

        if (m_pCurrentImage != NULL) {
            m_bCurrentImageInParamDB = m_pCurrentImage->IsInParamDB();

            if (!m_bKeepParams) {
                // [GF] Do not restore these to how they were when the image was loaded before. If the user has intentionally changed these settings since that time, the current settings should be respected.
                //m_bHQResampling = GetProcessingFlag(m_pCurrentImage->GetInitialProcessFlags(), PFLAG_HighQualityResampling);
                //m_bAutoContrast = GetProcessingFlag(m_pCurrentImage->GetInitialProcessFlags(), PFLAG_AutoContrast);

                m_nRotation = m_pCurrentImage->GetInitialRotation();
                m_dZoom = m_pCurrentImage->GetInititialZoom();
                m_offsets = m_pCurrentImage->GetInitialOffsets();

                if (m_pCurrentImage->HasZoomStoredInParamDB()) {
                    m_bUserZoom = m_bUserPan = true;
                }

                *m_pImageProcParams = m_pCurrentImage->GetInitialProcessParams();
            }

            if (m_bKeepParams) {
                m_nRotation = m_pCurrentImage->GetInitialRotation() + m_nUserRotation;
            }

            // [GF] Adjust m_offsets for BookMode
            if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                if (bMoveBack == false)
                    m_offsets = CPoint(-65000,65000);    // Book page RTL manga mode: focus top right (RTL). TODO: implement LTR option. (Not really needed - in BookMode, the page is never wider than window.)
                else
                    m_offsets = CPoint(65000,-65000);    // Book page RTL mode: focus bottom left (RTL). TODO: implement LTR option. (Not really needed - in BookMode, the page is never wider than window.)
    
                m_offsets_custom = m_offsets;
            }
        }

        if (!bAfterStartup && !m_bIsAnimationPlaying) {
            AdjustWindowToImage(false);
        }
    }
}

bool CMainDlg::IsAdjustWindowToImage() {
    return !m_bFullScreenMode && !::IsZoomed(m_hWnd) && m_bAutoFitWndToImage;
}

bool CMainDlg::IsImageExactlyFittingWindow() {
    return (m_pCurrentImage != NULL) && m_pCurrentImage->DIBWidth() == m_clientRect.Width() && m_pCurrentImage->DIBHeight() == m_clientRect.Height();
}

void CMainDlg::AdjustWindowToImage(bool bAfterStartup) {
    if (IsAdjustWindowToImage() && (m_pCurrentImage != NULL || bAfterStartup)) {
        // window size shall be adjusted to image size (at least keep aspect ratio)
        double dZoom = m_dZoom;
        CRect windowRect = GetWindowRectMatchingImageSize(m_hWnd, CSize(MIN_WND_WIDTH, MIN_WND_HEIGHT), HUGE_SIZE, dZoom, m_pCurrentImage, bAfterStartup, dZoom < 0, m_bWindowBorderless , GetAutoZoomMode());
        CRect defaultRect = CMultiMonitorSupport::GetDefaultWindowRect();
        if (bAfterStartup && CSettingsProvider::This().ExplicitWindowRect()) {
            windowRect = CRect(defaultRect.TopLeft(), windowRect.Size());
        }
        m_bResizeForNewImage = true;
        this->Invalidate(FALSE);
        this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
        this->GetClientRect(&m_clientRect);
        m_bResizeForNewImage = false;
    }
}

void CMainDlg::CheckIfApplyAutoFitWndToImage(bool bInInitDialog) {
    if (CSettingsProvider::This().AutoFullScreen()) {
        if (!CanDisplayImageWithoutResize(m_hWnd, m_pCurrentImage)) {
            if (!bInInitDialog) {
                ExecuteCommand(IDM_FULL_SCREEN_MODE);
            }
            m_bFullScreenMode = true;
            m_bTemporaryLowQ = false;
            MouseOff();
        } else {
            m_bAutoFitWndToImage = true;
        }
    }
}

void CMainDlg::SaveParameters() {
    if (m_bMovieMode) {
        return;
    }

    EProcessingFlags eFlags = CreateDefaultProcessingFlags(m_bKeepParams);
    CSettingsProvider::This().SaveSettings(*m_pImageProcParams,
                                           eFlags,
                                           m_pFileList->GetNavigationMode(),
                                           m_pFileList->GetSorting(),
                                           m_pFileList->IsSortedAscending(),
                                           m_eAutoZoomModeWindowed,
                                           m_eAutoZoomModeFullscreen,
                                           m_eTransitionEffect);
}

// Get current filename or filepath
QString CMainDlg::CurrentFileName(bool bFileNameOnly) {
    if (m_pFileList != nullptr) {
        return bFileNameOnly ? m_pFileList->CurrentFileTitle() : m_pFileList->Current();
    } else {
        return QString();
    }
}

void CMainDlg::SetCursorForMoveSection() {
    if (m_bDragging) {
        ::SetCursor(::LoadCursor(NULL, IDC_SIZEALL));
        m_bPanMouseCursorSet = true;
    } else if (m_bPanMouseCursorSet) {
        ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
        m_bPanMouseCursorSet = false;
    }
}

void CMainDlg::ToggleMonitor() {
    if (CMultiMonitorSupport::IsMultiMonitorSystem() && !m_bSpanVirtualDesktop) {
        int nMaxMonitorIdx = ::GetSystemMetrics(SM_CMONITORS);
        if (m_nMonitor == -1) {
            // index unknown, search
            for (int i = 0; i < nMaxMonitorIdx; i++) {
                if (m_monitorRect == CMultiMonitorSupport::GetMonitorRect(i)) {
                    m_nMonitor = i;
                    break;
                }
            }
        }
        m_dZoom = -1.0;
        this->Invalidate(FALSE);
        m_nMonitor = (m_nMonitor + 1) % nMaxMonitorIdx;
        m_monitorRect = CMultiMonitorSupport::GetMonitorRect(m_nMonitor);
        SetWindowPos(HWND_TOP, &m_monitorRect, SWP_NOZORDER);
        this->GetClientRect(&m_clientRect);
    }
}

CRect CMainDlg::GetZoomTextRect() {
    int nEndX = m_clientRect.right - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_OFFSET_X);
    int nStartX = nEndX - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_WIDTH);
    
    int nEndY = m_clientRect.bottom - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_OFFSET_Y);
    int nStartY = nEndY - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_HEIGHT);
    
    return CRect(nStartX, nStartY, nEndX, nEndY);
}

CRect CMainDlg::GetToastTextRect() {
    int nEndX = m_clientRect.right - Helpers::ScaleWithDPI(TOAST_TEXT_RECT_OFFSET_X);
    int nStartX = m_clientRect.right/2;
    
    int nStartY = Helpers::ScaleWithDPI(TOAST_TEXT_RECT_OFFSET_Y);
    int nEndY = nStartY + Helpers::ScaleWithDPI(TOAST_TEXT_RECT_HEIGHT);
    
    return CRect(nStartX, nStartY, nEndX, nEndY);
}

void CMainDlg::EditINIFile(bool bGlobalINI) {
    const QString sINIFileName = bGlobalINI
        ? CSettingsProvider::This().GetGlobalINIFileName()
        : CSettingsProvider::This().GetUserINIFileName();

    if (!bGlobalINI && !CSettingsProvider::This().ExistsUserINI()) {
        // Natürlicher Qt Dialog anstelle von ::MessageBox
        const auto result = QMessageBox::question(
            nullptr, // Später "this" (QWidget*), sobald CMainDlg von QWidget erbt
            QStringLiteral(JPEGVIEW_TITLE),
            QStringLiteral("No user INI file exists yet. Create user INI file from INI file template?"),
            QMessageBox::Yes | QMessageBox::No
        );

        if (result == QMessageBox::Yes) {
            CSettingsProvider::This().CopyUserINIFromTemplate();
        } else {
            return;
        }
    }

    const Helpers::EIniEditor iniEditor = CSettingsProvider::This().IniEditor();

    if (iniEditor == Helpers::INI_Notepad) {
        // Notepad explizit mit der INI-Datei starten
        QProcess::startDetached(QStringLiteral("notepad.exe"), { sINIFileName });
    }
    else if (iniEditor == Helpers::INI_System) {
        // Plattformunabhängiges Öffnen der Datei mit der Standard-Systemanwendung
        QDesktopServices::openUrl(QUrl::fromLocalFile(sINIFileName));
    }
    else {
        // Benutzerdefinierter Editor mit Ersetzung von %exepath%
        QString command = CSettingsProvider::This().CustomIniEditor();
        command.replace(QStringLiteral("%exepath%"), CSettingsProvider::This().GetEXEPath(), Qt::CaseInsensitive);

        // Aufruf von externem Prozess mit Argument
        QProcess::startDetached(command, { sINIFileName });
    }
}

void CMainDlg::UpdateWindowTitle(bool bForce) {
    static QString s_PrevFileExt;
    static QString s_PrevTitleText;

    const bool bShowFullPathInTitle = CSettingsProvider::This().ShowFullPathInTitle();
    const QString sCurrentFileName = CurrentFileName(!bShowFullPathInTitle);

    if (sCurrentFileName.isEmpty() || m_pCurrentImage == nullptr) {
        const QString defaultTitle = QStringLiteral(JPEGVIEW_TITLE);

        if ((s_PrevTitleText != defaultTitle) || bForce) {
            s_PrevTitleText = defaultTitle;
            this->SetWindowText(reinterpret_cast<LPCWSTR>(defaultTitle.utf16()));
        }
    } else {
        QString sWindowText = QDir::toNativeSeparators(sCurrentFileName)
        + QString(GetMultiframeIndex(m_pCurrentImage))
        + QStringLiteral(" - ")
        + QStringLiteral(JPEGVIEW_TITLE);

        if ((s_PrevTitleText != sWindowText) || bForce) {
            s_PrevTitleText = sWindowText;
            this->SetWindowText(reinterpret_cast<LPCWSTR>(sWindowText.utf16()));
        }
    }

    // Update title bar icon
    if (CSettingsProvider::This().TitleBarUseFileIcon()) {
        const QString sCurrentFilePath = CurrentFileName(false);
        QString cacheKey = QStringLiteral("__default__");
        QString currentCheckExt = QStringLiteral("__default__");

        if (!sCurrentFilePath.isEmpty() && m_pCurrentImage != nullptr) {
            const QFileInfo fileInfo(sCurrentFilePath);
            const QString ext = fileInfo.suffix(); // Endung ohne Punkt (z.B. "jpg")

            if (!ext.isEmpty()) {
                currentCheckExt = QStringLiteral(".") + ext; // ".jpg" für SHGetFileInfo
                cacheKey = currentCheckExt.toLower();
            } else {
                currentCheckExt = QString();
                cacheKey = QStringLiteral("__default__");
            }
        }

        if ((s_PrevFileExt != currentCheckExt) || bForce) {
            s_PrevFileExt = currentCheckExt;

            HICON hBig = nullptr;
            HICON hSmall = nullptr;

            if (s_iconCache.contains(cacheKey)) {
                hBig = s_iconCache[cacheKey].hBig;
                hSmall = s_iconCache[cacheKey].hSmall;
            } else {
                /* Debugging */ const UINT timeStart = Helpers::GetTimeMs();

                IconResult result;

                if (cacheKey == QStringLiteral("__default__")) {
                    HINSTANCE hInst = ::GetModuleHandle(nullptr);
                    const int cxIcon = ::GetSystemMetrics(SM_CXICON);
                    const int cyIcon = ::GetSystemMetrics(SM_CYICON);
                    const int cxSmIcon = ::GetSystemMetrics(SM_CXSMICON);
                    const int cySmIcon = ::GetSystemMetrics(SM_CYSMICON);

                    result.hBig = (HICON)::LoadImage(hInst, MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, cxIcon, cyIcon, LR_VGACOLOR);
                    result.hSmall = (HICON)::LoadImage(hInst, MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, cxSmIcon, cySmIcon, LR_VGACOLOR);
                } else {
                    SHFILEINFO shfi{};
                    // punktuelle Umwandlung von QString zu LPCTSTR/LPCWSTR für Win32 SHGetFileInfo
                    const auto* pExtWStr = reinterpret_cast<LPCWSTR>(cacheKey.utf16());

                    if (::SHGetFileInfo(pExtWStr, FILE_ATTRIBUTE_NORMAL, &shfi, sizeof(SHFILEINFO), SHGFI_ICON | SHGFI_USEFILEATTRIBUTES)) {
                        result.hBig = shfi.hIcon;
                    }
                    if (::SHGetFileInfo(pExtWStr, FILE_ATTRIBUTE_NORMAL, &shfi, sizeof(SHFILEINFO), SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
                        result.hSmall = shfi.hIcon;
                    }
                }

                if (result.hBig != nullptr || result.hSmall != nullptr) {
                    s_iconCache[cacheKey] = result;
                    hBig = result.hBig;
                    hSmall = result.hSmall;
                }

                /* Debugging */ const QString debugText = QStringLiteral("async icon loading for '%1' took %2 ms")
                .arg(cacheKey)
                .arg(Helpers::GetTimeMs() - timeStart);
                /* Debugging */ ::OutputDebugStringW(reinterpret_cast<LPCWSTR>(debugText.utf16()));
            }

            if (hBig != nullptr) {
                ::SendMessage(m_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBig));
            }

            if (hSmall != nullptr) {
                ::SendMessage(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall));
            }
        }
    }
}

int CMainDlg::TrackPopupMenu(CPoint pos, HMENU hMenu) {
    m_bInTrackPopupMenu = true;
    int nMenuCmd = ::TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, 
        pos.x, pos.y, 0, this->m_hWnd, NULL);
    m_bInTrackPopupMenu = false;
    return nMenuCmd;
}

int CMainDlg::GetLoadErrorAfterOpenFile() {
    if (m_pCurrentImage == NULL) {
        if (CurrentFileName(false).isEmpty()) {
            if (m_pFileList->IsSlideShowList()) {
                return FileLoad_SlideShowListInvalid;
            }
            return FileLoad_NoFilesInDirectory;
        }
        return FileLoad_LoadError;
    }
    return FileLoad_Ok;
}

void CMainDlg::PrefetchDIB(const CRect& clientRect) {
    if (m_pCurrentImage == NULL) return;

    CSize newSize = GetVirtualImageSize(m_pCurrentImage->OrigSize(), clientRect.Size(), GetAutoZoomMode(), m_dZoom);
    CSize clippedSize(std::min<long>(clientRect.Width(), newSize.cx), std::min<long>(clientRect.Height(), newSize.cy));
    CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, LimitOffsets(m_offsets, clientRect.Size(), newSize));
    m_pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage, *m_pImageProcParams, CreateDefaultProcessingFlags());
}

double CMainDlg::GetZoomMultiplier(CJPEGImage* pImage, const CRect& clientRect) {
    double dZoomToFit;

    Helpers::GetImageRect(pImage->OrigWidth(),
                          pImage->OrigHeight(),
                          clientRect.Width(),
                          clientRect.Height(),
                          true,
                          false,
                          false,
                          GetAutoZoomMode(),
                          dZoomToFit);

    // Zoom multiplier (zoom step) should be around 1.1 but
    // reach the value 1.0 after an integral number of zooming steps
    int n = Helpers::RoundToInt(log(1/dZoomToFit)/log(1.1));

    return (n == 0) ? 1.1 : exp(log(1/dZoomToFit)/n);
}

EProcessingFlags CMainDlg::CreateDefaultProcessingFlags(bool bKeepParams) {
    return CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, bKeepParams);
}

bool CMainDlg::HandleMouseButtonByKeymap(int nMouseButtonCode, bool bExecuteCommand) {
    bool bAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int nCommand = m_pKeyMap->GetCommandIdForKey(nMouseButtonCode, bAlt, bCtrl, bShift);
    if (nCommand > 0) {
        if (bExecuteCommand) {
            ExecuteCommand(nCommand);
        }
        return true;
    }
    return false;
}

bool CMainDlg::UseSlideShowTransitionEffect() {
    return /*m_bFullScreenMode &&*/ m_nCurrentTimeout >= 1000 && m_eTransitionEffect != Helpers::TE_None;
}

void CMainDlg::AnimateTransition() {

    int nFrameTimeMs = (m_eTransitionEffect >= Helpers::TE_RollLR || m_eTransitionEffect <= Helpers::TE_ScrollBT) ? 10 : 20;

    // paint to memory DC
    int nW = m_clientRect.Width(), nH = m_clientRect.Height();

    HDC hWndDC = ::GetDC(m_hWnd);
    if (hWndDC == NULL) {
        return;
    }
    CDCHandle paintDC(hWndDC);
    
    CDC memDC;
    memDC.CreateCompatibleDC(paintDC);
    CBitmap memDCBitmap;
    memDCBitmap.CreateCompatibleBitmap(paintDC, nW, nH);
    memDC.SelectBitmap(memDCBitmap);
    PaintToDC(memDC);

    int nSteps = std::max(1, (m_nTransitionTime + 20) / nFrameTimeMs);

    BLENDFUNCTION blendFunc{ 0 };
    blendFunc.BlendOp = AC_SRC_OVER;
    blendFunc.AlphaFormat = 0;
    float fAlphaStep = 255.0f / nSteps;

    DWORD lastTime = ::GetTickCount();
    for (int i = 0; i <= nSteps; i++) {
        switch (m_eTransitionEffect)
        {
        case Helpers::TE_None:
            break;
        case Helpers::TE_Blend:
            {
                if (i == nSteps) {
                    paintDC.BitBlt(0, 0, nW, nH, memDC, 0, 0, SRCCOPY);
                } else {
                    float fFactor = (float)i / nSteps ;
                    blendFunc.SourceConstantAlpha = std::min(255, (int)((fFactor * fFactor * i + 1) * fAlphaStep + 0.5f));
                    paintDC.AlphaBlend(0, 0, nW, nH, memDC, 0, 0, nW, nH, blendFunc);
                }
                break;
            }
        case Helpers::TE_SlideLR:
        case Helpers::TE_SlideRL:
        case Helpers::TE_SlideBT:
        case Helpers::TE_SlideTB:
            {
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nFracHeight = (int)(nH * fFactor + 0.5f);
                int nFracWidth = (int)(nW * fFactor + 0.5f);
                int nStartX = (m_eTransitionEffect == Helpers::TE_SlideLR) ? nFracWidth - nW : (m_eTransitionEffect == Helpers::TE_SlideRL) ? nW - nFracWidth : 0;
                int nStartY = (m_eTransitionEffect == Helpers::TE_SlideTB) ? nFracHeight - nH : (m_eTransitionEffect == Helpers::TE_SlideBT) ? nH - nFracHeight : 0;
                paintDC.BitBlt(nStartX, nStartY, nW, nH, memDC, 0, 0, SRCCOPY);
                break;
            }
        case Helpers::TE_RollLR:
        case Helpers::TE_RollRL:
        case Helpers::TE_RollBT:
        case Helpers::TE_RollTB:
            {
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nFracHeight = (int)(nH * fFactor + 0.5f);
                int nClipH = (m_eTransitionEffect == Helpers::TE_RollLR || m_eTransitionEffect == Helpers::TE_RollRL) ? nH : 1 + nH / (nSteps + 1);
                int nFracWidth = (int)(nW * fFactor + 0.5f);
                int nClipW = (m_eTransitionEffect == Helpers::TE_RollTB || m_eTransitionEffect == Helpers::TE_RollBT) ? nW : 1 + nW / (nSteps + 1);
                int nClipX = (m_eTransitionEffect == Helpers::TE_RollLR) ? nFracWidth - nClipW : (m_eTransitionEffect == Helpers::TE_RollRL) ? nW - nFracWidth : 0;
                int nClipY = (m_eTransitionEffect == Helpers::TE_RollTB) ? nFracHeight - nClipH : (m_eTransitionEffect == Helpers::TE_RollBT) ? nH - nFracHeight : 0;
                CRgn region;
                region.CreateRectRgn(nClipX, nClipY, nClipX + nClipW, nClipY + nClipH);
                paintDC.SelectClipRgn(region);
                paintDC.BitBlt(0, 0, nW, nH, memDC, 0, 0, SRCCOPY);
                break;
            }
        case Helpers::TE_ScrollLR:
        case Helpers::TE_ScrollRL:
        case Helpers::TE_ScrollBT:
        case Helpers::TE_ScrollTB:
            {
                float fFactorLast = (float)i / (nSteps + 1);
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nScrollW, nScrollH;
                int nStartX, nStartY;
                int nStartClipX, nStartClipY, nEndClipX, nEndClipY;
                if (m_eTransitionEffect == Helpers::TE_ScrollLR) {
                    nScrollW = (int)(nW * fFactor + 0.5f) - (int)(nW * fFactorLast + 0.5f);
                    nScrollH = 0;
                    nStartX = (int)(nW * fFactor + 0.5f) - nW;
                    nStartY = 0;
                    nStartClipX = nStartClipY = 0;
                    nEndClipX = nScrollW; nEndClipY = nH;
                } else if (m_eTransitionEffect == Helpers::TE_ScrollRL) {
                    nScrollW = - ((int)(nW * fFactor + 0.5f) - (int)(nW * fFactorLast + 0.5f));
                    nScrollH = 0;
                    nStartX = nW - (int)(nW * fFactor + 0.5f);
                    nStartY = 0;
                    nStartClipX = nW + nScrollW;
                    nStartClipY = 0;
                    nEndClipX = nW; nEndClipY = nH;
                } else if (m_eTransitionEffect == Helpers::TE_ScrollTB) {
                    nScrollW = 0;
                    nScrollH = (int)(nH * fFactor + 0.5f) - (int)(nH * fFactorLast + 0.5f);
                    nStartX = 0;
                    nStartY = (int)(nH * fFactor + 0.5f) - nH;
                    nStartClipX = 0; nStartClipY = 0;
                    nEndClipX = nW; nEndClipY = nScrollH;
                } else {
                    nScrollW = 0;
                    nScrollH = -((int)(nH * fFactor + 0.5f) - (int)(nH * fFactorLast + 0.5f));
                    nStartX = 0;
                    nStartY = nH - (int)(nH * fFactor + 0.5f);
                    nStartClipX = 0; nStartClipY = nH + nScrollH;
                    nEndClipX = nW; nEndClipY = nH;
                }

                paintDC.SelectClipRgn(NULL);
                paintDC.BitBlt(nScrollW, nScrollH, nW, nH, paintDC, 0, 0, SRCCOPY);
                
                CRgn region;
                region.CreateRectRgn(nStartClipX, nStartClipY, nEndClipX, nEndClipY);
                paintDC.SelectClipRgn(region);

                paintDC.BitBlt(nStartX, nStartY, nW, nH, memDC, 0, 0, SRCCOPY);
                break;
            }
        }
        DWORD time = ::GetTickCount();
        if (time - lastTime < nFrameTimeMs) {
            ::Sleep(nFrameTimeMs - (time - lastTime));
        }
        lastTime = time;
        
        // terminate if a key is pressed or context menu shall be shown
        MSG msg;
        if (::PeekMessage(&msg, m_hWnd, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE)) break;
        if (::PeekMessage(&msg, m_hWnd, WM_CONTEXTMENU, WM_CONTEXTMENU, PM_NOREMOVE)) break;
    }
    ::ReleaseDC(m_hWnd, hWndDC);
}

void CMainDlg::CleanupAndTerminate() {
    SaveBookmark();
    CSettingsProvider::This().SaveCustomSettings();
    StopMovieMode();
    StopAnimation();
    delete m_pJPEGProvider; // delete this early to properly shut down the loading threads
    m_pJPEGProvider = NULL;
    EndDialog(0);
}
    
void CMainDlg::StartAnimation() {
    if (m_bIsAnimationPlaying) {
        return;
    }
    m_eProcFlagsBeforeMovie = CreateDefaultProcessingFlags(m_bKeepParams);
    m_bKeepParametersBeforeAnimation = IsKeepParams();
    if (!m_bKeepParametersBeforeAnimation) {
        ExecuteCommand(IDM_KEEP_PARAMETERS);
    }

    m_bAutoContrast = false;
    m_bIsAnimationPlaying = true;
    m_nLastSlideShowImageTickCount = ::GetTickCount();

    m_nLastAnimationOffset = 0;
    
    int usedFrameTime;
    if (m_customAnimationFrameTime < 250) {
        usedFrameTime = m_customAnimationFrameTime;
    } else {
        usedFrameTime = m_pCurrentImage->FrameTimeMs();
    }
    
    int nTimerDelay = std::max(10, m_pCurrentImage->FrameTimeMs());
    m_nExpectedNextAnimationTickCount = ::GetTickCount() + nTimerDelay;
    ::SetTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID, nTimerDelay, NULL);
}

void CMainDlg::AdjustAnimationFrameTime() {
    // restart timer with new frame time
    ::KillTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID);
    
    int usedFrameTime;
    if (m_customAnimationFrameTime < 250) {
        usedFrameTime = m_customAnimationFrameTime;
    } else {
        usedFrameTime = m_pCurrentImage->FrameTimeMs();
    }
    

    m_nLastAnimationOffset = ::GetTickCount() - m_nExpectedNextAnimationTickCount;
    int nTimerDelay = std::max(10, usedFrameTime - m_nLastAnimationOffset);
    m_nExpectedNextAnimationTickCount += std::max(10, usedFrameTime);

    // If we can't process and display frames fast enough, m_nLastAnimationOffset will build up more and more.
    // SetTimer() allows for a minimum delay of 10 ms, but be still can't keep up.
    // At this point, there are only two things we can do:
    //      1) Don't drop frames by sticking to the timer and accepting that the animation is playing back slower than intended.
    //    2) Drop frames by sidestepping the timer and calling GotoImage() directly with "NO_UPDATE_WINDOW".
    //         The latter only works if the dLoadTickCount on average is smaller than our intended display FrameTimeMs.
    //         (We need to load each image because we need to access them in order for the readers that only support sequential access: JxlReader, PngReader, WebpReaderWriter)
    //         (We can only save time on not displaying (NO_UPDATE_WINDOW). And by using a direct call of GotoImage() instead of a timer.)
    //         (But if load time is already too long, we are out of look and just better stick to the timer and accept the slower animation.)
//    if (m_nLastAnimationOffset > 2 * usedFrameTime) {
//        GotoImage(POS_NextFrame, NO_REMOVE_KEY_MSG | NO_UPDATE_WINDOW);
//    } else {
        ::SetTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID, nTimerDelay, NULL);
//    }
}

void CMainDlg::StopAnimation() {
    if (!m_bIsAnimationPlaying) {
        return;
    }
    if (IsKeepParams() != m_bKeepParametersBeforeAnimation) {
        ExecuteCommand(IDM_KEEP_PARAMETERS);
    }
    if (GetProcessingFlag(m_eProcFlagsBeforeMovie, PFLAG_AutoContrast)) {
        m_bAutoContrast = true;
    }
    ::KillTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID);
    m_bIsAnimationPlaying = false;
}

void CMainDlg::ToggleAlwaysOnTop() {
    // toggle the member variable and call the SetWindowPos to set
    m_bAlwaysOnTop = !m_bAlwaysOnTop;

    // SetWindowPos - https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos
    this->SetWindowPos(
        m_bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE  // causes SetWindowPos to ignore the parameters for top/left/width/height
    );

}

/*################################################################*/
/* Custom Functions of linear scaling mod                         */
/*################################################################*/

// Handler used when receiving the custom WM_REFRESHVIEW message
// Compared to ReloadImage() this also reloads the file list!
LRESULT CMainDlg::OnRefreshView(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
    ExecuteCommand(IDM_RELOAD_FULL);
    return 0;
}

QString CMainDlg::LoadBookmark(const QString& filePath) {
    if (filePath.isEmpty()) {
        return QString();
    }

    const QString appDataPath = Helpers::JPEGViewAppDataPath();
    const QString bookmarkFilePath = QDir(appDataPath).filePath(QStringLiteral("Bookmarks.ini"));

    if (!QFileInfo::exists(bookmarkFilePath)) {
        return QString();
    }

    const QFileInfo fileInfo(filePath);
    const QString fileExt = fileInfo.suffix().toLower();
    const bool isArchive = (fileExt == QStringLiteral("cbz") ||
                            fileExt == QStringLiteral("cbr") ||
                            fileExt == QStringLiteral("cb7"));

    QString keyName = isArchive ? fileInfo.completeBaseName()
                                : fileInfo.dir().dirName();

    // 1. Universelles Entfernen von Sprach-Tags ([en], [de], [ch], [ko], etc.) am Ende
    static const QRegularExpression langTagRx(
        QStringLiteral(R"(\s+\[(en|de|ch|ko)\]$)"),
        QRegularExpression::CaseInsensitiveOption
    );
    keyName.remove(langTagRx);

    // 2. Sonderzeichen anpassen
    keyName.replace(QLatin1Char('['), QLatin1Char('<'));
    keyName.replace(QLatin1Char(']'), QLatin1Char('>'));

    // Auslesen der INI-Datei
    QSettings iniSettings(bookmarkFilePath, QSettings::IniFormat);
    iniSettings.beginGroup(QStringLiteral("JPEGView"));

    QString value = iniSettings.value(keyName, QString()).toString();
    iniSettings.endGroup();

    value.replace(QLatin1Char('['), QLatin1Char('<'));
    value.replace(QLatin1Char(']'), QLatin1Char('>'));

    return value;
}

void CMainDlg::SaveBookmark() {
    const QString filePath = CurrentFileName(false);
    bool bIsEndpoint = true;

    if (filePath.isEmpty() || !IsBookModeFile(filePath)) {
        return;
    }

    // 1. Pfade konfigurieren (QDir erstellt fehlende Pfade bei Bedarf automatisch)
    const QString appDataPath = Helpers::JPEGViewAppDataPath();
    const QDir bookmarkDir(appDataPath);

    if (!bookmarkDir.exists()) {
        if (!bookmarkDir.mkpath(QStringLiteral("."))) { // Erstellt Verzeichnis inkl. Unterordner
            return;
        }
    }

    const QString bookmarkFilePath = bookmarkDir.filePath(QStringLiteral("Bookmarks.ini"));

    // 2. Pfadanalyse mit QFileInfo & QString-Methoden
    const QFileInfo fileInfo(filePath);
    const QString fileExt = fileInfo.suffix().toLower(); // z.B. "cbz"
    const bool isArchive = (fileExt == QStringLiteral("cbz") ||
                            fileExt == QStringLiteral("cbr") ||
                            fileExt == QStringLiteral("cb7"));

    QString keyName;

    if (isArchive) {
        // Bei Archivdateien: Name der Archivdatei ohne Endung
        keyName = fileInfo.completeBaseName();
    } else {
        // Bei normalen Dateien: Name des übergeordneten Ordners
        keyName = fileInfo.dir().dirName();
    }

    // Sprach-Tag entfernen und Sonderzeichen ersetzen ([ -> <, ] -> >)
    QString keyNameCleaned = keyName;
    keyNameCleaned.remove(QStringLiteral(" [en]"), Qt::CaseInsensitive);
    keyNameCleaned.replace(QLatin1Char('['), QLatin1Char('<'));
    keyNameCleaned.replace(QLatin1Char(']'), QLatin1Char('>'));

    // 3. Neuen Wert für das Lesezeichen bestimmen
    QString newValueStr;

    if (isArchive) {
        // In Archiven: 1-basierter Frame-Index
        if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
            const int nCurrentFrame = m_pCurrentImage->FrameIndex();
            if (nCurrentFrame > 0 && nCurrentFrame < m_pCurrentImage->NumberOfFrames() - 1) {
                bIsEndpoint = false;
            }
            newValueStr = QString::number(nCurrentFrame + 1);
        }
    } else {
        // In Ordnern: Reiner Dateiname der aktuellen Bilddatei
        newValueStr = fileInfo.fileName();
        if (m_pFileList) {
            bIsEndpoint = m_pFileList->IsEndpoint();
        }
    }

    newValueStr.replace(QLatin1Char('['), QLatin1Char('<'));
    newValueStr.replace(QLatin1Char(']'), QLatin1Char('>'));

    // 4. INI-Datei mit QSettings verarbeiten (Ersetzt Get/WritePrivateProfileString)
    QSettings iniSettings(bookmarkFilePath, QSettings::IniFormat);
    iniSettings.beginGroup(QStringLiteral("JPEGView"));

    const QString oldValueStr = iniSettings.value(keyNameCleaned, QString()).toString();

    // Nur schreiben/löschen, wenn sich der Wert geändert hat
    if (oldValueStr != newValueStr) {
        if (bIsEndpoint) {
            // Am Anfang oder Ende des Buches -> Lesezeichen-Eintrag löschen
            iniSettings.remove(keyNameCleaned);
        } else {
            // Sonst -> Neuen Wert im Key speichern
            iniSettings.setValue(keyNameCleaned, newValueStr);
        }
    }

    iniSettings.endGroup();
}

bool CMainDlg::IsBookModeFile(const QString& filePath) {
    if (filePath.isEmpty()) {
        return false;
    }

    // 1. Dateiendung mit QFileInfo ermitteln (ohne führenden Punkt)
    const QString fileExt = QFileInfo(filePath).suffix().toLower();

    // 2. Prüfen auf Archiv-Endungen (.cbz, .cbr, .cb7)
    const bool isArchive = (fileExt == QStringLiteral("cbz") ||
                            fileExt == QStringLiteral("cbr") ||
                            fileExt == QStringLiteral("cb7"));

    if (isArchive) {
        return true;
    }

    // 3. Pfad-Teilstrings prüfen (Groß-/Kleinschreibung ignorieren)
    // Verwende den Qt-konformen Slash '/' oder Backslash '\'
    return filePath.contains(QStringLiteral("/manga/"), Qt::CaseInsensitive)  ||
           filePath.contains(QStringLiteral("\\manga\\"), Qt::CaseInsensitive) ||
           filePath.contains(QStringLiteral("/comics/"), Qt::CaseInsensitive) ||
           filePath.contains(QStringLiteral("\\comics\\"), Qt::CaseInsensitive);
}

float CMainDlg::MaxRatio(float valA, float valB) {
    double dRatio = (valA / valB);
    if (dRatio < 1.0)
        dRatio = 1 / dRatio;
    return dRatio;
}

void CMainDlg::SetToast(const QString& strToast, DWORD nDurationMs)
{
    ::KillTimer(this->m_hWnd, TOAST_EXPIRY_TIMER_EVENT_ID);
    ::SetTimer(this->m_hWnd, TOAST_EXPIRY_TIMER_EVENT_ID, nDurationMs, NULL);
    m_strToast = strToast;
    this->Invalidate(FALSE);
}

/*
* Show toast for current files sorting mode (and up counting mode).
*/
void CMainDlg::ToastSortingMode(Helpers::ESorting nSortMode, bool bUpCounting)
{
    static const QString sSortModes[] = {
        QStringLiteral("Sorting: Name"),
        QStringLiteral("Sorting: Modification Time"),
        QStringLiteral("Sorting: Size"),
        QStringLiteral("Sorting: Random")
    };

    QString sSortMode = sSortModes[static_cast<int>(nSortMode)];
    // sSortMode += (bUpCounting ? QStringLiteral(" ^") : QStringLiteral(" v"));
    SetToast(sSortMode);
}

// Gets an index string of the form [a/b] for multiframe images, empty string for single frame images
QString CMainDlg::GetMultiframeIndex(CJPEGImage* pImage) {
    const bool isMultiFrame = (pImage != nullptr) && (pImage->NumberOfFrames() > 1);
    
    if (isMultiFrame && !pImage->IsAnimation()) {
        return QStringLiteral(" [%1/%2]")
            .arg(pImage->FrameIndex() + 1)
            .arg(pImage->NumberOfFrames());
    }
    
    return QString();
}

QString CMainDlg::FormatFileSize(qint64 nFileSize) {
    if (nFileSize <= 0) {
        return QString();
    }

    if (nFileSize <= 1024) {
        return QString("%1 Bytes").arg(nFileSize);
    }

    static const QString units[] = { 
        QStringLiteral("Bytes"), QStringLiteral("KiB"), QStringLiteral("MiB"), 
        QStringLiteral("GiB"), QStringLiteral("TiB"), QStringLiteral("PiB"), 
        QStringLiteral("EiB"), QStringLiteral("ZiB"), QStringLiteral("YiB") 
    };

    double value = static_cast<double>(nFileSize);
    int exponent = 0;
    const int maxExponent = static_cast<int>(std::size(units)) - 1;

    while (value >= 1024.0 && exponent < maxExponent) {
        value /= 1024.0;
        exponent++;
    }

    int precision = 0;
    if (value < 10.0) {
        precision = 2; // z.B. 1.23 MiB
    } else if (value < 100.0) {
        precision = 1; // z.B. 12.3 MiB
    } else {
        precision = 0; // z.B. 123 MiB
    }

    return QString("%1 %2")
        .arg(value, 0, 'f', precision)
        .arg(units[exponent]);
}

QString CMainDlg::formatOutputZoomFactor(double m_dZoom) {
    // Skalierungsfaktor-Ausgabe für Werte > 200%
    if (m_dZoom > 100.0) {
        return QString::number(m_dZoom, 'f', 0) + "x";
    }
    if (m_dZoom > 10.0) {
        return QString::number(m_dZoom, 'f', 1) + "x";
    }
    if (m_dZoom > 2.0) {
        return QString::number(m_dZoom, 'f', 2) + "x";
    }

    // Prozent-Ausgabe für kleine Werte mit 2 signifikanten Stellen
    if (m_dZoom > 0.0) {
        int decimals = std::max(0, 1 - static_cast<int>(std::floor(std::log10(m_dZoom * 100))));
        return QString::number(m_dZoom * 100, 'f', decimals) + " %";
    }

    // Fallback
    return QString::number(qRound(m_dZoom * 100)) + " %";
}


bool CMainDlg::isRunningInWine() {
    // ntdll.dll ist in jedem Windows-Prozess bereits geladen
    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    if (!hNtDll) {
        return false;
    }

    // Check if wine-specific function exists
    pwine_get_version wine_get_version =
        (pwine_get_version)GetProcAddress(hNtDll, "wine_get_version");

    if (wine_get_version) {
        // Optional: Version auslesen (z.B. "9.0")
        // const char* version = wine_get_version();
        return true;
    }

    return false;
}

bool CMainDlg::IsLinuxBinaryAvailable(const QString& unixPath) {
    if (unixPath.isEmpty()) {
        return false;
    }

    QString dosPath;

    if (unixPath.startsWith(QLatin1Char('~'))) {
        // Unter Wine enthält USERNAME (oder USER) den Linux-Host-Benutzernamen
        QString user = qEnvironmentVariable("USERNAME");
        if (user.isEmpty()) {
            user = qEnvironmentVariable("USER");
        }

        if (!user.isEmpty()) {
            // '~' zu 'Z:\home\<username>\...' auflösen (mit mid(1) für den Restpfad)
            dosPath = QStringLiteral("Z:\\home\\%1%2").arg(user, unixPath.mid(1));
        } else {
            return false;
        }
    }
    else if (unixPath.startsWith(QLatin1Char('/'))) {
        // Absolute Unix-Pfade wie /usr/bin/... auf Laufwerk Z:\ mappen
        dosPath = QStringLiteral("Z:%1").arg(unixPath);
    }
    else {
        dosPath = unixPath;
    }

    // Unix-Slashes '/' in Windows-Backslashes '\' umwandeln
    dosPath.replace(QLatin1Char('/'), QLatin1Char('\\'));

    // Existenz als reguläre Datei prüfen (plattformunabhängig und ohne GetFileAttributes)
    const QFileInfo fileInfo(dosPath);
    return fileInfo.exists() && fileInfo.isFile();
}


// Konvertiert einen Windows-Pfad (z.B. Z:\home\...) in einen Unix-Pfad (/home/...)
#include <QString>
#include <windows.h>
QString CMainDlg::GetUnixPathFromWine(const QString& szDosPath) {
    if (szDosPath.isEmpty()) {
        return QString();
    }

    HMODULE hKernel32 = GetModuleHandle(TEXT("kernel32.dll"));
    if (!hKernel32) {
        return QString();
    }

    // Wine-Signatur: Nimmt LPCWSTR (UTF-16), liefert char* (UTF-8 POSIX-Pfad)
    using pwine_get_unix_file_name = char* (__cdecl*)(LPCWSTR);
    auto wine_get_unix_file_name = reinterpret_cast<pwine_get_unix_file_name>(
        GetProcAddress(hKernel32, "wine_get_unix_file_name")
    );

    if (wine_get_unix_file_name) {
        // QString sicher als LPCWSTR (const wchar_t*) an Wine übergeben
        const auto* pwszDosPath = reinterpret_cast<LPCWSTR>(szDosPath.utf16());
        char* unixPathUtf8 = wine_get_unix_file_name(pwszDosPath);

        if (unixPathUtf8) {
            // UTF-8 char* Stream direkt in einen Qt6 QString umwandeln
            QString sResult = QString::fromUtf8(unixPathUtf8);

            // Den von Wine auf dem Prozess-Heap allokierten Speicher freigeben
            HeapFree(GetProcessHeap(), 0, unixPathUtf8);

            return sResult;
        }
    }

    return QString();
}
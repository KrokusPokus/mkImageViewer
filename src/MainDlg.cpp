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
#include "ResizeFilter.h"
#include "ProcessingThreadPool.h"
#include "KeyMap.h"
#include "DirectoryWatcher.h"

#include <QApplication>
#include <QBuffer>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMessageBox>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QtGlobal>
#include <QKeyEvent>
#include <QMimeData>
#include <QMenu>
#include <QThread>
#include <QPainterPath>

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

    QSize GetVirtualImageSize(QSize originalImageSize, QSize screenSize, Helpers::EAutoZoomMode eAutoZoomMode, double & dZoom) {
        QSize newSize;

        if (dZoom < 0.0) {
            // zoom not set, interpret as 'fit to screen'
            // ---------------------------------------------
            newSize = Helpers::GetImageRect(originalImageSize.width(), originalImageSize.height(),
                                            screenSize.width(), screenSize.height(),
                                            eAutoZoomMode, dZoom);
        } else {
            // zoom set, use this value for the new size
            // ---------------------------------------------
            newSize = QSize(
                (int)(originalImageSize.width() * dZoom + 0.5),
                (int)(originalImageSize.height() * dZoom + 0.5)
                );
        }

        if (newSize.width() > 65535 || newSize.height() > 65535) {
            double dFac = 65535.0 / std::max(newSize.width(), newSize.height());
            dZoom = dZoom * dFac;
            newSize.setWidth((int)(originalImageSize.width() * dZoom + 0.5));
            newSize.setHeight((int)(originalImageSize.height() * dZoom + 0.5));
        }

        return newSize;
    }

    // Limits the given center based offsets so that the rectangle created by the offset and the given rectSize is totally inside of outerRect
    // outerRect has top,left coordinates 0/0
    QPoint LimitOffsets(const QPoint& offsets, const QSize & rectSize, const QSize & outerRect) {
        int nMaxOffsetX = (outerRect.width() - rectSize.width()) / 2;
        nMaxOffsetX = std::max(0, nMaxOffsetX);
        int nMaxOffsetY = (outerRect.height() - rectSize.height()) / 2;
        nMaxOffsetY = std::max(0, nMaxOffsetY);

        return QPoint(
            std::max<long>(-nMaxOffsetX, std::min<int>(+nMaxOffsetX, offsets.x())),
            std::max<long>(-nMaxOffsetY, std::min<int>(+nMaxOffsetY, offsets.y()))
            );
    }


    // Gets a window rectangle (in screen coordinates) that fits the given image
    QRect GetWindowRectMatchingImageSize(
        const QWidget* pWidget,
        QSize minSize,
        QSize maxSize,
        double& dZoom,
        CJPEGImage* pImage,
        bool bForceCenterWindow,
        bool bKeepAspectRatio,
        bool bWindowBorderless,
        Helpers::EAutoZoomMode eAutoZoomMode)
    {
        QRect workingArea = CMultiMonitorSupport::GetWorkingRect(pWidget);

        int nOrigWidth = (pImage == nullptr) ? workingArea.width() / 2 : pImage->OrigWidth();
        int nOrigWidthUnzoomed = nOrigWidth;
        int nOrigHeight = (pImage == nullptr) ? workingArea.height() / 2 : pImage->OrigHeight();

        if (dZoom == Helpers::ZoomMax) {
            if (nOrigWidth == nOrigHeight) {
                nOrigWidth = nOrigHeight = MAX_IMAGE_DIMENSION;
            } else if (nOrigWidth > nOrigHeight) {
                nOrigHeight = static_cast<int>(static_cast<double>(nOrigHeight) / nOrigWidth * MAX_IMAGE_DIMENSION);
                nOrigWidth = MAX_IMAGE_DIMENSION;
            } else {
                nOrigWidth = static_cast<int>(static_cast<double>(nOrigWidth) / nOrigHeight * MAX_IMAGE_DIMENSION);
                nOrigHeight = MAX_IMAGE_DIMENSION;
            }
        } else if (dZoom > 0) {
            nOrigWidth = static_cast<int>(nOrigWidth * dZoom + 0.5);
            nOrigHeight = static_cast<int>(nOrigHeight * dZoom + 0.5);
        }

        QSize borderSize = Helpers::GetTotalBorderSize(); // Angenommen, das liefert ein QSize oder lässt sich mappen

        int nRequiredWidth = borderSize.width() + nOrigWidth;
        int nRequiredHeight = borderSize.height() + nOrigHeight;

        if (bKeepAspectRatio && (nRequiredWidth > workingArea.width() || nRequiredHeight > workingArea.height())) {
            double dummyZoom;
            QSize imageRect = Helpers::GetImageRect(
                nOrigWidth, nOrigHeight,
                std::min(maxSize.width(), workingArea.width() - borderSize.width()),
                std::min(maxSize.height(), workingArea.height() - borderSize.height()),
                false, false, false, eAutoZoomMode, dummyZoom
                );
            nRequiredWidth = imageRect.width() + borderSize.width();
            nRequiredHeight = imageRect.height() + borderSize.height();
        } else {
            nRequiredWidth = std::min<int>(maxSize.width(), std::min(workingArea.width(), nRequiredWidth));
            nRequiredHeight = std::min<int>(maxSize.height(), std::min(workingArea.height(), nRequiredHeight));
        }

        nRequiredWidth = std::max<int>(minSize.width(), nRequiredWidth);
        nRequiredHeight = std::max<int>(minSize.height(), nRequiredHeight);

        dZoom = static_cast<double>(nRequiredWidth - borderSize.width()) / nOrigWidthUnzoomed;

        // Aktuelle Fenstergeometrie über Qt holen statt ::GetWindowRect
        QRect wndRect = pWidget ? pWidget->geometry() : workingArea;

        int nNewLeft = (wndRect.left() + wndRect.right() - nRequiredWidth) / 2;
        int nNewTop = (wndRect.top() + wndRect.bottom() - nRequiredHeight) / 2;

        nNewLeft = std::max<int>(workingArea.left(), std::min<int>(workingArea.right() - nRequiredWidth, nNewLeft));
        nNewTop = std::max<int>(workingArea.top(), std::min<int>(workingArea.bottom() - nRequiredHeight, nNewTop));

        if (!bForceCenterWindow) {
            return QRect(QPoint(nNewLeft, nNewTop), QSize(nRequiredWidth, nRequiredHeight));
        } else {
            return QRect(
                QPoint(workingArea.left() + (workingArea.width() - nRequiredWidth) / 2,
                       workingArea.top() + (workingArea.height() - nRequiredHeight) / 2),
                QSize(nRequiredWidth, nRequiredHeight)
                );
        }
    }

    // Gets the maximum client size for a framed window that fits into the working area of the screen the given window is placed on
    QSize GetMaxClientSize(const QWidget* pWidget) {
        QRect workingArea = CMultiMonitorSupport::GetWorkingRect(pWidget);
        QSize borderSize = Helpers::GetTotalBorderSize();

        return QSize(
            workingArea.width() - borderSize.width(),
            workingArea.height() - borderSize.height()
            );
    }

    // Draws an error text for the given file loading error, and detailed load error code (bit combination of EFileLoadError codes)
    // Draws an error text for the given file loading error, and detailed load error code (bit combination of EFileLoadError codes)
    void DrawImageLoadErrorText(QPainter& painter, const QRect& clientRect, const QString& failedFileName, int nFileLoadError, int nLoadErrorDetail) {
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

        QRect rectText(0, clientRect.height() / 2 - Helpers::ScaleWithDPI(40), clientRect.width(), clientRect.height());

        painter.drawText(rectText, Qt::AlignHCenter | Qt::TextWordWrap, message);
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
                        bool ok = false;

                        // 16 steht hier für die Hexadezimal-Basis (entspricht %x in swscanf)
                        int nCommand = sTabPart.toInt(&ok, 16);
                        if (!ok) {
                            nCommand = 0;
                        }

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

    // Draws a 32 bit DIB centered in the given target area, filling the remaining area with the given brush
    // The bmInfo struct will be initialized by this method and does not need to be preinitialized.
    // Return value is the top, left coordinate of the painted DIB in the target area
    /*
    QPoint DrawDIB32bppWithBlackBorders(QPainter& painter, void* pDIBData, const QColor& backColor, const CRect& targetArea, CSize dibSize, CPoint offset) {
        int targetWidth = targetArea.Width();
        int targetHeight = targetArea.Height();

        int xDest = (targetWidth - dibSize.cx) / 2 + offset.x;
        int yDest = (targetHeight - dibSize.cy) / 2 + offset.y;

        // 1. Verbleibende Randbereiche (Borders) mit der Hintergrundfarbe füllen
        if (xDest > 0) {
            QRect r(0, 0, xDest, targetHeight);
            painter.fillRect(r, backColor);
        }
        if (xDest + dibSize.cx < targetWidth) {
            QRect r(xDest + dibSize.cx, 0, targetWidth - (xDest + dibSize.cx), targetHeight);
            painter.fillRect(r, backColor);
        }
        if (yDest > 0) {
            QRect r(xDest, 0, dibSize.cx, yDest);
            painter.fillRect(r, backColor);
        }
        if (yDest + dibSize.cy < targetHeight) {
            QRect r(xDest, yDest + dibSize.cy, dibSize.cx, targetHeight - (yDest + dibSize.cy));
            painter.fillRect(r, backColor);
        }

        // 2. Rohdaten als QImage wrappen und hochperformant zeichnen
        if (pDIBData != nullptr) {
            // QImage übernimmt den Pinselstrich direkt auf die DIB-Daten (Format_RGB32 entspricht Windows 32bpp)
            QImage dibImage(static_cast<uchar*>(pDIBData), dibSize.cx, dibSize.cy, QImage::Format_RGB32);
            painter.drawImage(xDest, yDest, dibImage);
        }

        return QPoint(xDest, yDest);
    }
    */

} // namespace

//////////////////////////////////////////////////////////////////////////////////////////////
// Public
//////////////////////////////////////////////////////////////////////////////////////////////

CMainDlg::CMainDlg(const QString& sStartupFile)
    : QMainWindow(nullptr) {
    m_sStartupFile = sStartupFile;

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
    m_bResizeForNewImage = false;
    m_dZoom = -1.0;
    m_dStartZoom = -1.0;
    m_dRealizedZoom = -1.0;
    m_dZoomMult = -1.0;
    m_bDragging = false;
    m_bDoDragging = false;
    m_offsets = QPoint(0, 0);
    m_DIBOffsets = QPoint(0, 0);
    m_nCapturedX = m_nCapturedY = 0;
    m_nMouseX = m_nMouseY = 0;

    m_bFullScreenMode = sp.ShowFullScreen();
    m_bLockPaint = true;
    m_nCurrentTimeout = 0;
    m_startMouse.x = m_startMouse.y = -1;
    m_virtualImageSize = QSize(-1, -1);
    m_bInZooming = false;
    m_bTemporaryLowQ = false;
    m_bShowZoomFactor = false;
    m_bPanMouseCursorSet = false;
    m_storedWindowPlacement.length = sizeof(WINDOWPLACEMENT);
    m_nMonitor = 0;
    m_monitorRect = QRect(0, 0, 0, 0);
    m_windowRectOnClose = QRect(0, 0, 0, 0);
    m_bMouseOn = false;
    m_bKeepParametersBeforeAnimation = false;
    m_bIsAnimationPlaying = false;
    m_nLastAnimationOffset = 0;
    m_nExpectedNextAnimationTickCount = 0;
    m_isUserFitToScreen = false;
    m_autoZoomFitToScreen = Helpers::ZM_FillScreen;
    m_bSelectZoom = false;  // this value is set when LButtonDown happens, to be read by LButtonUp

    m_pKeyMap = new CKeyMap(); // routine to load the keymap, it's not as simple as just loading one file anymore, but all logic handled by CKeyMap

    m_eTransparencyMode = sp.TransparencyMode();
    m_bUseEmbeddedColorProfiles = sp.UseEmbeddedColorProfiles();

    /*############################################*/
    /* Custom variables of the linear scaling mod */
    /*############################################*/

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

    //setMinimumSize(0, 0);
    QSize minimalSize = sp.MinimalWindowSize();
    setMinimumSize(std::max(0, minimalSize.width()), std::max(0, minimalSize.height()));

    this->setWindowTitle("JPEGView");

    // DPI-Skalierung über den aktuellen Screen ermitteln
    QScreen* currentScreen = this->screen();
    if (currentScreen) {
        Helpers::DPIScalingFactor = currentScreen->logicalDotsPerInch() / 96.0f;
    } else {
        Helpers::DPIScalingFactor = 1.0f;
    }

    m_pDirectoryWatcher = new CDirectoryWatcher(this);

    // determine the monitor rectangle and client rectangle
    m_nMonitor = sp.DisplayMonitor();
    m_monitorRect = CMultiMonitorSupport::GetMonitorRect(m_nMonitor);
    this->setGeometry(m_monitorRect);

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

    m_clientRect = m_bFullScreenMode ? m_monitorRect : CMultiMonitorSupport::GetDefaultClientRectInWindowMode();

    this->setWindowIcon(QIcon(":/icons/mainframes.ico"));

    // turn on/off mouse coursor
    m_bMouseOn = !m_bFullScreenMode;
    if (!m_bMouseOn) {
        this->setCursor(Qt::BlankCursor);
    } else {
        this->unsetCursor();
    }

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

    int nReadAheadBuffers = (CSettingsProvider::This().CacheRange() * 2) + 1;    // The +1 is the current image

    // create JPEG provider and request first image - do no processing yet if not in fullscreen mode (as we do not know the size yet)
    m_pJPEGProvider = new CJPEGProvider(this, NUM_THREADS, nReadAheadBuffers);

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

    AfterNewImageLoaded(true, true); // synchronize to per image parameters

    if (!m_bFullScreenMode) {
		QRect windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
		this->setGeometry(windowRect);
        if (sp.DefaultMaximized()) {
            this->showMaximized();
        }
    } else {
        PrefetchDIB(m_monitorRect);
        this->showFullScreen();
        this->setGeometry(m_monitorRect);
    }

    m_bLockPaint = false;

    if (m_nAutoStartSlideShow > 0) {
        StartMovieMode(1.0 / m_nAutoStartSlideShow);
    }

    StartLowQTimer(ZOOM_TIMEOUT);    // Start out in low quality scaling to get something on the screen asap. Feels more responsive with huge images.

    this->update();

    this->setAcceptDrops(true);
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
}

void CMainDlg::paintEvent(QPaintEvent* /*event*/)
{
    if (m_bLockPaint) {
        return;
    }

    QPainter painter(this);
    m_dRealizedZoom = 1.0;

    QRect m_clientRect = rect();
    m_clientRect = rect();

    QColor backColor = CSettingsProvider::This().ColorBackground();
    painter.fillRect(m_clientRect, backColor);

    if (m_pCurrentImage != NULL) {
        m_pCurrentImage->VerifyRotation(CRotationParams(m_pCurrentImage->GetRotationParams(), m_nRotation));

        if (m_dZoomMult < 0.0) {
            m_dZoomMult = GetZoomMultiplier(m_pCurrentImage, m_clientRect);
        }

        QSize newSize = GetVirtualImageSize(m_pCurrentImage->OrigSize(),
                                            m_clientRect.size(),
                                            m_isUserFitToScreen ? m_autoZoomFitToScreen : GetAutoZoomMode(),
                                            m_dZoom);

        m_virtualImageSize = newSize;
        m_dRealizedZoom = (double)newSize.width() / m_pCurrentImage->OrigSize().cx;
        QPoint unlimitedOffsets = m_offsets;
        m_offsets = LimitOffsets(m_offsets, m_clientRect.size(), newSize);
        m_DIBOffsets = m_bZoomMode ? (unlimitedOffsets - m_offsets) : QPoint(0, 0);

        QSize clippedSize(std::min<long>(m_clientRect.width(), newSize.width()), std::min<long>(m_clientRect.height(), newSize.height()));
        CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, m_offsets);

        void* pDIBData = m_pCurrentImage->GetDIB(newSize,
                                                 clippedSize,
                                                 offsetsInImage,
                                                 *m_pImageProcParams,
                                                 CreateProcessingFlags(m_bHQResampling && !m_bTemporaryLowQ && !m_bZoomMode, m_bAutoContrast, false));

        // Das rohe DIB über Qt QImage hochperformant zeichnen
        if (pDIBData != NULL) {
            QImage dibImage(static_cast<uchar*>(pDIBData), clippedSize.width(), clippedSize.height(), QImage::Format_RGB32);

            int xDest = (m_clientRect.width() - clippedSize.width()) / 2 + m_DIBOffsets.x();
            int yDest = (m_clientRect.height() - clippedSize.height()) / 2 + m_DIBOffsets.y();

            painter.drawImage(xDest, yDest, dibImage);
        }

        if (m_bZoomMode) m_offsets = unlimitedOffsets;

        // -------------------------------------------------------------------------
        // 1. Info-Overlay (m_bShowInfo) über QPainter zeichnen
        // -------------------------------------------------------------------------
        if (m_bShowInfo) {
            CSettingsProvider& sp = CSettingsProvider::This();
            int iNumThreads = sp.NumberOfCoresToUse();
            Helpers::CPUType cpu = sp.AlgorithmImplementation();
            EFilterType filter = sp.DownsamplingFilter();
            QString sCPU;
            QString sFilter;

            if (cpu == Helpers::CPU_Unknown) sCPU = "Unknown";
            else if (cpu == Helpers::CPU_Generic) sCPU = "Generic";
            else if (cpu == Helpers::CPU_MMX) sCPU = "MMX";
            else if (cpu == Helpers::CPU_SSE) sCPU = "SSE2 (128 bit)";
            else if (cpu == Helpers::CPU_AVX2) sCPU = "AVX2 (256 bit)";

            if (filter == Filter_Downsampling_None) sFilter = "None";
            else if (filter == Filter_Downsampling_Hermite) sFilter = "Hermite";
            else if (filter == Filter_Downsampling_Catrom) sFilter = "Catrom";
            else if (filter == Filter_Downsampling_Mitchell) sFilter = "Mitchell";
            else if (filter == Filter_Downsampling_Lanczos2) sFilter = "Lanczos2";

            unsigned int iRealWidth = static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigWidth()));
            unsigned int iRealHeight = static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigHeight()));

            QString infoText;
            QString sFullPath = CurrentFileName(false);

            if (!sFullPath.isEmpty()) {
                QFileInfo fileInfo(sFullPath);
                QString sFileSize = FormatFileSize(fileInfo.size());
                QString visiblePath = QDir::toNativeSeparators(sFullPath);

                if (m_pCurrentImage->NumberOfFrames() > 1) {
                    if (m_bIsAnimationPlaying) {
                        infoText = QString("%1\nFrame %2 of %3 (%4 ms)\nFile Size:   %5\nImage Size:  %6 x %7\nZoomed Size: %8 x %9   (Zoom Factor: %10)\nWindow Size: %11 x %12\n\nCPU-Threads: %13\nCPU-Algorithm: %14\nDownsampling-Filter: %15\n\nLoading Image:  %16 ms\nScaling Image:  %17 ms")
                        .arg(visiblePath).arg(m_pCurrentImage->FrameIndex() + 1).arg(m_pCurrentImage->NumberOfFrames())
                            .arg(m_pCurrentImage->FrameTimeMs()).arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                            .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                            .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs());
                    } else {
                        infoText = QString("%1\nFrame %2 of %3\nFile Size:   %4\nImage Size:  %5 x %6\nZoomed Size: %7 x %8   (Zoom Factor: %9)\nWindow Size: %10 x %11\n\nCPU-Threads: %12\nCPU-Algorithm: %13\nDownsampling-Filter: %14\n\nLoading Image:  %15 ms\nScaling Image:  %16 ms")
                        .arg(visiblePath).arg(m_pCurrentImage->FrameIndex() + 1).arg(m_pCurrentImage->NumberOfFrames())
                            .arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                            .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                            .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs());
                    }
                } else {
                    infoText = QString("%1\nFile Size:   %2\nImage Size:  %3 x %4\nZoomed Size: %5 x %6   (Zoom Factor: %7)\nWindow Size: %8 x %9\n\nCPU-Threads: %10\nCPU-Algorithm: %11\nDownsampling-Filter: %12\n\nLoading Image:  %13 ms\nScaling Image:  %14 ms")
                    .arg(visiblePath).arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                        .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                        .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs());
                }
            } else {
                infoText = QString("\n\nImage Size:  %1 x %2\nZoomed Size: %3 x %4   (Zoom Factor: %5)\nWindow Size: %6 x %7\n\nCPU-Threads: %8\nCPU-Algorithm: %9\nDownsampling-Filter: %10\n\nLoading Image:  %11 ms\nScaling Image:  %12 ms")
                .arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                    .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                    .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs());
            }

            // QPainter Konfiguration für Text
            painter.setFont(QFont("Consolas", 10));

            // Verfügbares Rechteck definieren (mit 10px Abstand zum Rand)
            QRect textRect = m_clientRect.adjusted(10, 10, -10, -10);

            // 1. Exakte Ausdehnung des Textes (inkl. Wortumbruch) im Vorfeld berechnen
            QRect boundingRect = painter.boundingRect(
                textRect,
                Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                infoText
                );

            // 2. Hintergrund-Rechteck (z.B. halbtransparent schwarz oder deckend schwarz) zeichnen
            // Für echtes OPAQUE (vollständig deckendes Schwarz): QColor(0, 0, 0)
            // Für ein schöneres, leicht transparentes Overlay (z.B. RGBA 150): QColor(0, 0, 0, 150)
            painter.fillRect(boundingRect.adjusted(-4, -4, 4, 4), QColor(0, 0, 0, 180));

            // 3. Text in Weiß darüberzeichnen
            painter.setPen(Qt::white);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, infoText);
        }
    }

    // -------------------------------------------------------------------------
    // 2. Zoom-Faktor-Anzeige
    // -------------------------------------------------------------------------
    if (m_bInZooming || m_bShowZoomFactor) {
        /*
        painter.setFont(QFont("Arial", 12, QFont::Bold));
        painter.setPen(CSettingsProvider::This().ColorGUI());
        painter.drawText(GetZoomTextRect(), Qt::AlignRight | Qt::AlignVCenter, formatOutputZoomFactor(m_dZoom));
        */
        QString zoomText = formatOutputZoomFactor(m_dZoom);

        QRect zoomRect = GetZoomTextRect();

        QFont font("Arial", 12, QFont::Bold);
        painter.setFont(font);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath textPath;
        QFontMetrics fm(font);
        int textWidth = fm.horizontalAdvance(zoomText);

        int x = zoomRect.right() - textWidth;
        int y = zoomRect.center().y() + (fm.ascent() - fm.descent()) / 2;

        textPath.addText(x, y, font, zoomText);

        // 1. Outline-Pfad (Stroker) erzeugen – dieser wächst sauber nach außen
        QPainterPathStroker stroker;
        stroker.setWidth(3); // Dicke der Umrandung (z.B. 2 oder 3 Pixel)
        QPainterPath outlinePath = stroker.createStroke(textPath);

        // 2. Zuerst die schwarze Umrandung zeichnen
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPath(outlinePath);

        // 3. Danach den weißen Text exakt darüberlegen
        painter.setBrush(Qt::white);
        painter.drawPath(textPath);
    }

    // -------------------------------------------------------------------------
    // 3. Toast-Nachrichten
    // -------------------------------------------------------------------------
    if (!m_strToast.isEmpty()) {
        /*
        painter.setFont(QFont("Arial", 12, QFont::Bold));
        painter.setPen(CSettingsProvider::This().ColorGUI());
        painter.drawText(GetToastTextRect(), Qt::AlignRight | Qt::AlignVCenter, m_strToast);
        */
        QRect toastRect = GetToastTextRect();

        QFont font("Arial", 14, QFont::Bold);
        painter.setFont(font);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath textPath;
        QFontMetrics fm(font);
        int textWidth = fm.horizontalAdvance(m_strToast);

        int x = toastRect.right() - textWidth;
        int y = toastRect.center().y() + (fm.ascent() - fm.descent()) / 2;

        textPath.addText(x, y, font, m_strToast);

        // 1. Outline-Pfad (Stroker) erzeugen – dieser wächst sauber nach außen
        QPainterPathStroker stroker;
        stroker.setWidth(3); // Dicke der Umrandung (z.B. 2 oder 3 Pixel)
        QPainterPath outlinePath = stroker.createStroke(textPath);

        // 2. Zuerst die schwarze Umrandung zeichnen
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPath(outlinePath);

        // 3. Danach den weißen Text exakt darüberlegen
        painter.setBrush(Qt::white);
        painter.drawPath(textPath);
    }

    SetCursorForMoveSection();
}

/*
void CMainDlg::PaintToDC(QPainter& painter) {
    QColor backColor = CSettingsProvider::This().ColorBackground();
    if (backColor.rgb() == 0) {
        backColor = QColor(0, 0, 1); // nVidia Bug-Workaround für reines Schwarz
    }

    m_dRealizedZoom = 1.0;

    QRect qClient(m_clientRect.left, m_clientRect.top, m_clientRect.Width(), m_clientRect.Height());

    CJPEGImage* pCurrentImage = GetCurrentImage();
    if (pCurrentImage == NULL) {
        painter.fillRect(qClient, backColor);
    } else {
        // Rotation verifizieren
        pCurrentImage->VerifyRotation(CRotationParams(pCurrentImage->GetRotationParams(), GetRotation()));

        // Virtuelle Bildgröße und Zoom berechnen
        double dZoom = m_dZoom;
        QSize newSize = GetVirtualImageSize(pCurrentImage->OrigSize(), qClient.size(), GetAutoZoomMode(), dZoom);
        CPoint offsets = LimitOffsets(GetOffsets(), qClient.size(), newSize);
        m_dRealizedZoom = (double)newSize.cx / m_pCurrentImage->OrigSize().cx;

        // Auf Client-Rechteck clippen und DIB anfordern
        CSize clippedSize(std::min<long>(qClient.width(), newSize.cx), std::min<long>(qClient.height(), newSize.cy));
        CPoint offsetsInImage = pCurrentImage->ConvertOffset(newSize, clippedSize, offsets);

        void* pDIBData = pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage,
                *GetImageProcessingParams(),
                CreateDefaultProcessingFlags());

        // Hintergrund füllen und Bild hochperformant via QImage / QPainter zeichnen
        painter.fillRect(qClient, backColor);

        if (pDIBData != NULL) {
            QImage dibImage(static_cast<uchar*>(pDIBData), clippedSize.width(), clippedSize.height(), QImage::Format_RGB32);

            int xDest = (qClient.width() - clippedSize.width()) / 2;
            int yDest = (qClient.height() - clippedSize.height()) / 2;

            painter.drawImage(xDest, yDest, dibImage);
        }

        // Fehler- und Warnungsanzeige mit dem neuen QPainter übergeben
        DisplayErrors(pCurrentImage, qClient, painter);
    }
}
*/

void CMainDlg::DisplayErrors(CJPEGImage* pCurrentImage, const QRect& clientRect, QPainter& painter)
{
    // Textfarbe aus den Einstellungen setzen
    painter.setPen(CSettingsProvider::This().ColorGUI());

    // Einheitliche Schriftart für Fehlermeldungen setzen (z. B. Arial oder Standard-GUI-Font)
    painter.setFont(QFont("Arial", 12, QFont::Bold));

    if (m_sStartupFile.isEmpty() && m_pCurrentImage == NULL) {
        QRect rectText(0, clientRect.height() / 2 - Helpers::ScaleWithDPI(40), clientRect.width(), Helpers::ScaleWithDPI(80));
        QString message = QStringLiteral("No image loaded!\n\nPress ESC to exit...");

        // Mit QPainter zentriert und mit Wortumbruch zeichnen
        painter.drawText(rectText, Qt::AlignCenter | Qt::TextWordWrap, message);
    }
    else if (pCurrentImage == NULL) {
        QString sFileName;
        if (m_nLastLoadError == FileLoad_SlideShowListInvalid) {
            sFileName = m_sStartupFile;
        } else if (m_nLastLoadError == FileLoad_NoFilesInDirectory) {
            sFileName = m_pFileList->CurrentDirectory();
        } else {
            sFileName = CurrentFileName(false);
        }

        // Falls DrawImageLoadErrorText ebenfalls CDC verwendet, muss diese Funktion
        // im nächsten Schritt ebenfalls auf QPainter& umgestellt werden!
        DrawImageLoadErrorText(painter, clientRect, sFileName, m_nLastLoadError,
                               (m_bOutOfMemoryLastImage ? FileLoad_OutOfMemory : 0) | (m_bExceptionErrorLastImage ? FileLoad_ExceptionError : 0));
    }
}
void CMainDlg::resizeEvent(QResizeEvent* event) {
    if (isMinimized()) {
        QMainWindow::resizeEvent(event);
        return;
    }

    Helpers::EAutoZoomMode eAutoZoomMode = GetAutoZoomMode();
    bool bFillCrop = (eAutoZoomMode == Helpers::ZM_FillScreenNoZoom || eAutoZoomMode == Helpers::ZM_FillScreen);
    bool bAllowZoomIn = (eAutoZoomMode == Helpers::ZM_FitToScreen || eAutoZoomMode == Helpers::ZM_FillScreen);
    bool bAdjustWindowSize = false;

    float dZoomForFit = GetZoomFactorForFitToScreen(bFillCrop, bAllowZoomIn);
    bool bKeepFitToScreen = !m_bResizeForNewImage && (MaxRatio(m_dZoom, dZoomForFit) < 1.01);

    m_clientRect = QRect(0, 0, width(), height());

    this->update();

    m_nMouseX = m_nMouseY = -1;

    // keep fit to screen
    if (bKeepFitToScreen) {
        dZoomForFit = GetZoomFactorForFitToScreen(bFillCrop, bAllowZoomIn);    // Erneut aufrufen, da m_clientRect nun aktuell ist.
        if (MaxRatio(m_dZoom, dZoomForFit) >= 1.00001) {
            ResetZoomToFitScreen(bFillCrop, bAllowZoomIn, bAdjustWindowSize);
        }
    }

    QMainWindow::resizeEvent(event);
}

// Note: Reacts to WM_COPYDATA message.
// If the message contains a file path, we can either open it and send back KEY_MAGIC, or ignore it and return NULL.
// The hidden assumption being that if the WM_COPYDATA came from newly launched instance, that instance will,
// upon receiving KEY_MAGIC, dutifully close itself, since we answered it "I got this" by returning KEY_MAGIC.
// If we return NULL, the new instance is free to do as it pleases and show the image itself or whatever.
bool CMainDlg::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#if defined(Q_OS_WIN)
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_COPYDATA) {
            // Todo: Handle via QLocalServer/QLocalSocket or QSharedMemory
            auto* pData = reinterpret_cast<COPYDATASTRUCT*>(msg->lParam);
            if (pData != nullptr) {
                if (pData->dwData == KEY_MAGIC && pData->cbData > 0) {
                    bool bSameImageFolder = false;

                    const auto* pWCharData = static_cast<const wchar_t*>(pData->lpData);
                    const int charCount = static_cast<int>((pData->cbData / sizeof(wchar_t)) - 1);
                    const QString sCopyDataFile = QString::fromWCharArray(pWCharData, charCount);

                    const QString sCurrentFile = CurrentFileName(false);

                    if (!sCurrentFile.isEmpty()) {
                        if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
                            if (sCopyDataFile.compare(sCurrentFile, Qt::CaseInsensitive) == 0) {
                                *result = KEY_MAGIC;
                                showNormal();
                                raise();
                                activateWindow();
                                return true; // Handled
                            } else {
                                *result = 0;
                                return true;
                            }
                        } else {
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

                        // Asynchron laden über Qt-invokeMethod statt PostMessage
                        QMetaObject::invokeMethod(this, [this]() {
                            // Hier dein Laden triggern, analog zu WM_LOAD_FILE_ASYNCH
                        }, Qt::QueuedConnection);

                        showNormal();
                        raise();
                        activateWindow();
                        *result = KEY_MAGIC;
                        return true;
                    }
                }
            }
            *result = 0;
            return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void CMainDlg::loadFileAsynch() {
    if (isEnabled()) {
        StopMovieMode();
        StopAnimation();
        MouseOn();
        OpenFile(m_sStartupFile, false);
    }
}

void CMainDlg::closeEvent(QCloseEvent* event) {
    // geometry() liefert die echten Fensterkoordinaten (ohne Window-Manager-Rahmen),
    // die setGeometry() beim Start auch fehlerfrei wiederherstellen kann.
    m_windowRectOnClose = geometry();
    event->accept(); // Erlaubt das Schließen des Fensters (entspricht bHandled = FALSE)
}

//LRESULT CMainDlg::OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
void CMainDlg::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QPoint pos = event->pos(); // statt GET_X_LPARAM(lParam)

        this->grabMouse();
        CPoint pointClicked(pos.x(), pos.y()); // Direkt über das Qt-Event aufgelöst

        bool bCtrl = event->modifiers().testFlag(Qt::ControlModifier);
        bool bShift = event->modifiers().testFlag(Qt::ShiftModifier);

        bool bDraggingRequired = m_virtualImageSize.width() > m_clientRect.width() || m_virtualImageSize.height() > m_clientRect.height();

        if (!m_bZoomModeOnLeftMouse && HandleMouseButtonByKeymap(VK_LBUTTON)) {
            return;
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
        return;
    }
    else if (event->button() == Qt::RightButton) {
        // Entspricht OnRButtonDown
        if (HandleMouseButtonByKeymap(VK_RBUTTON)) {
            event->accept();
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton) {
        this->grabMouse();

        if (HandleMouseButtonByKeymap(VK_MBUTTON)) {
            return;
        }

        QPoint pos = event->pos();
        StartDragging(pos.x(), pos.y(), false);
    }
    else if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        bool isXButton1 = (event->button() == Qt::BackButton);
        int vkCode = isXButton1 ? VK_XBUTTON1 : VK_XBUTTON2;

        if (HandleMouseButtonByKeymap(vkCode)) {
            return;
        }

        bool bExchangeXButtons = CSettingsProvider::This().ExchangeXButtons();
        if (isXButton1) {
            GotoImage(bExchangeXButtons ? POS_Next : POS_Previous);
        } else {
            GotoImage(bExchangeXButtons ? POS_Previous : POS_Next);
        }
    }

    QMainWindow::mousePressEvent(event);
}

void CMainDlg::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_bZoomMode) {
            m_bZoomMode = false;
            update();
        } else if (m_bDragging) {
            EndDragging();
        }
        this->releaseMouse();
    }
    else if (event->button() == Qt::RightButton) {
        // Entspricht OnRButtonUp (bExecuteCommand = false)
        if (HandleMouseButtonByKeymap(VK_RBUTTON, false)) {
            event->accept();
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton) {
        EndDragging();
        this->releaseMouse();
    }

    QMainWindow::mouseReleaseEvent(event);
}

void CMainDlg::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_pCurrentImage != NULL) {
            if (HandleMouseButtonByKeymap(VK_LBUTTONDBLCLK)) {
                return;
            }
            double dZoom = -1.0;
            QSize sizeAutoZoom = GetVirtualImageSize(m_pCurrentImage->OrigSize(), m_clientRect.size(), GetAutoZoomMode(), dZoom);
            if (sizeAutoZoom != m_virtualImageSize) {
                ExecuteCommand(GetAutoZoomMode() * 10 + IDM_AUTO_ZOOM_NONE);
            } else {
                ResetZoomTo100Percents(true);
            }
        }
    }
    // Falls du andere Doppelklicks hast, kannst du hier erweitern
    QMainWindow::mouseDoubleClickEvent(event);
}

void CMainDlg::mouseMoveEvent(QMouseEvent* event) {
    // Turn mouse pointer on when mouse has moved some distance
    m_nMouseX = event->pos().x();
    m_nMouseY = event->pos().y();

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
        PerformZoom(m_dStartZoom * dFactor, false, true);
    } else if (m_bDragging) {
        DoDragging();
    }
    if (!m_bPanMouseCursorSet && !bMouseCursorSet) {
        ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
    }
}

void CMainDlg::wheelEvent(QWheelEvent* event) {
    bool bCtrl = event->modifiers().testFlag(Qt::ControlModifier);
    bool bShift = event->modifiers().testFlag(Qt::ShiftModifier);

    // Qt angleDelta().y() liefert 120 für einen Standard-Rastschritt, analog zu WHEEL_DELTA
    int nDelta = event->angleDelta().y();
    if (nDelta == 0) {
        nDelta = event->angleDelta().x();
    }

    if (CSettingsProvider::This().NavigateWithMouseWheel()) {
        if (!bCtrl && !bShift) {
            // Smart handling:
            // Pan vertically (if image is higher than window)
            // OR go to previous/next image (if image is smaller).
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                        if (PerformPan(0, -PAN_STEP, false) == true) {
                            this->update();
                        }
                    } else {
                        unsigned int iRealHeight = static_cast<unsigned int>(m_dZoom * (m_pCurrentImage->OrigHeight()));

                        if (iRealHeight > m_clientRect.height()) {
                            if (PerformPan(0, -PAN_STEP, false) == true) {
                                this->update();
                            }
                        } else {
                            GotoImage(POS_Next);
                        }
                    }
                }
                else if (nDelta > 0) {
                    if (GetAutoZoomMode() == Helpers::ZM_BookMode) {
                        if (PerformPan(0, PAN_STEP, false) == true) {
                            this->update();
                        }
                    } else {
                        uint iRealHeight = static_cast<uint>(m_dZoom * (m_pCurrentImage->OrigHeight()));

                        if (iRealHeight > m_clientRect.height()) {
                            if (PerformPan(0, PAN_STEP, false) == true) {
                                this->update();
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
                    CSettingsProvider::This().MouseWheelZoomSpeed() * double(nDelta) / WHEEL_DELTA,
                    true,
                    m_bMouseOn
                    );
            }
        } else if (bShift) { // Pan horizontally
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (PerformPan(-PAN_STEP, 0, false) == true) {
                        this->update();
                    }
                } else if (nDelta > 0) {
                    if (PerformPan(PAN_STEP, 0, false) == true) {
                        this->update();
                    }
                }
            }
        } else if (bCtrl) { // Pan vertically
            if (m_pCurrentImage != NULL) {
                if (nDelta < 0) {
                    if (PerformPan(0, -PAN_STEP, false) == true) {
                        this->update();
                    }
                } else if (nDelta > 0) {
                    if (PerformPan(0, PAN_STEP, false) == true) {
                        this->update();
                    }
                }
            }
        }
    } else if (m_dZoom > 0) {
        PerformZoom(
            CSettingsProvider::This().MouseWheelZoomSpeed() * double(nDelta) / WHEEL_DELTA,
            true,
            m_bMouseOn
            );
    }

    event->accept();
}

void CMainDlg::keyPressEvent(QKeyEvent* event) {
    bool bCtrl = event->modifiers().testFlag(Qt::ControlModifier);
    bool bShift = event->modifiers().testFlag(Qt::ShiftModifier);
    bool bAlt = event->modifiers().testFlag(Qt::AltModifier);

    // Qt-Key in den alten Win32 Virtual-Key übersetzen (falls m_pKeyMap noch VK_* erwartet)
    // Alternativ: Nutze event->key(), falls dein KeyMap bereits Qt-Keys versteht.
    int vkCode = event->nativeVirtualKey();
    if (vkCode == 0) {
        vkCode = event->key(); // Fallback
    }

    if (!bCtrl && vkCode != Qt::Key_Escape && m_nLastLoadError == FileLoad_NoFilesInDirectory && !m_sStartupFile.isEmpty()) {
        // search in subfolders if initial directory has no images
        m_pFileList->SetNavigationMode(Helpers::NM_LoopSubDirectories);
        GotoImage(POS_Next);
        event->accept();
        return;
    }

    if (vkCode == Qt::Key_F1) {
        ExecuteCommand(IDM_HELP);
        event->accept();
        return;
    }

    int nCommand = m_pKeyMap->GetCommandIdForKey(vkCode, bAlt, bCtrl, bShift);
    if (nCommand > 0) {
        if (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN || IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT) {
            // Prüfung analog zum Original: Wenn eine andere Taste als die Pfeiltaste selbst das Panning triggert
            if ((nCommand == IDM_PAN_UP && vkCode != Qt::Key_Up) ||
                (nCommand == IDM_PAN_DOWN && vkCode != Qt::Key_Down) ||
                (nCommand == IDM_PAN_LEFT && vkCode != Qt::Key_Left) ||
                (nCommand == IDM_PAN_RIGHT && vkCode != Qt::Key_Right)) {
                m_bAllowSmoothPanning = false;
            }

            // Qt-Äquivalent für Key-Repeat (ersetzt lParam & 0xc0000000)
            if (m_bAllowSmoothPanning && event->isAutoRepeat()) {
                event->accept();
                return;
            }
        }

        ExecuteCommand(nCommand);
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void CMainDlg::onDisplayedFileChangedOnDisk() {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged() &&
            m_pCurrentImage != NULL &&
            m_pFileList != NULL &&
            m_pFileList->CanOpenCurrentFileForReading()) {
        ExecuteCommand(IDM_RELOAD);
    }
}

void CMainDlg::onActiveDirectoryFilelistChanged() {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged()) {
        if (m_pFileList != NULL && m_pFileList->CurrentFileExists()) {
            m_pFileList->Reload(NULL, false);
            update(); // Qt-Methode zum Neuzeichnen
        }

        if (m_pCurrentImage != NULL) {
            GotoImage(POS_Current);
        }
    }
}

void CMainDlg::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void CMainDlg::dropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();
        if (!urlList.isEmpty()) {
            // Wir nehmen die erste gedroppte Datei
            QString filePath = urlList.first().toLocalFile();

            if (!filePath.isEmpty()) {
                QFileInfo fileInfo(filePath);
                // Falls es ein Ordner ist, hängen wir wie im Originalcode einen Slash an
                if (fileInfo.isDir()) {
                    if (!filePath.endsWith('/') && !filePath.endsWith('\\')) {
                        filePath += QDir::separator();
                    }
                }

                OpenFile(filePath, false);
                event->acceptProposedAction();
            }
        }
    }
}

void CMainDlg::timerEvent(QTimerEvent* event) {
    int timerId = event->timerId();

    if (timerId == m_animationTimerId || (timerId == m_slideshowTimerId && m_nCurrentTimeout > 0)) {
        // In Qt läuft die Event-Queue sequenziell ab, das manuelle Herausfischen
        // von WM_TIMER-Messages via PeekMessage entfällt hier in der Regel.

        // Ersetze ::GetTickCount() durch Qt / C++ Standard (Millisekunden)
        qint64 currentTick = QDateTime::currentMSecsSinceEpoch();
        qint64 nRealDisplayTimeMs = currentTick - m_nLastSlideShowImageTickCount;

        if (m_nCurrentTimeout > 250 && timerId == m_slideshowTimerId) {
            if (m_nCurrentTimeout - nRealDisplayTimeMs > 100) {
                killTimer(m_slideshowTimerId);
                QThread::msleep(m_nCurrentTimeout - nRealDisplayTimeMs);
                m_slideshowTimerId = startTimer(m_nCurrentTimeout);
            }
        }

        GotoImage((timerId == m_animationTimerId) ? POS_NextFrame : POS_NextSlideShow, NO_REMOVE_KEY_MSG);

        if (timerId == m_slideshowTimerId && UseSlideShowTransitionEffect()) {
            //AnimateTransition();
        }

        if (timerId != m_animationTimerId) {
            m_nLastSlideShowImageTickCount = QDateTime::currentMSecsSinceEpoch();
        }
    }
    else if (timerId == m_zoomTimerId) {
        killTimer(m_zoomTimerId);
        m_zoomTimerId = 0;

        if (m_bTemporaryLowQ || m_bInZooming) {
            m_zoomTextTimerId = startTimer(ZOOM_TEXT_TIMEOUT);
            if (m_bInZooming) {
                m_bShowZoomFactor = true;
            }
            m_bTemporaryLowQ = false;
            m_bInZooming = false;
            if (m_bHQResampling && m_pCurrentImage != nullptr) {
                this->update();
            }
        }
    }
    else if (timerId == m_zoomTextTimerId) {
        m_bShowZoomFactor = false;
        killTimer(m_zoomTextTimerId);
        m_zoomTextTimerId = 0;

        // InvalidateRect wird in Qt durch update(QRect) ersetzt
        this->update(GetZoomTextRect());
    }
    else if (timerId == m_toastTimerId) {
        killTimer(m_toastTimerId);
        m_toastTimerId = 0;
        m_strToast = "";

        this->update(GetToastTextRect());
    }
    else if (timerId == m_panTimerId) {
        if (m_pCurrentImage != nullptr) {
            // In Qt prüfen wir Tastendrücke idealerweise über Qt-Events oder plattformunabhängig.
            // Für den Beibehalt von GetAsyncKeyState unter Windows:
            bool bUp = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
            bool bDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
            bool bLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
            bool bRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

            // Wenn keine Taste mehr gedrückt wird, stoppen wir den Pan-Timer
            if (!bUp && !bDown && !bLeft && !bRight) {
                killTimer(m_panTimerId);
                m_panTimerId = 0;
                m_bPanTimerActive = false;
                return;
            }

            int PanXbase = 0;
            int PanYbase = 0;
            INT BorderDistanceY = 0;
            int PanX = 0;
            int PanY = 0;
            INT iRealHeight = INT(m_dZoom * (m_pCurrentImage->OrigHeight()));
            INT iRealWidth = INT(m_dZoom * (m_pCurrentImage->OrigWidth()));

            if (GetAutoZoomMode() == Helpers::ZM_BookMode && iRealHeight > m_clientRect.height() && iRealWidth <= m_clientRect.width()) {
                if ((bLeft && !bRight) || (bUp && !bDown)) PanYbase = 10;
                else if ((!bLeft && bRight) || (!bUp && bDown)) PanYbase = -10;
                else PanYbase = 0;
            } else {
                if (bUp && !bDown) PanYbase = 10;
                else if (!bUp && bDown) PanYbase = -10;
                else PanYbase = 0;

                if (bLeft && !bRight) PanXbase = 10;
                else if (!bLeft && bRight) PanXbase = -10;
                else PanXbase = 0;
            }

            if (PanXbase != 0 && PanYbase != 0) { // Diagonale Bewegung
                PanX = static_cast<int>(PanXbase * 0.7);
                PanY = static_cast<int>(PanYbase * 0.7);
            } else {
                PanX = PanXbase;
                PanY = PanYbase;
            }

            // Ausführen des Pans
            if (PerformPan(PanX, PanY, false)) {
                // Erzwingt ein sofortiges Neuzeichnen ohne den Event-Loop zu blockieren
                this->repaint();
            }
        }
    }
}

//LRESULT CMainDlg::OnContextMenu(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
/*
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
    if (m_bFullScreenMode) ::CheckMenuItem(hMenuZoom,  IDM_FULL_SCREEN_MODE, MF_CHECKED);
    if (!m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_SPAN_SCREENS, MF_BYCOMMAND | MF_GRAYED);
    
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
*/

void CMainDlg::onShowContextMenu(const QPoint &pos) {
    MouseOn();

    QMenu menu(this);

    // 1. Aktionen hinzufügen und konfigurieren (Zustände, Haken, Aktivierung)
    QAction* actReload = menu.addAction("Neu laden");
    actReload->setData(IDM_RELOAD_FULL);
    actReload->setEnabled(m_pCurrentImage != nullptr);

    // ... weitere Menüeinträge hinzufügen ...

    // 2. Direkt ausführen und auf globale Mausposition setzen
    QAction* pSelectedAction = menu.exec(pos);
	/*
    // 3. Befehl ausführen, falls etwas gewählt wurde
    if (pSelectedAction != nullptr) {
        int nCommand = pSelectedAction->data().toInt();
        if (nCommand > 0) {
            ExecuteCommand(nCommand);
        }
    }
	*/
}

///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void CMainDlg::ExecuteCommand(int nCommand) {
    CSettingsProvider& sp = CSettingsProvider::This();
    switch (nCommand) {
        case IDM_MINIMIZE:
            this->showMinimized();
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
                this->update();
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
                this->update();
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
                    bool bRecycleOnly = false;
                    if (!showDeleteConfirmationDialog({currentFileName}, bRecycleOnly)) {
                        break;
                    }
                }

                if (allowDelete) {
                    CFileList* fileListOfDeletedImage = m_pFileList;
                    GotoImage(POS_AwayFromCurrent, NO_REQUEST);
                    if (QFile::remove(currentFileName)) {
                        fileListOfDeletedImage->Reload(NULL, false);
                        m_pFileList->DeleteHistory(true);
                        update();
                        GotoImage(POS_Current);
                    } else {
                        update();
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
                m_dZoom = -1;
                m_bUserZoom = false;
                m_bUserPan = false;
                this->update();
            }
            break;
        case IDM_MIRROR_H:
        case IDM_MIRROR_V:
            if (m_pCurrentImage != NULL) {
                m_pCurrentImage->Mirror(nCommand == IDM_MIRROR_H);
                this->update();
            }
            break;
        case IDM_AUTO_CORRECTION:
            m_bAutoContrast = !m_bAutoContrast;
            SetToast(m_bAutoContrast ? QStringLiteral("Auto Contrast: ON") : QStringLiteral("Auto Contrast: OFF"));
            this->update();
            break;
        case IDM_KEEP_PARAMETERS:
            m_bKeepParams = !m_bKeepParams;
            if (m_bKeepParams) {
                *m_pImageProcParamsKept = *m_pImageProcParams;
                m_eProcessingFlagsKept = CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, m_bKeepParams);
                m_dZoomKept = m_bUserZoom ? m_dZoom : -1;
                m_offsetKept = m_bUserPan ? m_offsets : QPoint(0, 0);
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
                this->update();
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
        case IDM_FULL_SCREEN_MODE: {
            m_bFullScreenMode = !m_bFullScreenMode;
            m_dZoomAtResizeStart = 1.0;

            if (m_bFullScreenMode) {
                // Vor dem Umschalten ins Vollbild die aktuelle Position speichern (falls gewünscht)
                if (!isMaximized() && sp.ExplicitWindowRect()) {
                    QRect rect = this->geometry();
                    CMultiMonitorSupport::SetDefaultWindowRect(rect);
                }

                // Qt-Natives Vollbild (erkennt automatisch den aktuellen Monitor)
                this->showFullScreen();
                this->MouseOn();
            }
            else {
                // Vollbild verlassen und normales Fenster wiederherstellen
                this->showNormal();
/*
                if (sp.ExplicitWindowRect()) {
                    QRect defaultWindowRect = CMultiMonitorSupport::GetDefaultWindowRect();
                    double dZoom = -1;
                    // Deine Logik zur Größenberechnung beibehalten
                    QRect windowRect = GetWindowRectMatchingImageSize( // Gets a window rectangle (in screen coordinates) that fits the given image
                        this,
                        CSize(MIN_WND_WIDTH, MIN_WND_HEIGHT),
                        defaultWindowRect.size(),
                        dZoom, m_pCurrentImage, false, true, false, GetAutoZoomMode()
                        );
                    this->setGeometry(windowRect.left(), windowRect.top(), windowRect.width(), windowRect.height());
                }
*/

                UpdateWindowTitle(true); // Aktualisiert den Titel / das Icon der Titelleiste
                this->MouseOn();
            }

            m_dZoom = -1;
            StartLowQTimer(ZOOM_TIMEOUT);
            break;
        }
        case IDM_ZOOM_400:
            PerformZoom(4.0, false, m_bMouseOn);
            break;
        case IDM_ZOOM_200:
            PerformZoom(2.0, false, m_bMouseOn);
            break;
        case IDM_ZOOM_100:
            ResetZoomTo100Percents(m_bMouseOn);
            break;
        case IDM_ZOOM_50:
            PerformZoom(0.5, false, m_bMouseOn);
            break;
        case IDM_ZOOM_25:
            PerformZoom(0.25, false, m_bMouseOn);
            break;
        case IDM_ZOOM_INC:
        case IDM_ZOOM_DEC:
            PerformZoom((nCommand == IDM_ZOOM_INC) ? 1 : -1, true, m_bMouseOn);
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
                this->update();
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
                this->update();
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
                this->update();
            }
            break;
        case IDM_COLOR_CORRECTION_INC:
        case IDM_COLOR_CORRECTION_DEC:
            if (m_bAutoContrast) {
                double dInc = (nCommand == IDM_COLOR_CORRECTION_INC) ? 0.05 : -0.05;
                m_pImageProcParams->ColorCorrectionFactor = std::max(-0.5, std::min(0.5, m_pImageProcParams->ColorCorrectionFactor + dInc));
                this->update();
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
            this->update();
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

                    if (((iRealHeight > m_clientRect.height()) && (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN))
                     || (( iRealWidth >  m_clientRect.width()) && (nCommand == IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                m_panTimerId = startTimer(10);
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

                    if (((iRealHeight > m_clientRect.height()) && (nCommand == IDM_PAN_UP || nCommand == IDM_PAN_DOWN))
                     || (( iRealWidth >  m_clientRect.width()) && (nCommand == IDM_PAN_LEFT || nCommand == IDM_PAN_RIGHT))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                m_panTimerId = startTimer(10);
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
                            if (iRealHeight > m_clientRect.height()) {
                                if (m_bPanTimerActive == false) {
                                    if ((m_offsets.y() >= ((iRealHeight - m_clientRect.height()) / 2)) && (nCommand == IDM_PAN_LEFT)) {    // Upper Border!
                                        GotoImage(POS_Previous);
                                    } else if ((-m_offsets.y() >= ((iRealHeight - m_clientRect.height()) / 2)) && (nCommand == IDM_PAN_RIGHT)) {    //Lower Border!
                                        GotoImage(POS_Next);
                                    } else {
                                        if (m_bAllowSmoothPanning) {
                                            m_bPanTimerActive = true;
                                            m_panTimerId = startTimer(10);
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
            QPoint clientPoint(m_nMouseX, m_nMouseY);
            QPoint screenPoint = this->mapToGlobal(clientPoint);
            onShowContextMenu(screenPoint);
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
            this->update();
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

    // 1. Vollbildmodus verlassen (falls aktiv)
    if (m_bFullScreenMode) {
        m_bFullScreenMode = false;

        // Qt-Äquivalent für das Wiederherstellen des normalen Fensters
        showNormal();

        // Falls du die vorherige Geometrie gespeichert hattest, kannst du sie hier via restoreGeometry() setzen.

        MouseOn();

        m_dZoom = -1;
        m_bInZooming = true;
        StartLowQTimer(ZOOM_TIMEOUT);
    }

    // 2. Datei im Editor öffnen
    if (m_bIsRunningInWine) {
        QString sUnixPath = GetUnixPathFromWine(sCurrentFileName);
        if (!sUnixPath.isEmpty()) {
            // Unter Wine via QProcess starten statt rohem ShellExecuteW
            QStringList arguments;
            arguments << "/unix" << "/usr/bin/gimp" << sUnixPath;
            QProcess::startDetached("start.exe", arguments);
        }
    }
    else {
        // Der moderne, plattformunabhängige Qt-Weg, um Dateien mit der Standard-App zu öffnen
        // (Ersetzt das Windows-spezifische "ShellExecuteW" mit dem "edit"-Verb)
        QDesktopServices::openUrl(QUrl::fromLocalFile(sCurrentFileName));
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
            if (IsLinuxBinaryAvailable(QStringLiteral("~/.local/bin/mkFolderWidget"))) {
                QStringList args;
                args << QStringLiteral("/unix") << QStringLiteral("/bin/sh") << QStringLiteral("-c")
                     << QStringLiteral("mkFolderWidget -f \"%1\"").arg(sUnixPath);
                QProcess::startDetached(QStringLiteral("start.exe"), args);
            }
            else if (IsLinuxBinaryAvailable(QStringLiteral("/usr/bin/dolphin"))) {
                QStringList args;
                args << QStringLiteral("/unix") << QStringLiteral("/usr/bin/dolphin")
                     << QStringLiteral("--new-window") << QStringLiteral("--select") << sUnixPath;
                QProcess::startDetached(QStringLiteral("start.exe"), args);
            }
            else {
                const QString sFolderPath = QFileInfo(sCurrentFileName).path();
                QStringList args;
                args << sFolderPath;
                QProcess::startDetached(QStringLiteral("winebrowser.exe"), args);
            }
        }
    } else {
        // Nativer Windows-Build: explorer.exe mit /select über QProcess aufrufen
        QString nativePath = QDir::toNativeSeparators(sCurrentFileName);
        QStringList args;
        args << QStringLiteral("/select,%1").arg(nativePath);

        QProcess::startDetached(QStringLiteral("explorer.exe"), args);
    }
#elif defined(Q_OS_LINUX)
    // --- Nativer Linux-Build (ohne Wine / reines Qt6) ---
    if (QFileInfo(sCurrentFileName).exists()) {
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
                                                    CreateProcessParams(!m_bFullScreenMode && bAfterStartup),
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    m_nLastLoadError = GetLoadErrorAfterOpenFile();

    AfterNewImageLoaded(true, false);
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        StartAnimation();
    }
    m_startMouse.x = m_startMouse.y = -1;
    MouseOff();
    this->update();
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
    if (m_DIBOffsets != QPoint(0, 0)) {
        update();
    }
    SetCursorForMoveSection();
}

void CMainDlg::GotoImage(EImagePosition ePos) {
    GotoImage(ePos, 0);
}

void CMainDlg::GotoImage(EImagePosition ePos, int nFlags) {
/* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos1";

    // Timer handling for slideshows
    if (ePos == POS_Next || ePos == POS_NextSlideShow) {
        if (m_nCurrentTimeout > 0) {
            StartSlideShowTimer(m_nCurrentTimeout);
        }
        StopAnimation();
        m_customAnimationFrameTime = 1000;

/* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos1a (ePos == POS_Next || ePos == POS_NextSlideShow)";

    } else if (ePos != POS_NextFrame) {
        StopMovieMode();
        StopAnimation();
        m_customAnimationFrameTime = 1000;

/* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos1b (ePos != POS_NextFrame)";
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

        /* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos4 m_offsets.x:" <<  m_offsets.x() << "m_offsets.y:" << m_offsets.y() << "nOldFrameIndex:" << nOldFrameIndex << "nFrameIndex:" << nFrameIndex;

    if (bCheckIfSameImage && (m_pFileList == pOldFileList && (nOldFrameIndex == nFrameIndex || bNoWrapAroundEdgeFrame) && !m_pFileList->ChangedSinceCheckpoint())) {
        if (m_bMovieMode && m_bAutoExit) {
            CleanupAndTerminate();
        } else {
/* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos4b  bCheckIfSameImage:" << bCheckIfSameImage << "bNoWrapAroundEdgeFrame:" << bNoWrapAroundEdgeFrame << "m_pFileList->ChangedSinceCheckpoint():" << m_pFileList->ChangedSinceCheckpoint();
            return; // not placed on a new image, don't do anything
        }
    }

    if (ePos != POS_Current && ePos != POS_NextFrame && ePos != POS_AwayFromCurrent) {
        MouseOff();
    }

    if (nFlags & KEEP_PARAMETERS) {
        if (!m_bUserZoom) {
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

/* Debugging */ qDebug() << "GotoImage()->RequestImage()  nFrameIndex:" << nFrameIndex << "of" << m_pFileList->Current();

    m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList,
                                                    (ePos == POS_AwayFromCurrent) ? CJPEGProvider::NONE : eDirection,
                                                    m_pFileList->Current(),
                                                    nFrameIndex,
                                                    procParams,
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    m_nLastLoadError = (m_pCurrentImage == NULL) ? ((m_pFileList->Current().isEmpty()) ? FileLoad_NoFilesInDirectory : FileLoad_LoadError) : FileLoad_Ok;

    /* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos6  m_offsets.x:" << m_offsets.x() << "m_offsets.y:" << m_offsets.y() << "nOldFrameIndex:" << nOldFrameIndex << "nFrameIndex:" << nFrameIndex << "of" << m_pFileList->Current();

    bool bSynchronize = (nFlags & KEEP_PARAMETERS) == 0;

    AfterNewImageLoaded(bSynchronize, false, ePos == POS_Previous);

    /* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos7  m_offsets.x:" << m_offsets.x() << "m_offsets.y:" << m_offsets.y() << "nOldFrameIndex:" << nOldFrameIndex << "nFrameIndex:" << nFrameIndex << "of" << m_pFileList->Current();

    // If current image is animated, start playback
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        if (m_bIsAnimationPlaying) {
            AdjustAnimationFrameTime();
        } else {
            StartAnimation();
        }
    }

    /* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos8  m_offsets.x:" << m_offsets.x() << "m_offsets.y:" << m_offsets.y();

    if (((nFlags & NO_UPDATE_WINDOW) == 0) && !(ePos == POS_NextSlideShow && UseSlideShowTransitionEffect())) {
        this->update();
        // this will force to wait until really redrawn, preventing to process images but do not show them
        this->repaint();
    }

/* Debugging */ qDebug() << "GotoImage() Pos9 after repaint() nFrameIndex:" << nFrameIndex << "of" << m_pFileList->Current();
}

void CMainDlg::ReloadImage(bool keepParameters, bool updateWindow) {
    int flags = keepParameters ? KEEP_PARAMETERS : 0;
    GotoImage(CMainDlg::POS_Current, updateWindow ? flags : flags | NO_UPDATE_WINDOW);
}

// returns true on success, false if nothing was done
bool CMainDlg::PerformZoom(double dValue, bool bExponent, bool bZoomToMouse) {
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
        int nOldX = nOldXSize/2 - m_clientRect.width()/2 + nCenterX - m_offsets.x();
        int nOldY = nOldYSize/2 - m_clientRect.height()/2 + nCenterY - m_offsets.y();
        double dFac = m_dZoom/dOldZoom;
        m_offsets.setX(Helpers::RoundToInt(nNewXSize/2 - m_clientRect.width()/2 + nCenterX - nOldX*dFac));
        m_offsets.setY(Helpers::RoundToInt(nNewYSize/2 - m_clientRect.height()/2 + nCenterY - nOldY*dFac));
    } else {
        // zoom to center
        m_offsets.setX(Helpers::RoundToInt(m_offsets.x() * m_dZoom / dOldZoom));
        m_offsets.setY(Helpers::RoundToInt(m_offsets.y() * m_dZoom / dOldZoom));
    }

    m_bInZooming = true;
    StartLowQTimer(ZOOM_TIMEOUT);
    if (fabs(dOldZoom - m_dZoom) > 0.0001 || m_bZoomMode) {
        this->update();

        // Recalculate the new m_nBookModePageHeight based on the current zoom m_dZoom
        // Todo: Save changes to user ini
        if ((GetAutoZoomMode() == Helpers::ZM_BookMode) && (m_bZoomMode == false)) {
            int nImageW = m_pCurrentImage->OrigWidth();
            int nImageH = m_pCurrentImage->OrigHeight();
            int nWinW = m_clientRect.width();
            int nWinH = m_clientRect.height();

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
    if ((m_virtualImageSize.width() > 0 && m_virtualImageSize.width() > m_clientRect.width()) ||
        (m_virtualImageSize.height() > 0 && m_virtualImageSize.height() > m_clientRect.height())) {
        if (bAbsolute) {
            m_offsets = QPoint(dx, dy);
        } else {
            m_offsets = QPoint(m_offsets.x() + dx, m_offsets.y() + dy);
        }
        m_bUserPan = true;

        this->update();
        return true;
    }
    return false;
}

double CMainDlg::GetZoomFactorForFitToScreen(bool bFillWithCrop, bool bAllowEnlarge) {
    if (m_pCurrentImage != NULL) {
        double dZoom;
        Helpers::GetImageRect(m_pCurrentImage->OrigWidth(), m_pCurrentImage->OrigHeight(), 
            m_clientRect.width(), m_clientRect.height(), bAllowEnlarge, bFillWithCrop, false, GetAutoZoomMode(), dZoom);
        double dZoomMax = bAllowEnlarge ? Helpers::ZoomMax : std::max(1.0, m_dZoomAtResizeStart);
        return std::max(0.0001, std::min(dZoomMax, dZoom));
    } else {
        return 1.0;
    }
}

void CMainDlg::ResetZoomToFitScreen(bool bFillWithCrop, bool bAllowEnlarge, bool bAdjustWindowSize) {
    m_isUserFitToScreen = false;
    if (m_pCurrentImage != NULL) {
		double dOldZoom = m_dZoom;
		m_dZoom = GetZoomFactorForFitToScreen(bFillWithCrop, bAllowEnlarge);
		if (bAllowEnlarge) {
			m_isUserFitToScreen = true;
			m_dZoom = -1;
			m_autoZoomFitToScreen = bFillWithCrop ? Helpers::ZM_FillScreen : Helpers::ZM_FitToScreen;
		}
		m_bUserZoom = m_dZoom > 1.0;
		m_bUserPan = false;
		if (fabs(dOldZoom - m_dZoom) > 0.01) {
			//m_bInZooming = true;    // Disabled, since this would trigger a display of the Zoom Factor text at ANY change in zoom (fullscreen, onsize() etc.)
			StartLowQTimer(ZOOM_TIMEOUT);
			this->update();
		}
    }
}

void CMainDlg::ResetZoomTo100Percents(bool bZoomToMouse) {
    if (m_pCurrentImage != NULL && fabs(m_dZoom - 1) > 0.01) {
        // the current design (unless changed) cursor always shows in windowed mode, so always zoom to cursor when not fullscreen
        PerformZoom(1.0, false, bZoomToMouse || !m_bFullScreenMode);
    }
}

CProcessParams CMainDlg::CreateProcessParams(bool bNoProcessingAfterLoad) {
    int nClientWidth = m_clientRect.width();
    int nClientHeight = m_clientRect.height();
	
    if (nClientWidth == 0 && nClientHeight == 0) {
        QRect rect = CMultiMonitorSupport::GetMonitorRect(this);
        nClientWidth = rect.width();
        nClientHeight = rect.height();
    }
	
    Helpers::EAutoZoomMode eAutoZoomMode = GetAutoZoomMode();
	
    if (m_bKeepParams) {
        *m_pImageProcParamsKept = *m_pImageProcParams;
        m_eProcessingFlagsKept = CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, m_bKeepParams);
        m_dZoomKept = m_bUserZoom ? m_dZoom : -1;
        m_offsetKept = m_bUserPan ? m_offsets : QPoint(0, 0);
		
        if (m_isUserFitToScreen) {
			eAutoZoomMode = m_autoZoomFitToScreen;
		}

        return CProcessParams(nClientWidth, nClientHeight,
            CMultiMonitorSupport::GetMonitorRect(this).size(),
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
            CMultiMonitorSupport::GetMonitorRect(this).size(),
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
    m_slideshowTimerId = startTimer(nMilliSeconds);
    m_nLastSlideShowImageTickCount = QDateTime::currentMSecsSinceEpoch();
}

void CMainDlg::StopSlideShowTimer(void) {
    if (m_nCurrentTimeout > 0) {
        m_nCurrentTimeout = 0;

        if (m_slideshowTimerId != 0) {
            killTimer(m_slideshowTimerId);
            m_slideshowTimerId = 0;
        }
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
    update();
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
        this->update();
    }
}

void CMainDlg::StartLowQTimer(int nTimeout) {
    m_bTemporaryLowQ = true;

    // Alten Zoom-Timer stoppen, falls er bereits läuft
    if (m_zoomTimerId != 0) {
        killTimer(m_zoomTimerId);
        m_zoomTimerId = 0;
    }

    // Neuen Timer starten und die ID in unserer Member-Variable speichern
    m_zoomTimerId = startTimer(nTimeout);
}

void CMainDlg::MouseOff() {
    if (m_bMouseOn) {
        // Prüfen, ob aktuell ein Menü/Popup offen ist (ersetzt m_bInTrackPopupMenu)
        if (QApplication::activePopupWidget() != nullptr) {
            return;
        }

        if (m_nMouseY < this->rect().bottom()) {
            if (m_bFullScreenMode) {
                // Cursor nur im Vollbildmodus verstecken (Qt plattformübergreifend)
                this->setCursor(Qt::BlankCursor);
            }

            m_startMouse.x = m_startMouse.y = -1;
            // Verwendung von QPoint statt Win32-Structs
            //m_startMouse.setX(-1);
            //m_startMouse.setY(-1);
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
    } else if (!m_bUserZoom) {
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
    this->update();
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

QRect CMainDlg::GetZoomTextRect() {
    int nEndX = m_clientRect.right() - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_OFFSET_X);
    int nStartX = nEndX - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_WIDTH);

    int nEndY = m_clientRect.bottom() - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_OFFSET_Y);
    int nStartY = nEndY - Helpers::ScaleWithDPI(ZOOM_TEXT_RECT_HEIGHT);

    return QRect(nStartX, nStartY, nEndX - nStartX, nEndY - nStartY);
}

QRect CMainDlg::GetToastTextRect() {
    int nEndX = m_clientRect.right() - Helpers::ScaleWithDPI(TOAST_TEXT_RECT_OFFSET_X);
    int nStartX = m_clientRect.right() / 2;
    
    int nStartY = Helpers::ScaleWithDPI(TOAST_TEXT_RECT_OFFSET_Y);
    int nEndY = nStartY + Helpers::ScaleWithDPI(TOAST_TEXT_RECT_HEIGHT);
    
    return QRect(nStartX, nStartY,  nEndX - nStartX, nEndY - nStartY);
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

    // 1. Fenstertitel aktualisieren
    QString sWindowText;
    if (sCurrentFileName.isEmpty() || m_pCurrentImage == nullptr) {
        sWindowText = QStringLiteral(JPEGVIEW_TITLE);
    } else {
        QString visibleName = QDir::toNativeSeparators(sCurrentFileName);
        if (m_bIsRunningInWine && bShowFullPathInTitle) {
            QString sUnixPath = GetUnixPathFromWine(sCurrentFileName);
            if (!sUnixPath.isEmpty()) {
                visibleName = sUnixPath;
            }
        }
        sWindowText = visibleName
                      + QString(GetMultiframeIndex(m_pCurrentImage))
                      + QStringLiteral(" - ")
                      + QStringLiteral(JPEGVIEW_TITLE);
    }

    if ((s_PrevTitleText != sWindowText) || bForce) {
        s_PrevTitleText = sWindowText;
        // Qt-native Methode zum Setzen des Fenstertitels
        this->setWindowTitle(sWindowText);
    }

    // 2. Titelbild-Icon aktualisieren
    if (CSettingsProvider::This().TitleBarUseFileIcon()) {
        const QString sCurrentFilePath = CurrentFileName(false);
        QString cacheKey = QStringLiteral("__default__");
        QString currentCheckExt = QStringLiteral("__default__");

        if (!sCurrentFilePath.isEmpty() && m_pCurrentImage != nullptr) {
            const QFileInfo fileInfo(sCurrentFilePath);
            const QString ext = fileInfo.suffix(); // z.B. "jpg"

            if (!ext.isEmpty()) {
                currentCheckExt = ext.toLower();
                cacheKey = currentCheckExt;
            }
        }

        if ((s_PrevFileExt != currentCheckExt) || bForce) {
            s_PrevFileExt = currentCheckExt;

            // Statischer Icon-Cache für QIcon-Objekte (ersetzt den alten HICON-Cache)
            static QMap<QString, QIcon> s_qtIconCache;

            if (!s_qtIconCache.contains(cacheKey)) {
                QIcon fileIcon;
                if (cacheKey == QStringLiteral("__default__")) {
                    // Standard-App-Icon laden (z.B. aus Qt-Ressourcen)
                    fileIcon = QIcon(QStringLiteral(":/icons/app_icon.ico"));
                } else {
                    // QFileIconProvider holt das System-Dateityp-Icon komplett plattformunabhängig!
                    QFileIconProvider iconProvider;
                    QFileInfo fileInfo(sCurrentFilePath);
                    if (fileInfo.exists()) {
                        fileIcon = iconProvider.icon(fileInfo);
                    } else {
                        // Fallback über eine temporäre Dummy-Datei mit der Endung
                        QFileInfo dummyInfo(QStringLiteral("dummy.") + cacheKey);
                        fileIcon = iconProvider.icon(dummyInfo);
                    }
                }
                s_qtIconCache[cacheKey] = fileIcon;
            }

            // Setzt das Fenster-Icon direkt über Qt
            this->setWindowIcon(s_qtIconCache[cacheKey]);
        }
    }
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

void CMainDlg::PrefetchDIB(const QRect& clientRect) {
    if (m_pCurrentImage == NULL) return;

    QSize newSize = GetVirtualImageSize(m_pCurrentImage->OrigSize(), clientRect.size(), GetAutoZoomMode(), m_dZoom);
    QSize clippedSize(std::min<long>(clientRect.width(), newSize.width()), std::min<long>(clientRect.height(), newSize.height()));
    CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, LimitOffsets(m_offsets, clientRect.size(), newSize));
    m_pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage, *m_pImageProcParams, CreateDefaultProcessingFlags());
}

double CMainDlg::GetZoomMultiplier(CJPEGImage* pImage, const QRect& clientRect) {
    double dZoomToFit;

    Helpers::GetImageRect(pImage->OrigWidth(),
                          pImage->OrigHeight(),
                          clientRect.width(),
                          clientRect.height(),
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
    return m_nCurrentTimeout >= 1000 && m_eTransitionEffect != Helpers::TE_None;
}
/*
void CMainDlg::AnimateTransition() {
    if (m_eTransitionEffect == Helpers::TE_None) {
        update();
        return;
    }

    int nW = width();
    int nH = height();
    if (nW <= 0 || nH <= 0) return;

    int nFrameTimeMs = (m_eTransitionEffect >= Helpers::TE_RollLR || m_eTransitionEffect <= Helpers::TE_ScrollBT) ? 10 : 20;

    // 1. Aktuelles Bild (Vorher-Zustand) als Screenshot sichern
    QImage oldImage = this->grab().toImage();

    // 2. Neues Bild (Nachher-Zustand) in ein Offscreen-Image rendern
    QImage memImage(nW, nH, QImage::Format_ARGB32_Premultiplied);
    memImage.fill(Qt::black);
    {
        QPainter memPainter(&memImage);
        memPainter.setRenderHint(QPainter::Antialiasing, true);
        PaintToPainter(memPainter);
    }

    int nSteps = std::max(1, (m_nTransitionTime + 20) / nFrameTimeMs);
    qint64 lastTime = QDateTime::currentMSecsSinceEpoch();

    for (int i = 0; i <= nSteps; i++) {
        QImage frameImage(nW, nH, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&frameImage);

        switch (m_eTransitionEffect) {
        case Helpers::TE_Blend:
            {
                painter.drawImage(0, 0, oldImage);
                if (i == nSteps) {
                    painter.drawImage(0, 0, memImage);
                } else {
                    float fFactor = (float)i / nSteps;
                    float fCurve = fFactor * fFactor * i; // Behält die originale Beschleunigungskurve bei
                    int alpha = std::min(255, (int)((fCurve + 1) * (255.0f / nSteps) + 0.5f));
                    painter.setOpacity(alpha / 255.0;
                    painter.drawImage(0, 0, memImage);
                }
                break;
            }
        case Helpers::TE_SlideLR:
        case Helpers::TE_SlideRL:
        case Helpers::TE_SlideBT:
        case Helpers::TE_SlideTB:
            {
                painter.drawImage(0, 0, oldImage);
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nFracHeight = (int)(nH * fFactor + 0.5f);
                int nFracWidth = (int)(nW * fFactor + 0.5f);
                int nStartX = (m_eTransitionEffect == Helpers::TE_SlideLR) ? nFracWidth - nW : (m_eTransitionEffect == Helpers::TE_SlideRL) ? nW - nFracWidth : 0;
                int nStartY = (m_eTransitionEffect == Helpers::TE_SlideTB) ? nFracHeight - nH : (m_eTransitionEffect == Helpers::TE_SlideBT) ? nH - nFracHeight : 0;

                painter.drawImage(nStartX, nStartY, memImage);
                break;
            }
        case Helpers::TE_RollLR:
        case Helpers::TE_RollRL:
        case Helpers::TE_RollBT:
        case Helpers::TE_RollTB:
            {
                painter.drawImage(0, 0, oldImage);
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nFracHeight = (int)(nH * fFactor + 0.5f);
                int nClipH = (m_eTransitionEffect == Helpers::TE_RollLR || m_eTransitionEffect == Helpers::TE_RollRL) ? nH : 1 + nH / (nSteps + 1);
                int nFracWidth = (int)(nW * fFactor + 0.5f);
                int nClipW = (m_eTransitionEffect == Helpers::TE_RollTB || m_eTransitionEffect == Helpers::TE_RollBT) ? nW : 1 + nW / (nSteps + 1);
                int nClipX = (m_eTransitionEffect == Helpers::TE_RollLR) ? nFracWidth - nClipW : (m_eTransitionEffect == Helpers::TE_RollRL) ? nW - nFracWidth : 0;
                int nClipY = (m_eTransitionEffect == Helpers::TE_RollTB) ? nFracHeight - nClipH : (m_eTransitionEffect == Helpers::TE_RollBT) ? nH - nFracHeight : 0;

                painter.setClipRect(nClipX, nClipY, nClipW, nClipH);
                painter.drawImage(0, 0, memImage);
                break;
            }
        case Helpers::TE_ScrollLR:
        case Helpers::TE_ScrollRL:
        case Helpers::TE_ScrollBT:
        case Helpers::TE_ScrollTB:
            {
                float fFactorLast = (float)i / (nSteps + 1);
                float fFactor = (float)(i + 1) / (nSteps + 1);
                int nScrollW = 0, nScrollH = 0;
                int nStartX = 0, nStartY = 0;
                QRect clipRect;

                if (m_eTransitionEffect == Helpers::TE_ScrollLR) {
                    nScrollW = (int)(nW * fFactor + 0.5f) - (int)(nW * fFactorLast + 0.5f);
                    nStartX = (int)(nW * fFactor + 0.5f) - nW;
                    clipRect = QRect(0, 0, nScrollW, nH);
                } else if (m_eTransitionEffect == Helpers::TE_ScrollRL) {
                    nScrollW = -((int)(nW * fFactor + 0.5f) - (int)(nW * fFactorLast + 0.5f));
                    nStartX = nW - (int)(nW * fFactor + 0.5f);
                    clipRect = QRect(nW + nScrollW, 0, -nScrollW, nH);
                } else if (m_eTransitionEffect == Helpers::TE_ScrollTB) {
                    nScrollH = (int)(nH * fFactor + 0.5f) - (int)(nH * fFactorLast + 0.5f);
                    nStartY = (int)(nH * fFactor + 0.5f) - nH;
                    clipRect = QRect(0, 0, nW, nScrollH);
                } else {
                    nScrollH = -((int)(nH * fFactor + 0.5f) - (int)(nH * fFactorLast + 0.5f));
                    nStartY = nH - (int)(nH * fFactor + 0.5f);
                    clipRect = QRect(0, nH + nScrollH, nW, -nScrollH);
                }

                // Alten Inhalt verschoben zeichnen + neuen Teil einblenden
                painter.drawImage(nScrollW, nScrollH, oldImage);
                painter.setClipRect(clipRect);
                painter.drawImage(nStartX, nStartY, memImage);
                break;
            }
        default:
            painter.drawImage(0, 0, memImage);
            break;
        }

        // Frame direkt auf das Widget zeichnen
        {
            QPainter widgetPainter(this);
            widgetPainter.drawImage(0, 0, frameImage);
        }

        // Timing & Event-Loop Verarbeiten (damit das UI während der Animation nicht einfriert)
        qint64 time = QDateTime::currentMSecsSinceEpoch();
        qint64 elapsed = time - lastTime;
        if (elapsed < nFrameTimeMs) {
            QThread::msleep(nFrameTimeMs - elapsed);
        }
        lastTime = QDateTime::currentMSecsSinceEpoch();

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    // Am Ende einmal regulär aktualisieren
    update();
}
*/
void CMainDlg::CleanupAndTerminate() {
    SaveBookmark();
    CSettingsProvider::This().SaveCustomSettings();
    StopMovieMode();
    StopAnimation();
    delete m_pJPEGProvider; // delete this early to properly shut down the loading threads
    m_pJPEGProvider = NULL;
    close();
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
    m_nLastSlideShowImageTickCount = QDateTime::currentMSecsSinceEpoch();

    m_nLastAnimationOffset = 0;
    
    int usedFrameTime;
    if (m_customAnimationFrameTime < 250) {
        usedFrameTime = m_customAnimationFrameTime;
    } else {
        usedFrameTime = m_pCurrentImage->FrameTimeMs();
    }
    
    int nTimerDelay = std::max(10, m_pCurrentImage->FrameTimeMs());
    m_nExpectedNextAnimationTickCount = QDateTime::currentMSecsSinceEpoch() + nTimerDelay;
    m_animationTimerId = startTimer(nTimerDelay);
}

void CMainDlg::AdjustAnimationFrameTime() {
    // restart timer with new frame time
    if (m_animationTimerId != 0) {
        killTimer(m_animationTimerId);
        m_animationTimerId = 0;
    }
    
    int usedFrameTime;
    if (m_customAnimationFrameTime < 250) {
        usedFrameTime = m_customAnimationFrameTime;
    } else {
        usedFrameTime = m_pCurrentImage->FrameTimeMs();
    }
    

    m_nLastAnimationOffset = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - m_nExpectedNextAnimationTickCount);
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
        m_animationTimerId = startTimer(nTimerDelay);
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

    // Animations-Timer über die Qt-interne ID stoppen
    if (m_animationTimerId != 0) {
        killTimer(m_animationTimerId);
        m_animationTimerId = 0;
    }

    m_bIsAnimationPlaying = false;
}

void CMainDlg::onRefreshView() {
    ExecuteCommand(IDM_RELOAD_FULL);
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

void CMainDlg::SetToast(const QString& strToast, int nDurationMs)
{
    // Alten Toast-Timer stoppen, falls er noch läuft
    if (m_toastTimerId != 0) {
        killTimer(m_toastTimerId);
        m_toastTimerId = 0;
    }

    // Neuen Timer starten und ID speichern
    m_toastTimerId = startTimer(nDurationMs);

    m_strToast = strToast;
    this->update();
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

QString CMainDlg::FormatFileSize(qint64 bytes) {
    static const QLocale locale = QLocale::system();

    if (bytes < 1024) {
        return locale.toString(bytes) + " Bytes";
    }

    double size = static_cast<double>(bytes);
    static const QStringList units = {"Bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    int unitIndex = 0;

    while (size >= 1024.0 && unitIndex < units.size() - 1) {
        size /= 1024.0;
        unitIndex++;
    }

    int precision = 0;
    if (size < 10.0) {
        precision = 2; // z.B. 1,23 MiB
    } else if (size < 100.0) {
        precision = 1; // z.B. 12,3 MiB
    } else {
        precision = 0; // z.B. 123 MiB
    }

    return locale.toString(size, 'f', precision) + " " + units[unitIndex];
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
#if defined(Q_OS_WIN)
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
#endif
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

bool CMainDlg::showDeleteConfirmationDialog(const QStringList &pathList, bool bRecycleOnly) {
    QString sTitle;
    QString sText;
    QString sWarning;
    QMessageBox::Icon iIcon;

    if (bRecycleOnly) {
        iIcon = QMessageBox::Question;
        sWarning = "";
        if (pathList.size() == 1) {
            sTitle = tr("Delete File");
            sText = tr("Do you really want to move this file into the recycle bin?");
        } else {
            sTitle = tr("Delete multiple elements");
            sText = QString(tr("Do you really want to move these %1 files into the recycle bin?")).arg(pathList.size());
        }
    } else {
        iIcon = QMessageBox::Warning;
        sWarning = "<p style='color: red;'><i>" + tr("This process cannot be undone.") + "</i></p>";
        if (pathList.size() == 1) {
            sTitle = tr("Delete File");
            sText = tr("Are you sure you want to delete this file permanently?");
        } else {
            sTitle = tr("Delete multiple elements");
            sText = QString(tr("Are you sure you want to delete these %1 files permanently?")).arg(pathList.size());
        }
    }

    QMessageBox msgBox(nullptr); // todo: use "this" instead of "nullptr"
    msgBox.setWindowTitle(sTitle);
    msgBox.setIcon(iIcon);

    if (pathList.size() == 1) {
        QFileInfo fileInfo(pathList.first());
        QString fileName = fileInfo.fileName();
        QString size = FormatFileSize(fileInfo.size());
        QString lastModified = fileInfo.lastModified().toString("yyyy-MM-dd  HH:mm:ss");

        QIcon icon = m_iconProvider.icon(fileInfo);
        QPixmap pix = icon.pixmap(QSize(48, 48));
        /*
        if (!Helpers::hasIconExt(fileInfo)) {
            QPixmap thumb = Helpers::generateThumbnail(fileInfo);
            if (!thumb.isNull()) {
                pix = thumb;
            }
        }
        */

        QByteArray ba;
        QBuffer bu(&ba);
        pix.save(&bu, "PNG");
        QString imgBase64 = ba.toBase64();

        msgBox.setText(QString("<h3>%1</h3>").arg(sText));

        const QString htmlTemplate = QStringLiteral(R"(
            <table width='100%' cellspacing='0' cellpadding='0'>
                <tr>
                    <td rowspan='4' width='48' valign='top' style='padding-right: 10px;'>
                        <img src='data:image/png;base64,%1'>
                    </td>
                    <td style='color: #555; padding: 2px 8px;' width='1%'>%2</td>
                    <td style='color: #555; padding: 2px 8px;'>%3</td>
                </tr>
                <tr>
                    <td style='color: #555; padding: 2px 8px;'>%4</td>
                    <td style='color: #555; padding: 2px 8px;'>%5</td>
                </tr>
                <tr>
                    <td style='color: #555; padding: 2px 8px;'>%6</td>
                    <td style='color: #555; padding: 2px 8px;'>%7</td>
                </tr>
                <tr>
                    <td colspan='2' style='padding: 8px 8px 2px 8px;'>%8</td>
                </tr>
            </table>
            )");


        msgBox.setInformativeText(htmlTemplate.arg(
            imgBase64,
            tr("Name:"),
            fileName,
            tr("Size:"),
            size,
            tr("Date:"),
            lastModified,
            sWarning
            ));
    } else {
        msgBox.setText(QString("<h3>%1</h3>").arg(sText));
        msgBox.setInformativeText(sWarning);
    }

    QPushButton *deleteButton = msgBox.addButton(tr("Delete"), QMessageBox::AcceptRole);
    msgBox.addButton(tr("Cancel"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(deleteButton);
    deleteButton->setStyleSheet("QPushButton { font-weight: bold; min-width: 80px; }");

    msgBox.exec();

    if (msgBox.clickedButton() != deleteButton) {
        return false;
    }

    return true;
}

// MainDlg.cpp : implementation of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#include "MainDlg.h"
#include "FileList.h"
#include "ImageProvider.h"
#include "BaseImage.h"
#include "SettingsProvider.h"
#include "MultiMonitorSupport.h"
#include "HistogramCorr.h"
#include "Clipboard.h"
#include "ProcessParams.h"
#include "ResizeFilter.h"
#include "ProcessingThreadPool.h"
#include "KeyMap.h"
#include "Commands.h"
#include "DirectoryWatcher.h"
#include "RenameDialog.h"

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QtGlobal>
#include <QLibrary>
#include <QVariant>
#include <optional>

// Damit CommandInvocation als QVariant an QActions haengen kann (Kontextmenue).
Q_DECLARE_METATYPE(Cmd::CommandInvocation)

namespace {

    const int MIN_WND_WIDTH = 320;
    const int MIN_WND_HEIGHT = 240;

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

    const QString APP_TITLE = "mkImageViewer";

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

    QString FormatFileSize(qint64 bytes) {
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

    // Gets default image processing parameters as set in INI file
    CImageProcessingParams GetDefaultProcessingParams() {
        CSettingsProvider& sp = CSettingsProvider::This();
        return CImageProcessingParams(
            sp.Contrast(),
            sp.Gamma(),
            sp.Saturation(),
            0.5); // ContrastCorrectionFactor
    }

    // Gets hardcoded processing parameters, turning off all processing
    CImageProcessingParams GetNoProcessingParams() {
        return CImageProcessingParams(0.0, 1.0, 1.0, 0.5);
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

    // Draws an error text for the given file loading error, and detailed load error code (bit combination of EFileLoadError codes)
    void DrawImageLoadErrorText(QPainter& painter, const QRect& clientRect, const QString& failedFileName, int nFileLoadError, int nLoadErrorDetail) {
        const bool bOutOfMemory = (nLoadErrorDetail & FileLoad_OutOfMemory) != 0;
        const bool bExceptionError = (nLoadErrorDetail & FileLoad_ExceptionError) != 0;

        QString message;

        switch (nFileLoadError) {
        case FileLoad_LoadError:
            {
            QFileInfo fileInfo(failedFileName);
            QString sFileSize = FormatFileSize(fileInfo.size());
            message = QStringLiteral("File could not be read:\n\n%1\nFile size: %2").arg(failedFileName, sFileSize);
            break;
            }

        case FileLoad_SlideShowListInvalid:
            message = QStringLiteral("File does not contain a list of file names:\n\n%1").arg(failedFileName);
            break;

        case FileLoad_NoFilesInDirectory:
            message = QStringLiteral("Directory does not contain any image files to display:\n\n%1").arg(failedFileName);
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
            if (bExceptionError) {
                message += QLatin1Char('\n');
                message += QStringLiteral("An exception error has occured.");
            }
        }

        QRect rectText(0, clientRect.height() / 2 - Helpers::ScaleWithDPI(40), clientRect.width(), clientRect.height());

        QFont font = QApplication::font();
        //font.setBold(true);
        font.setPointSize(10);
        painter.setFont(font);
        painter.setPen(Qt::red);
        painter.drawText(rectText, Qt::AlignHCenter | Qt::TextWordWrap, message);
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

    m_pImageProcParams = new CImageProcessingParams(GetDefaultProcessingParams());
    InitFromProcessingFlags(GetDefaultProcessingFlags(), m_bHQResampling, m_bAutoContrast);

    m_pImageProcParamsKept = new CImageProcessingParams();
    m_eProcessingFlagsKept = PFLAG_HighQualityResampling;
    m_dZoomKept = -1;
    m_offsetKept = CPoint(0, 0);

    m_bKeepParams = sp.KeepParams();
    m_eAutoZoomModeWindowed = sp.AutoZoomMode();
    m_eAutoZoomModeFullscreen = sp.AutoZoomModeFullscreen();

    m_eTransitionEffect = sp.SlideShowTransitionEffect();
    m_nTransitionTime = sp.SlideShowEffectTimeMs();

    CHistogramCorr::SetContrastCorrectionStrength((float)sp.AutoContrastAmount());
    CHistogramCorr::SetBrightnessCorrectionStrength((float)sp.AutoBrightnessAmount());


    m_pFileList = NULL;
    m_pImageProvider = NULL;
    m_pCurrentImage = NULL;
    m_bOutOfMemoryLastImage = false;
    m_bExceptionErrorLastImage = false;
    m_nLastLoadError = FileLoad_Ok;

    m_nLastSlideShowImageTickCount = 0;
    m_dMovieFPS = 1.0;

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
    m_startMouse = QPoint(-1, -1);
    m_virtualImageSize = QSize(-1, -1);
    m_bInZooming = false;
    m_bTemporaryLowQ = false;
    m_bShowZoomFactor = false;
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

    this->setWindowTitle(APP_TITLE);

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

    m_clientRect = m_bFullScreenMode ? m_monitorRect : CMultiMonitorSupport::GetDefaultWindowRect();

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
                                sp.Sorting(),
                                sp.IsSortedAscending(),
                                sp.WrapAroundFolder());

    m_pFileList->SetNavigationMode(sp.Navigation());

    // create thread pool for processing requests on multiple CPU cores
    CProcessingThreadPool::This().CreateThreadPoolThreads(CSettingsProvider::This().NumberOfCoresToUse());

    int nReadAheadBuffers = (CSettingsProvider::This().CacheRange() * 2) + 1;    // The +1 is the current image

    // create JPEG provider and request first image - do no processing yet if not in fullscreen mode (as we do not know the size yet)
    m_pImageProvider = new CImageProvider(this, nReadAheadBuffers);

    m_pCurrentImage = m_pImageProvider->RequestImage(m_pFileList,
                                                    CImageProvider::FORWARD,
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

    StartLowQTimer(ZOOM_TIMEOUT);    // Start out in low quality scaling to get something on the screen asap. Feels more responsive with huge images.

    this->update();

    this->setAcceptDrops(true);
    this->setMouseTracking(true); // Aktiviert permanente Mouse-Move-Events

    connect(m_pDirectoryWatcher, &CDirectoryWatcher::DirectoryFileListChanged, this, &CMainDlg::onActiveDirectoryFilelistChanged);
    connect(m_pDirectoryWatcher, &CDirectoryWatcher::DisplayedFileChangedOnDisk, this, &CMainDlg::onDisplayedFileChangedOnDisk);

}

CMainDlg::~CMainDlg() {
    delete m_pFileList;
    delete m_pDirectoryWatcher;
    if (m_pImageProvider != NULL) delete m_pImageProvider;
    delete m_pImageProcParams;
    delete m_pImageProcParamsKept;
    delete m_pKeyMap;
}

void CMainDlg::paintEvent(QPaintEvent* /*event*/)
{
    if (m_bLockPaint) {
        return;
    }

    QPainter painter(this);
    m_dRealizedZoom = 1.0;

    m_clientRect = rect();

    QColor backColor = CSettingsProvider::This().ColorBackground();
    painter.fillRect(m_clientRect, backColor);

    if (m_pCurrentImage != NULL) {
        qint64 startTime = QDateTime::currentMSecsSinceEpoch();

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
            qint64 paintTime = QDateTime::currentMSecsSinceEpoch() - startTime;

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

            double dZoom = m_dZoom > 0 ? m_dZoom : 1.0;
            unsigned int iRealWidth = static_cast<unsigned int>(dZoom * (m_pCurrentImage->OrigWidth()));
            unsigned int iRealHeight = static_cast<unsigned int>(dZoom * (m_pCurrentImage->OrigHeight()));

            QString infoText;
            QString sFullPath = CurrentFileName(false);

            if (!sFullPath.isEmpty()) {
                QFileInfo fileInfo(sFullPath);
                // for images in an archive: size of the file within the archive, not of the archive
                const bool bArchiveEntry = m_pCurrentImage->InArchive() && !m_pCurrentImage->ArchiveFileName().isEmpty();
                QString sFileSize = FormatFileSize(bArchiveEntry ? static_cast<qint64>(m_pCurrentImage->ArchiveFileSize())
                                                                 : fileInfo.size());
                QString visiblePath = CurrentDisplayPath(); // for images in an archive "archive/file"

                if (m_pCurrentImage->NumberOfFrames() > 1) {
                    if (m_bIsAnimationPlaying) {
                        infoText = QString("%1\nFrame %2 of %3 (%4 ms)\nFile Size:   %5\nImage Size:  %6 x %7\nZoomed Size: %8 x %9   (Zoom Factor: %10)\nWindow Size: %11 x %12\n\nCPU-Threads: %13\nCPU-Algorithm: %14\nDownsampling-Filter: %15\n\nLoading Image:  %16 ms\nScaling Image:  %17 ms\nPainting Image:  %18 ms")
                        .arg(visiblePath).arg(m_pCurrentImage->FrameIndex() + 1).arg(m_pCurrentImage->NumberOfFrames())
                            .arg(m_pCurrentImage->FrameTimeMs()).arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                            .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                            .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs()).arg(paintTime);
                    } else {
                        infoText = QString("%1\nFrame %2 of %3\nFile Size:   %4\nImage Size:  %5 x %6\nZoomed Size: %7 x %8   (Zoom Factor: %9)\nWindow Size: %10 x %11\n\nCPU-Threads: %12\nCPU-Algorithm: %13\nDownsampling-Filter: %14\n\nLoading Image:  %15 ms\nScaling Image:  %16 ms\nPainting Image:  %17 ms")
                        .arg(visiblePath).arg(m_pCurrentImage->FrameIndex() + 1).arg(m_pCurrentImage->NumberOfFrames())
                            .arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                            .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                            .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs()).arg(paintTime);
                    }
                } else {
                    infoText = QString("%1\nFile Size:   %2\nImage Size:  %3 x %4\nZoomed Size: %5 x %6   (Zoom Factor: %7)\nWindow Size: %8 x %9\n\nCPU-Threads: %10\nCPU-Algorithm: %11\nDownsampling-Filter: %12\n\nLoading Image:  %13 ms\nScaling Image:  %14 ms\nPainting Image:  %15 ms")
                    .arg(visiblePath).arg(sFileSize).arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                        .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                        .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs()).arg(paintTime);
                }
            } else {
                infoText = QString("\n\nImage Size:  %1 x %2\nZoomed Size: %3 x %4   (Zoom Factor: %5)\nWindow Size: %6 x %7\n\nCPU-Threads: %8\nCPU-Algorithm: %9\nDownsampling-Filter: %10\n\nLoading Image:  %11 ms\nScaling Image:  %12 ms\nPainting Image:  %13 ms")
                .arg(m_pCurrentImage->OrigWidth()).arg(m_pCurrentImage->OrigHeight())
                    .arg(iRealWidth).arg(iRealHeight).arg(m_dZoom).arg(m_clientRect.width()).arg(m_clientRect.height())
                    .arg(iNumThreads).arg(sCPU).arg(sFilter).arg(m_pCurrentImage->GetFileLoadDelayMs()).arg(m_pCurrentImage->GetDIBLoadDelayMs()).arg(paintTime);
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
        painter.drawText(GetZoomTextRect(), Qt::AlignRight | Qt::AlignVCenter, formatOutputZoomFactor(m_dZoom));
        */
        QString zoomText = formatOutputZoomFactor(m_dZoom);

        QRect zoomRect = GetZoomTextRect();

        QFont font = QApplication::font();
        font.setBold(true);
        font.setPointSize(12);
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
        painter.drawText(GetToastTextRect(), Qt::AlignRight | Qt::AlignVCenter, m_strToast);
        */
        QRect toastRect = GetToastTextRect();

        QFont font = QApplication::font();
        font.setBold(true);
        font.setPointSize(14);
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

    DisplayErrors(m_pCurrentImage, m_clientRect, painter);
}

/*
void CMainDlg::PaintToDC(QPainter& painter) {
    QColor backColor = CSettingsProvider::This().ColorBackground();
    if (backColor.rgb() == 0) {
        backColor = QColor(0, 0, 1); // nVidia Bug-Workaround für reines Schwarz
    }

    m_dRealizedZoom = 1.0;

    QRect qClient(m_clientRect.left, m_clientRect.top, m_clientRect.Width(), m_clientRect.Height());

    CBaseImage* pCurrentImage = GetCurrentImage();
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

void CMainDlg::DisplayErrors(CBaseImage* pCurrentImage, const QRect& clientRect, QPainter& painter)
{
    // Einheitliche Schriftart für Fehlermeldungen setzen (z. B. Arial oder Standard-GUI-Font)
    QFont font = QApplication::font();
    //font.setBold(true);
    font.setPointSize(12);
    painter.setFont(font);

    if (m_sStartupFile.isEmpty() && m_pCurrentImage == NULL) {
        QRect rectText(0, clientRect.height() / 2 - Helpers::ScaleWithDPI(40), clientRect.width(), Helpers::ScaleWithDPI(80));
        QString message = QStringLiteral("No image loaded!\nPress ESC to exit...");
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

void CMainDlg::closeEvent(QCloseEvent* event) {
    CleanupAndTerminate();
    event->accept(); // Erlaubt das Schließen des Fensters (entspricht bHandled = FALSE)
}

void CMainDlg::mousePressEvent(QMouseEvent* event) {
    m_nMouseX = event->pos().x();
    m_nMouseY = event->pos().y();

    if (event->button() == Qt::LeftButton) {
        QPoint pos = event->pos();

        this->grabMouse();
        CPoint pointClicked(pos.x(), pos.y());

        bool bCtrl = event->modifiers().testFlag(Qt::ControlModifier);
        bool bShift = event->modifiers().testFlag(Qt::ShiftModifier);

        bool bDraggingRequired = m_virtualImageSize.width() > m_clientRect.width() || m_virtualImageSize.height() > m_clientRect.height();

        if (!m_bZoomModeOnLeftMouse && HandleMouseButtonByKeymap(Mouse_Left)) {
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
        return;
    }
    else if (event->button() == Qt::RightButton) {
        if (HandleMouseButtonByKeymap(Mouse_Right)) {
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton) {
        this->grabMouse();

        if (HandleMouseButtonByKeymap(Mouse_Middle)) {
            return;
        }

        QPoint pos = event->pos();
        StartDragging(pos.x(), pos.y(), false);
    }
    else if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        bool isXButton1 = (event->button() == Qt::BackButton);
        int vkCode = isXButton1 ? Mouse_X1 : Mouse_X2;

        if (HandleMouseButtonByKeymap(vkCode)) {
            return;
        }

        if (isXButton1) {
            GotoImage(POS_Previous);
        } else {
            GotoImage(POS_Next);
        }
        return;
    }

    QMainWindow::mousePressEvent(event);
}

void CMainDlg::mouseReleaseEvent(QMouseEvent* event) {
    m_nMouseX = event->pos().x();
    m_nMouseY = event->pos().y();

    if (event->button() == Qt::LeftButton) {
        if (m_bZoomMode) {
            m_bZoomMode = false;
            update();
        } else if (m_bDragging) {
            EndDragging();
        }
        this->releaseMouse();
        return;
    }
    else if (event->button() == Qt::RightButton) {
        if (HandleMouseButtonByKeymap(Mouse_Right, false)) {
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton) {
        EndDragging();
        this->releaseMouse();
        return;
    }

    QMainWindow::mouseReleaseEvent(event);
}

void CMainDlg::mouseDoubleClickEvent(QMouseEvent* event) {
    m_nMouseX = event->pos().x();
    m_nMouseY = event->pos().y();

    if (event->button() == Qt::LeftButton) {
        if (m_pCurrentImage != NULL) {
            if (HandleMouseButtonByKeymap(Mouse_DblClk)) {
                return;
            }
            double dZoom = -1.0;
            QSize sizeAutoZoom = GetVirtualImageSize(m_pCurrentImage->OrigSize(), m_clientRect.size(), GetAutoZoomMode(), dZoom);
            if (sizeAutoZoom != m_virtualImageSize) {
                ExecuteCommand({ Cmd::Command::SetAutoZoom, static_cast<int>(GetAutoZoomMode()) });
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
        if (m_startMouse.x() == -1 && m_startMouse.y() == -1) {
            m_startMouse.setX(m_nMouseX);
            m_startMouse.setY(m_nMouseY);
        } else {
            if (abs(m_nMouseX - m_startMouse.x()) + abs(m_nMouseY - m_startMouse.y()) > 50) {
                MouseOn();
            }
        }
    }

    // Do dragging when needed, else pass event to panel manager and zoom navigator
    if (m_bZoomMode) {
        int nDX = m_nMouseX - m_nCapturedX;
        double dFactor = 1 + pow(nDX * nDX, 0.8) / 1500;
        if (nDX < 0) dFactor = 1.0/dFactor;
        PerformZoom(m_dStartZoom * dFactor, false, true);
    } else if (m_bDragging) {
        DoDragging();
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
                        double dZoom = m_dZoom > 0 ? m_dZoom : 1.0;
                        unsigned int iRealHeight = static_cast<unsigned int>(dZoom * (m_pCurrentImage->OrigHeight()));

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
                    CSettingsProvider::This().MouseWheelZoomSpeed() * double(nDelta) / 120.0,
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
            CSettingsProvider::This().MouseWheelZoomSpeed() * double(nDelta) / 120.0,
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

    int vkCode = event->key();

    if (!event->isAutoRepeat()) {
        if (vkCode == Qt::Key_Up) {
            m_bPanKeyUp = true;
        } else if (vkCode == Qt::Key_Down) {
            m_bPanKeyDown = true;
        } else if (vkCode == Qt::Key_Left) {
            m_bPanKeyLeft = true;
        } else if (vkCode == Qt::Key_Right) {
            m_bPanKeyRight = true;
        }
    }

    if (!bCtrl && vkCode != Qt::Key_Escape && m_nLastLoadError == FileLoad_NoFilesInDirectory && !m_sStartupFile.isEmpty()) {
        // search in subfolders if initial directory has no images
        m_pFileList->SetNavigationMode(Helpers::NM_LoopSubDirectories);
        GotoImage(POS_Next);
        event->accept();
        return;
    }

    // (Der fest verdrahtete F1-Sonderfall ist entfallen: F1 -> Help steht jetzt in der Standard-
    //  keymap und ist damit wie jede andere Taste umbelegbar.)

    // FIX (Numpad): Qt meldet Nummernblock-Tasten mit Qt::KeypadModifier. Ohne dieses Flag war
    // "Numpad1" nicht von "1" unterscheidbar (gleicher Key-Code Qt::Key_1) - die Belegungen
    // aus keymap.txt haben sich gegenseitig ueberschrieben. CKeyMap faellt selbst auf die
    // Nicht-Keypad-Belegung zurueck, wenn fuer die Numpad-Taste keine eigene existiert.
    const bool bKeypad = event->modifiers().testFlag(Qt::KeypadModifier);

    const std::optional<Cmd::CommandInvocation> inv = m_pKeyMap->GetCommandForKey(vkCode, bAlt, bCtrl, bShift, bKeypad);
    // DoNothing wird wie bisher verbraucht (Taste belegt, aber ohne Funktion) und nicht an Qt weitergereicht.
    if (inv) {
        const Cmd::Command cmd = inv->id;
        if (cmd == Cmd::Command::PanUp || cmd == Cmd::Command::PanDown || cmd == Cmd::Command::PanLeft || cmd == Cmd::Command::PanRight) {
            // Prüfung analog zum Original: Wenn eine andere Taste als die Pfeiltaste selbst das Panning triggert
            if ((cmd == Cmd::Command::PanUp && vkCode != Qt::Key_Up) ||
                (cmd == Cmd::Command::PanDown && vkCode != Qt::Key_Down) ||
                (cmd == Cmd::Command::PanLeft && vkCode != Qt::Key_Left) ||
                (cmd == Cmd::Command::PanRight && vkCode != Qt::Key_Right)) {
                m_bAllowSmoothPanning = false;
            }

            // Qt-Äquivalent für Key-Repeat (ersetzt lParam & 0xc0000000)
            if (m_bAllowSmoothPanning && event->isAutoRepeat()) {
                event->accept();
                return;
            }
        }

        ExecuteCommand(*inv);
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void CMainDlg::keyReleaseEvent(QKeyEvent* event) {
    int vkCode = event->key();

    if (!event->isAutoRepeat()) {
        if (vkCode == Qt::Key_Up) {
            m_bPanKeyUp = false;
        } else if (vkCode == Qt::Key_Down) {
            m_bPanKeyDown = false;
        } else if (vkCode == Qt::Key_Left) {
            m_bPanKeyLeft = false;
        } else if (vkCode == Qt::Key_Right) {
            m_bPanKeyRight = false;
        }
    }
    QMainWindow::keyReleaseEvent(event);
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

                OpenFile(filePath);
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

        qint64 currentTick = QDateTime::currentMSecsSinceEpoch();
        qint64 nRealDisplayTimeMs = currentTick - m_nLastSlideShowImageTickCount;

        if (m_nCurrentTimeout > 250 && timerId == m_slideshowTimerId) {
            if (m_nCurrentTimeout - nRealDisplayTimeMs > 100) {
                killTimer(m_slideshowTimerId);
                m_slideshowTimerId = startTimer(m_nCurrentTimeout - nRealDisplayTimeMs);
                return;
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
            if (m_zoomTextTimerId != 0) killTimer(m_zoomTextTimerId);
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
            // Wenn keine Taste mehr gedrückt wird, stoppen wir den Pan-Timer
            if (!m_bPanKeyUp && !m_bPanKeyDown && !m_bPanKeyLeft && !m_bPanKeyRight) {
                killTimer(m_panTimerId);
                m_panTimerId = 0;
                m_bPanTimerActive = false;
                return;
            }

            int PanXbase = 0;
            int PanYbase = 0;
            int BorderDistanceY = 0;
            int PanX = 0;
            int PanY = 0;
            int iRealHeight = int(m_dZoom * (m_pCurrentImage->OrigHeight()));
            int iRealWidth = int(m_dZoom * (m_pCurrentImage->OrigWidth()));

            if (GetAutoZoomMode() == Helpers::ZM_BookMode && iRealHeight > m_clientRect.height() && iRealWidth <= m_clientRect.width()) {
                if ((m_bPanKeyLeft && !m_bPanKeyRight) || (m_bPanKeyUp && !m_bPanKeyDown)) PanYbase = 10;
                else if ((!m_bPanKeyLeft && m_bPanKeyRight) || (!m_bPanKeyUp && m_bPanKeyDown)) PanYbase = -10;
                else PanYbase = 0;
            } else {
                if (m_bPanKeyUp && !m_bPanKeyDown) PanYbase = 10;
                else if (!m_bPanKeyUp && m_bPanKeyDown) PanYbase = -10;
                else PanYbase = 0;

                if (m_bPanKeyLeft && !m_bPanKeyRight) PanXbase = 10;
                else if (!m_bPanKeyLeft && m_bPanKeyRight) PanXbase = -10;
                else PanXbase = 0;
            }

            if (PanXbase != 0 && PanYbase != 0) { // Diagonale Bewegung
                PanX = static_cast<int>(PanXbase * 0.7);
                PanY = static_cast<int>(PanYbase * 0.7);
            } else {
                PanX = PanXbase;
                PanY = PanYbase;
            }

            if (PerformPan(PanX, PanY, false)) {
                this->update(); // Alternative: this->repaint() = Erzwingt ein sofortiges Neuzeichnen ohne den Event-Loop zu blockieren
            }
        } else {
            killTimer(m_panTimerId);
            m_panTimerId = 0;
            m_bPanTimerActive = false;
            return;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////
// Uebersetzung Befehlsargument -> Programm-Enums
//
// Cmd::*Arg (Commands.h) ist bewusst von Helpers::* entkoppelt. Die Zuordnung steht hier an
// EINER Stelle (statt verteilt als "nCommand * 10 + OFFSET"-Arithmetik).
///////////////////////////////////////////////////////////////////////////////////

// Die Argumente von SetAutoZoom werden direkt in Helpers::EAutoZoomMode umgewandelt. Das ist nur
// zulaessig, solange die Reihenfolge gleich bleibt - die alten IDs (IDM_AUTO_ZOOM_*) setzten
// dieselbe Reihenfolge voraus. Die Asserts schlagen beim Kompilieren an, falls jemand das Enum aendert.
// (ZM_Disabled/None ist implizit 0; sein Name ist hier nicht bekannt, daher nicht geprueft.)
static_assert(static_cast<int>(Helpers::ZM_FillScreenNoZoom) == static_cast<int>(Cmd::AutoZoomArg::FillNoZoom), "Cmd::AutoZoomArg != Helpers::EAutoZoomMode");
static_assert(static_cast<int>(Helpers::ZM_FitToScreen)      == static_cast<int>(Cmd::AutoZoomArg::Fit),        "Cmd::AutoZoomArg != Helpers::EAutoZoomMode");
static_assert(static_cast<int>(Helpers::ZM_FillScreen)       == static_cast<int>(Cmd::AutoZoomArg::Fill),       "Cmd::AutoZoomArg != Helpers::EAutoZoomMode");
static_assert(static_cast<int>(Helpers::ZM_BookMode)         == static_cast<int>(Cmd::AutoZoomArg::BookMode),   "Cmd::AutoZoomArg != Helpers::EAutoZoomMode");

static auto NavigationModeForArg(Cmd::LoopModeArg arg) {
    switch (arg) {
        case Cmd::LoopModeArg::Recursively: return Helpers::NM_LoopSubDirectories;
        case Cmd::LoopModeArg::Siblings:    return Helpers::NM_LoopSameDirectoryLevel;
        case Cmd::LoopModeArg::Folder:
        default:                            return Helpers::NM_LoopDirectory;
    }
}

void CMainDlg::onShowContextMenu(const QPoint &pos) {
    MouseOn();

    QMenu menu(this);

    // Jeder Menueeintrag traegt als QAction-Daten einen Cmd::CommandInvocation (Befehl + Argument).
    // Frueher war das ein nackter int, bei dem Familien wie Slideshow-Zeiten oder Zoomstufen ueber
    // Offsets auf IDs kodiert waren; entsprechend entfiel auch die Rueckrechnung beim Anzeigen
    // des Haken-Zustands (z.B. "navMode * 10 + IDM_LOOP_FOLDER == id").
    //
    // AddCommandAction haengt (wenn gewuenscht) die aktuell belegte Taste per '\t' an den Text,
    // sodass Qt sie rechts im Menue ausgerichtet anzeigt. Eintraege, die den Tab selbst schon fuer
    // einen Wert nutzen ("Waiting time\t5 sec"), bekommen kein Kuerzel.
    const CKeyMap* pKeyMap = m_pKeyMap;
    auto AddCmd = [pKeyMap](QMenu* pMenu, const QString& sText, Cmd::Command cmd, int arg = 0, bool bShowShortcut = true) -> QAction* {
        QString sFullText = sText;
        if (bShowShortcut && pKeyMap != nullptr) {
            const QString sKeys = pKeyMap->GetKeyStringForCommand(cmd, arg);
            if (!sKeys.isEmpty()) {
                sFullText += QLatin1Char('\t') + sKeys;
            }
        }
        QAction* pAction = pMenu->addAction(sFullText);
        pAction->setData(QVariant::fromValue(Cmd::CommandInvocation{ cmd, arg }));
        return pAction;
    };

    // 1. Navigation
    AddCmd(&menu, tr("Next image"), Cmd::Command::NextImage);
    AddCmd(&menu, tr("Previous image"), Cmd::Command::PrevImage);

    menu.addSeparator();

    // 2. Current View (Untermenü)
    QMenu* menuCurrentView = menu.addMenu(tr("Current View"));
    AddCmd(menuCurrentView, tr("Rotate +90"), Cmd::Command::Rotate90);
    AddCmd(menuCurrentView, tr("Rotate -90"), Cmd::Command::Rotate270);
    menuCurrentView->addSeparator();
    AddCmd(menuCurrentView, tr("Mirror horizontally"), Cmd::Command::MirrorH);
    AddCmd(menuCurrentView, tr("Mirror vertically"), Cmd::Command::MirrorV);
    menuCurrentView->addSeparator();
    AddCmd(menuCurrentView, tr("Fit to window"), Cmd::Command::FitToScreen);
    AddCmd(menuCurrentView, tr("Fill with crop"), Cmd::Command::FillWithCrop);
    menuCurrentView->addSeparator();

    AddCmd(menuCurrentView, QStringLiteral("400 %"), Cmd::Command::ZoomTo, 400);
    AddCmd(menuCurrentView, QStringLiteral("200 %"), Cmd::Command::ZoomTo, 200);
    AddCmd(menuCurrentView, QStringLiteral("100 %"), Cmd::Command::ZoomTo, 100);
    AddCmd(menuCurrentView, QStringLiteral("50 %"), Cmd::Command::ZoomTo, 50);
    AddCmd(menuCurrentView, QStringLiteral("25 %"), Cmd::Command::ZoomTo, 25);
    menuCurrentView->addSeparator();

    QAction* actFullscreen = AddCmd(menuCurrentView, tr("Full screen mode"), Cmd::Command::FullScreenMode);
    actFullscreen->setCheckable(true);
    actFullscreen->setChecked(m_bFullScreenMode);

    if (m_pCurrentImage == nullptr) {
        menuCurrentView->setEnabled(false);
    }

    menu.addSeparator();

    // 3. Play folder as slideshow/movie (Untermenü)
    QMenu* menuMovie = menu.addMenu(tr("Play folder as slideshow/animation"));

    // (Der Text enthielt frueher "\t0xff" - ein Relikt aus der Win32-Zeit. Die Anzeige uebernimmt
    //  jetzt AddCmd; fuer StopMovie ist keine Taste belegt, Esc laeuft ueber DefaultEscape.)
    QAction* actStopMovie = AddCmd(menuMovie, tr("Pause slideshow/animation"), Cmd::Command::StopMovie);
    actStopMovie->setEnabled(m_bMovieMode);

    menuMovie->addSeparator();

    // Ueberschrift ohne Funktion (frueher IDM_SLIDESHOW_START als Pseudo-ID): kein Befehl, keine Daten.
    QAction* actSlideStart = menuMovie->addAction(tr("Slideshow"));
    actSlideStart->setEnabled(false);

    // Wartezeiten für Slideshow (Sekunden pro Bild)
    const int slideTimes[] = {1, 2, 3, 4, 5, 7, 10, 20};
    for (int nSeconds : slideTimes) {
        AddCmd(menuMovie, QString(tr("Waiting time\t%1 sec")).arg(nSeconds), Cmd::Command::StartSlideshow, nSeconds, false);
    }

    // Transition Effect (Untermenü)
    QMenu* menuEffect = menuMovie->addMenu(tr("Transition Effect"));
    QActionGroup* groupEffect = new QActionGroup(menuEffect);
    groupEffect->setExclusive(true);

    auto addEffect = [&](const QString& text, Cmd::TransitionEffectArg effect) {
        QAction* act = AddCmd(menuEffect, text, Cmd::Command::SetTransitionEffect, static_cast<int>(effect), false);
        act->setCheckable(true);
        groupEffect->addAction(act);
        // m_eTransitionEffect hat dieselbe Reihenfolge wie Cmd::TransitionEffectArg (siehe ExecuteCommand)
        if (static_cast<int>(m_eTransitionEffect) == static_cast<int>(effect)) act->setChecked(true);
    };
    addEffect(tr("None"), Cmd::TransitionEffectArg::None);
    menuEffect->addSeparator();
    addEffect(tr("Blend"), Cmd::TransitionEffectArg::Blend);
    menuEffect->addSeparator();
    addEffect(tr("Slide from right"), Cmd::TransitionEffectArg::SlideFromRight);
    addEffect(tr("Slide from left"), Cmd::TransitionEffectArg::SlideFromLeft);
    addEffect(tr("Slide from top"), Cmd::TransitionEffectArg::SlideFromTop);
    addEffect(tr("Slide from bottom"), Cmd::TransitionEffectArg::SlideFromBottom);
    menuEffect->addSeparator();
    addEffect(tr("Roll from right"), Cmd::TransitionEffectArg::RollFromRight);
    addEffect(tr("Roll from left"), Cmd::TransitionEffectArg::RollFromLeft);
    addEffect(tr("Roll from top"), Cmd::TransitionEffectArg::RollFromTop);
    addEffect(tr("Roll from bottom"), Cmd::TransitionEffectArg::RollFromBottom);
    menuEffect->addSeparator();
    addEffect(tr("Scroll from right"), Cmd::TransitionEffectArg::ScrollFromRight);
    addEffect(tr("Scroll from left"), Cmd::TransitionEffectArg::ScrollFromLeft);
    addEffect(tr("Scroll from top"), Cmd::TransitionEffectArg::ScrollFromTop);
    addEffect(tr("Scroll from bottom"), Cmd::TransitionEffectArg::ScrollFromBottom);

    // Transition Speed (Untermenü)
    QMenu* menuSpeed = menuMovie->addMenu(tr("Transition Speed"));
    QActionGroup* groupSpeed = new QActionGroup(menuSpeed);
    groupSpeed->setExclusive(true);

    const Cmd::TransitionSpeedArg speedArgs[] = {
        Cmd::TransitionSpeedArg::VeryFast, Cmd::TransitionSpeedArg::Fast, Cmd::TransitionSpeedArg::Normal,
        Cmd::TransitionSpeedArg::Slow, Cmd::TransitionSpeedArg::VerySlow };
    const QString speedNames[] = {tr("Very fast"), tr("Fast"), tr("Normal"), tr("Slow"), tr("Very slow")};
    // Gegenstueck zu "125 * (1 << arg)" in ExecuteCommand: aus der Zeit wieder den Index bestimmen
    const int nIndex = (m_nTransitionTime < 180) ? 0 : (m_nTransitionTime < 375) ? 1 : (m_nTransitionTime < 750) ? 2 : (m_nTransitionTime < 1500) ? 3 : 4;
    for (int i = 0; i < 5; ++i) {
        QAction* act = AddCmd(menuSpeed, speedNames[i], Cmd::Command::SetTransitionSpeed, static_cast<int>(speedArgs[i]), false);
        act->setCheckable(true);
        groupSpeed->addAction(act);
        if (nIndex == static_cast<int>(speedArgs[i])) act->setChecked(true);
    }

    menuMovie->addSeparator();

    // Ueberschrift ohne Funktion (frueher IDM_MOVIE_START_FPS als Pseudo-ID)
    QAction* actMovieStartFps = menuMovie->addAction(tr("Movie"));
    actMovieStartFps->setEnabled(false);

    const int fpsValues[] = {5, 10, 15, 20, 25, 30, 50, 60, 100};
    for (int nFps : fpsValues) {
        AddCmd(menuMovie, QString(tr("Playback speed\t%1 fps")).arg(nFps), Cmd::Command::PlayMovie, nFps, false);
    }

    menu.addSeparator();

    // 4. Haupt-Dateiaktionen
    QAction* actReload = AddCmd(&menu, tr("Reload image"), Cmd::Command::ReloadFull);
    actReload->setEnabled(m_pCurrentImage != nullptr && !m_bMovieMode);

    AddCmd(&menu, tr("Edit image in default editor"), Cmd::Command::OpenDefaultEditor);

    QAction* actCopyFull = AddCmd(&menu, tr("Copy original image content to clipboard"), Cmd::Command::CopyFull);
    actCopyFull->setEnabled(m_pCurrentImage != nullptr);

    QAction* actCopyPath = AddCmd(&menu, tr("Copy file path to clipboard"), Cmd::Command::CopyPath);
    actCopyPath->setEnabled(m_pCurrentImage != nullptr);

    AddCmd(&menu, tr("Cut file to clipboard"), Cmd::Command::CutFile);
    AddCmd(&menu, tr("Show in folder"), Cmd::Command::ExploreFile);

    menu.addSeparator();

    // 5. Folder Mode (Untermenü)
    QMenu* menuFolderMode = menu.addMenu(tr("Folder Mode"));
    QActionGroup* groupFolder = new QActionGroup(menuFolderMode);
    groupFolder->setExclusive(true);

    const Cmd::LoopModeArg folderArgs[] = {Cmd::LoopModeArg::Folder, Cmd::LoopModeArg::Recursively, Cmd::LoopModeArg::Siblings};
    const QString folderNames[] = {tr("Loop folder"), tr("Loop recursively"), tr("Loop siblings")};
    // Vergleich ueber die echten Helpers::NM_*-Werte (statt "navMode * 10 + ID-Offset")
    const int navMode = m_pFileList ? static_cast<int>(m_pFileList->GetNavigationMode()) : static_cast<int>(Helpers::NM_LoopDirectory);
    for (int i = 0; i < 3; ++i) {
        QAction* act = AddCmd(menuFolderMode, folderNames[i], Cmd::Command::SetLoopMode, static_cast<int>(folderArgs[i]), false);
        act->setCheckable(true);
        groupFolder->addAction(act);
        if (navMode == static_cast<int>(NavigationModeForArg(folderArgs[i]))) {
            act->setChecked(true);
        }
    }

    // 6. Sort Order (Untermenü)
    QMenu* menuSort = menu.addMenu(tr("Sort Order"));
    QActionGroup* groupSort = new QActionGroup(menuSort);
    groupSort->setExclusive(true);

    Helpers::ESorting sorting = m_pFileList ? m_pFileList->GetSorting() : Helpers::FS_FileName;
    bool isRandom = (sorting == Helpers::FS_Random);

    auto addSortAction = [&](const QString& text, Cmd::SortModeArg mode, Helpers::ESorting fs) {
        QAction* act = AddCmd(menuSort, text, Cmd::Command::SetSortMode, static_cast<int>(mode), false);
        act->setCheckable(true);
        groupSort->addAction(act);
        if (sorting == fs) act->setChecked(true);
    };

    addSortAction(tr("File name"), Cmd::SortModeArg::Name, Helpers::FS_FileName);
    addSortAction(tr("Modification date"), Cmd::SortModeArg::ModificationDate, Helpers::FS_LastModTime);
    addSortAction(tr("File size"), Cmd::SortModeArg::Size, Helpers::FS_FileSize);
    addSortAction(tr("Random"), Cmd::SortModeArg::Random, Helpers::FS_Random);

    menuSort->addSeparator();

    bool isAscending = m_pFileList ? m_pFileList->IsSortedAscending() : true;
    QAction* actAsc = AddCmd(menuSort, tr("Ascending"), Cmd::Command::SetSortOrder, static_cast<int>(Cmd::SortOrderArg::Ascending), false);
    actAsc->setCheckable(true);
    actAsc->setChecked(isAscending);
    actAsc->setEnabled(!isRandom);

    QAction* actDesc = AddCmd(menuSort, tr("Descending"), Cmd::Command::SetSortOrder, static_cast<int>(Cmd::SortOrderArg::Descending), false);
    actDesc->setCheckable(true);
    actDesc->setChecked(!isAscending);
    actDesc->setEnabled(!isRandom);

    // 7. Auto Zoom Mode (Untermenü)
    QMenu* menuAutoZoom = menu.addMenu(tr("Auto Zoom Mode"));
    QActionGroup* groupAutoZoom = new QActionGroup(menuAutoZoom);
    groupAutoZoom->setExclusive(true);

    const Cmd::AutoZoomArg autoZoomArgs[] = {
        Cmd::AutoZoomArg::None, Cmd::AutoZoomArg::Fit, Cmd::AutoZoomArg::Fill,
        Cmd::AutoZoomArg::FitNoZoom, Cmd::AutoZoomArg::FillNoZoom, Cmd::AutoZoomArg::BookMode };
    const QString autoZoomNames[] = {tr("None"), tr("Fit to window"), tr("Fill with crop"), tr("Fit to window (never upsize)"), tr("Fill with crop (never upsize)"), tr("Book Mode")};
    const int autoZoomMode = static_cast<int>(GetAutoZoomMode());
    for (int i = 0; i < 6; ++i) {
        QAction* act = AddCmd(menuAutoZoom, autoZoomNames[i], Cmd::Command::SetAutoZoom, static_cast<int>(autoZoomArgs[i]), false);
        act->setCheckable(true);
        groupAutoZoom->addAction(act);
        if (autoZoomMode == static_cast<int>(autoZoomArgs[i])) {
            act->setChecked(true);
        }
    }

    // Auto Contrast
    QAction* actAutoContrast = AddCmd(&menu, tr("Auto Contrast"), Cmd::Command::ToggleAutoCorrection);
    actAutoContrast->setCheckable(true);
    actAutoContrast->setChecked(m_bAutoContrast);

    menu.addSeparator();

    // 8. Einstellungen & Beenden
    AddCmd(&menu, tr("Settings"), Cmd::Command::SettingsDialog);

    menu.addSeparator();

    AddCmd(&menu, tr("Exit"), Cmd::Command::Exit);

    // 9. Menü an Mausposition ausführen und auswerten
    QAction* pSelectedAction = menu.exec(pos);
    if (pSelectedAction != nullptr) {
        // Eintraege ohne Befehl (z.B. die Ueberschriften) haben keine Daten und werden ignoriert.
        const QVariant data = pSelectedAction->data();
        if (data.canConvert<Cmd::CommandInvocation>()) {
            ExecuteCommand(data.value<Cmd::CommandInvocation>());
        }
    }
}

void CMainDlg::ExecuteCommand(const Cmd::CommandInvocation& inv) {
    using Cmd::Command;

    CSettingsProvider& sp = CSettingsProvider::This();
    switch (inv.id) {
        case Command::Minimize:
            this->showMinimized();
            break;
        case Command::ExploreFile:
            ExploreFile();
            break;
        case Command::SettingsDialog:
            sp.ShowSettingsDialog(this);
            break;
        case Command::Reload:
            ReloadImage(false);
            break;
        case Command::ReloadFull:
            if (m_pFileList != NULL && m_pFileList->CurrentFileExists())
                {
                m_pFileList->Reload(NULL);
                this->update();
                }

            ReloadImage(false);
            break;
        case Command::CopyScreen:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyImageToClipboard(m_pCurrentImage, m_pFileList->Current());
            }
            break;
        case Command::CopyFull:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyFullImageToClipboard(m_pCurrentImage, *m_pImageProcParams, CreateDefaultProcessingFlags(), m_pFileList->Current());
                this->update();
            }
            break;
        case Command::CopyPath:
            if (m_pCurrentImage != NULL) {
                CClipboard::CopyPathToClipboard(m_pCurrentImage, m_pFileList->Current());
            }
            break;
        case Command::CutFile:
            if (m_pCurrentImage != NULL) {
                CClipboard::CutFileToClipboard(m_pFileList->Current());
            }
            break;
        case Command::DeleteFile:
            MouseOn();
            if (m_pCurrentImage != NULL && m_pFileList != NULL) {
                QString currentFileName = CurrentFileName(false);
                if (currentFileName.isEmpty()) {
                    break;
                }

                Helpers::EDeleteConfirmation confirmationPolicy = CSettingsProvider::This().DeleteConfirmation();
                if (confirmationPolicy != Helpers::DC_Never) {
                    bool bRecycleOnly = false;
                    if (!showDeleteConfirmationDialog({currentFileName}, bRecycleOnly)) {
                        break;
                    }
                }

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
            break;
        case Command::NextFrame:
            GotoImage(POS_NextFrame);
            break;
        case Command::NextFile:
            GotoImage(POS_NextFile);
            break;
        case Command::PrevFile:
            GotoImage(POS_PreviousFile);
            break;
        case Command::NextImage:
            GotoImage(POS_Next);
            break;
        case Command::PrevImage:
            GotoImage(POS_Previous);
            break;
        case Command::FirstImage:
            SetToast(QStringLiteral("Jump: First image"));
            GotoImage(POS_First);
            break;
        case Command::LastImage:
            SetToast(QStringLiteral("Jump: Last image"));
            GotoImage(POS_Last);
            break;
        case Command::PlayPause:
            if (m_bIsAnimationPlaying) {
                SetToast(QStringLiteral("Animation Paused"));
                StopAnimation(); // stop any running animation
            } else if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
                SetToast(QStringLiteral("Animation Continued"));
                StartAnimation();
            }
            break;
        case Command::PlayPauseNext:
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
        case Command::SetLoopMode:
            m_pFileList->SetNavigationMode(
                NavigationModeForArg(static_cast<Cmd::LoopModeArg>(inv.arg)));
            break;
        case Command::SetSortMode:
            {
                Helpers::ESorting nSortMode = m_pFileList->GetSorting();
                bool bSortedAscending = m_pFileList->IsSortedAscending();
                
                if (inv.arg != static_cast<int>(Cmd::SortModeArg::Random)) {
                    m_eSortModeBeforeRandom = nSortMode;
                }

                nSortMode = (inv.arg == static_cast<int>(Cmd::SortModeArg::ModificationDate)) ? Helpers::FS_LastModTime :
                            (inv.arg == static_cast<int>(Cmd::SortModeArg::Random)) ? Helpers::FS_Random :
                            (inv.arg == static_cast<int>(Cmd::SortModeArg::Size)) ? Helpers::FS_FileSize : Helpers::FS_FileName;
                
                ToastSortingMode(nSortMode, bSortedAscending);
                m_pFileList->SetSorting(nSortMode, bSortedAscending);
            }
            break;
        case Command::SetSortOrder:
            m_pFileList->SetSorting(m_pFileList->GetSorting(), inv.arg == static_cast<int>(Cmd::SortOrderArg::Ascending));
            break;
        case Command::StopMovie:
            SetToast(QStringLiteral("Slideshow Paused"));
            StopMovieMode();
            break;
        case Command::ResumeSlideshow:
            {
            const int timerSec = static_cast<int>(1.0 / m_dMovieFPS);
            SetToast(QStringLiteral("Slideshow Resumed (Timer %1 s)").arg(timerSec));
            StartMovieMode(m_dMovieFPS);
            }
            break;
        case Command::StartSlideshow:
            {
            const int nValue = inv.arg;   // Sekunden pro Bild
            SetToast(QStringLiteral("Starting Slideshow (Timer %1 s)").arg(nValue));
            StartMovieMode(1.0/nValue);
            }
            break;
        case Command::SetTransitionEffect:
            // Die Reihenfolge von Cmd::TransitionEffectArg entspricht der von Helpers::ETransitionEffect
            // (wie bisher die Reihenfolge der IDM_EFFECT_*-IDs).
            m_eTransitionEffect = static_cast<Helpers::ETransitionEffect>(inv.arg);
            break;
        case Command::SetTransitionSpeed:
            m_nTransitionTime = 125 * (1 << inv.arg);   // VeryFast=125 ms ... VerySlow=2000 ms
            break;
        case Command::PlayMovie:
            {
                QString toastText;
                const int nValue = inv.arg;   // Bilder pro Sekunde

                if (m_pCurrentImage != nullptr && m_pCurrentImage->IsAnimation()) {
                    const int targetFrameTime = 1000 / nValue;

                    if (m_customAnimationFrameTime == targetFrameTime) {
                        m_customAnimationFrameTime = 1000;
                        if (m_pCurrentImage->FrameTimeMs() != 0) {
                            const int defaultFps = 1000 / m_pCurrentImage->FrameTimeMs();
                            toastText = QStringLiteral("Default Playback Speed: %1 fps").arg(defaultFps);
                        } else {
                            toastText = QStringLiteral("Default Playback Speed");
                        }
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
        case Command::Rotate90:
        case Command::Rotate270:
            if (m_pCurrentImage != NULL) {
                uint32 nRotationDelta = (inv.id == Command::Rotate90) ? 90 : 270;
                m_nRotation = (m_nRotation + nRotationDelta) % 360;
                m_nUserRotation = (m_nUserRotation + nRotationDelta) % 360;
                m_pCurrentImage->Rotate(nRotationDelta);
                m_dZoom = -1;
                m_bUserZoom = false;
                m_bUserPan = false;
                this->update();
            }
            break;
        case Command::MirrorH:
        case Command::MirrorV:
            if (m_pCurrentImage != NULL) {
                m_pCurrentImage->Mirror(inv.id == Command::MirrorH);
                this->update();
            }
            break;
        case Command::ToggleAutoCorrection:
            m_bAutoContrast = !m_bAutoContrast;
            SetToast(m_bAutoContrast ? QStringLiteral("Auto Contrast: ON") : QStringLiteral("Auto Contrast: OFF"));
            this->update();
            break;
        case Command::KeepParameters:
            m_bKeepParams = !m_bKeepParams;
            if (m_bKeepParams) {
                *m_pImageProcParamsKept = *m_pImageProcParams;
                m_eProcessingFlagsKept = CreateProcessingFlags(m_bHQResampling, m_bAutoContrast, m_bKeepParams);
                m_dZoomKept = m_bUserZoom ? m_dZoom : -1;
                m_offsetKept = m_bUserPan ? m_offsets : QPoint(0, 0);
            }
            break;
        case Command::FitToScreen:
        case Command::FitToScreenNoEnlarge:
            ResetZoomToFitScreen(false, inv.id == Command::FitToScreen, true);
            break;
        case Command::FillWithCrop:
            ResetZoomToFitScreen(true, true, true);
            break;
        case Command::ToggleAutoZoomFit:
            {
            // This command is meant to be used with keyboard shortcuts.
            // It toggles the AutoZoomMode between FitToScreen and None,
            // or, in case of BookMode, between FitToScreen and BookMode.
            // This is meant as an alternative to Command::ToggleFitTo100,
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
        case Command::ToggleFitTo100:
        case Command::ToggleFillWithCropTo100:
            m_bShowZoomFactor = true;
            if (fabs(m_dZoom - 1) < 0.01) {
                ResetZoomToFitScreen(inv.id == Command::ToggleFillWithCropTo100, true, true);
            } else {
                ResetZoomTo100Percents(m_bMouseOn);
            }
            break;
        case Command::FullScreenMode: {
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
                UpdateWindowTitle(true); // Aktualisiert den Titel / das Icon der Titelleiste
                this->MouseOn();
            }

            m_dZoom = -1;
            StartLowQTimer(ZOOM_TIMEOUT);
            break;
        }
        case Command::ZoomTo:
            // Argument = Prozent. 100 % ist "echte Groesse" und hat eine eigene Funktion
            // (ResetZoomTo100Percents zoomt im Fenstermodus immer auf die Mausposition).
            if (inv.arg == 100) {
                ResetZoomTo100Percents(m_bMouseOn);
            } else {
                PerformZoom(inv.arg / 100.0, false, m_bMouseOn);
            }
            break;
        case Command::ZoomIn:
        case Command::ZoomOut:
            PerformZoom((inv.id == Command::ZoomIn) ? 1 : -1, true, m_bMouseOn);
            break;
        case Command::ZoomMode:
            m_bZoomModeOnLeftMouse = !m_bZoomModeOnLeftMouse;
            break;
        case Command::SetAutoZoom:
            {
                // Die Reihenfolge von Cmd::AutoZoomArg entspricht Helpers::EAutoZoomMode (siehe static_assert oben).
                Helpers::EAutoZoomMode eAutoZoomMode = static_cast<Helpers::EAutoZoomMode>(inv.arg);
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
        case Command::Exit:
            CleanupAndTerminate();
            break;
        case Command::DefaultEscape:
            if (m_bMovieMode) {
                SetToast(QStringLiteral("Slideshow Paused"));
                StopMovieMode(); // stop any running movie/slideshow
            } else if (m_bIsAnimationPlaying) {
                SetToast(QStringLiteral("Animation Paused"));
                StopAnimation(); // stop any running animation
            } else if (m_bShowInfo == true) {
                m_bShowInfo = false;
                this->update();
            } else if (m_bFullScreenMode) {
                ExecuteCommand(Cmd::CommandInvocation{ Command::FullScreenMode, 0 });
            } else {
                CleanupAndTerminate();
            }
            break;
        case Command::ContrastCorrectionInc:
        case Command::ContrastCorrectionDec:
            if (m_bAutoContrast) {
                double dInc = (inv.id == Command::ContrastCorrectionInc) ? 0.05 : -0.05;
                m_pImageProcParams->ContrastCorrectionFactor = std::max(0.0, std::min(1.0, m_pImageProcParams->ContrastCorrectionFactor + dInc));
                this->update();
            }
            break;
        case Command::ToggleTransparency:
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
            m_pImageProvider->NotifyNotUsed(m_pCurrentImage);
            m_pImageProvider->ClearAllRequests();
            m_pCurrentImage = NULL;
            ReloadImage(true);
            break;
        case Command::ToggleEmbeddedColorProfiles:
            m_bUseEmbeddedColorProfiles = !m_bUseEmbeddedColorProfiles;
            CSettingsProvider::This().SetUseEmbeddedColorProfiles(m_bUseEmbeddedColorProfiles);
            if (m_bUseEmbeddedColorProfiles == false) {
                SetToast(QStringLiteral("Embedded Color Profiles: ignored"));
            } else {
                SetToast(QStringLiteral("Embedded Color Profiles: used"));
            }
            m_pImageProvider->NotifyNotUsed(m_pCurrentImage);
            m_pImageProvider->ClearAllRequests();
            m_pCurrentImage = NULL;
            ReloadImage(true);
            break;
        case Command::ToggleResamplingQuality:
            m_bHQResampling = !m_bHQResampling;
            SetToast(m_bHQResampling? QStringLiteral("Resampling: enabled"): QStringLiteral("Resampling: disabled"));
            this->update();
            break;
        case Command::PanUp:
        case Command::PanDown:
        case Command::PanRight:
        case Command::PanLeft:
            if (sp.SmartPanningKeys() == false) {
                if (m_pCurrentImage != NULL) {
                    int iRealHeight = (int) (m_dZoom * (m_pCurrentImage->OrigHeight()));
                    int iRealWidth = (int) (m_dZoom * (m_pCurrentImage->OrigWidth()));

                    if (((iRealHeight > m_clientRect.height()) && (inv.id == Command::PanUp || inv.id == Command::PanDown))
                     || (( iRealWidth >  m_clientRect.width()) && (inv.id == Command::PanLeft || inv.id == Command::PanRight))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                if (m_panTimerId != 0) killTimer(m_panTimerId);
                                m_panTimerId = startTimer(17);
                            }
                        } else {
                            PerformPan((inv.id == Command::PanLeft) ? PAN_STEP : (inv.id == Command::PanRight) ? -PAN_STEP : 0,
                                (inv.id == Command::PanUp) ? PAN_STEP : (inv.id == Command::PanDown) ? -PAN_STEP : 0, false);
                        }                    
                    } else {
                        PerformPan((inv.id == Command::PanLeft) ? PAN_STEP : (inv.id == Command::PanRight) ? -PAN_STEP : 0,
                            (inv.id == Command::PanUp) ? PAN_STEP : (inv.id == Command::PanDown) ? -PAN_STEP : 0, false);
                    }
                } else {
                    PerformPan((inv.id == Command::PanLeft) ? PAN_STEP : (inv.id == Command::PanRight) ? -PAN_STEP : 0,
                        (inv.id == Command::PanUp) ? PAN_STEP : (inv.id == Command::PanDown) ? -PAN_STEP : 0, false);
                }
            } else {
                if (m_pCurrentImage != NULL) {
                    int iRealHeight = (int) (m_dZoom * (m_pCurrentImage->OrigHeight()));
                    int iRealWidth = (int) (m_dZoom * (m_pCurrentImage->OrigWidth()));

                    if (((iRealHeight > m_clientRect.height()) && (inv.id == Command::PanUp || inv.id == Command::PanDown))
                     || (( iRealWidth >  m_clientRect.width()) && (inv.id == Command::PanLeft || inv.id == Command::PanRight))) {
                        if (m_bAllowSmoothPanning) {
                            if (m_bPanTimerActive == false) {
                                m_bPanTimerActive = true;
                                if (m_panTimerId != 0) killTimer(m_panTimerId);
                                m_panTimerId = startTimer(17);
                            }
                        } else {
                            PerformPan((inv.id == Command::PanLeft) ? PAN_STEP : (inv.id == Command::PanRight) ? -PAN_STEP : 0,
                                (inv.id == Command::PanUp) ? PAN_STEP : (inv.id == Command::PanDown) ? -PAN_STEP : 0, false);
                        }                    
                    } else {
                        if (GetAutoZoomMode() != Helpers::ZM_BookMode) {
                            if (inv.id == Command::PanRight)
                                GotoImage(POS_Next);
                            else if (inv.id == Command::PanLeft)
                                GotoImage(POS_Previous);
                        } else {
                            if (iRealHeight > m_clientRect.height()) {
                                if (m_bPanTimerActive == false) {
                                    if ((m_offsets.y() >= ((iRealHeight - m_clientRect.height()) / 2)) && (inv.id == Command::PanLeft)) {    // Upper Border!
                                        GotoImage(POS_Previous);
                                    } else if ((-m_offsets.y() >= ((iRealHeight - m_clientRect.height()) / 2)) && (inv.id == Command::PanRight)) {    //Lower Border!
                                        GotoImage(POS_Next);
                                    } else {
                                        if (m_bAllowSmoothPanning) {
                                            m_bPanTimerActive = true;
                                            if (m_panTimerId != 0) killTimer(m_panTimerId);
                                            m_panTimerId = startTimer(17);
                                        } else {
                                            PerformPan(0, ((inv.id == Command::PanUp) || (inv.id == Command::PanLeft)) ? PAN_STEP : ((inv.id == Command::PanDown) || (inv.id == Command::PanRight)) ? -PAN_STEP : 0, false);
                                        }
                                    }
                                }
                            } else {
                                if (m_bPanTimerActive == false) {
                                    if (inv.id == Command::PanRight)
                                        GotoImage(POS_Next);
                                    else if (inv.id == Command::PanLeft)
                                        GotoImage(POS_Previous);
                                }
                            }
                        }
                    }
                } else {
                    // No image. No dimensions. Panning impossible. GotoImage
                    if (inv.id == Command::PanRight)
                        GotoImage(POS_Next);
                    else if (inv.id == Command::PanLeft)
                        GotoImage(POS_Previous);
                }
            }
            break;
        case Command::ContextMenu:
            {
                QPoint globalMousePos = QCursor::pos();

                // 2. Das lokale Rechteck deines Fensters (ClientArea) in globale Koordinaten umrechnen
                QRect globalClientRectangle = QRect(this->mapToGlobal(QPoint(0, 0)), this->size());

                // 3. Prüfen, ob sich der Mauszeiger innerhalb dieses globalen Rechtecks befindet
                bool isOverMyWindow = globalClientRectangle.contains(globalMousePos);

                if (!isOverMyWindow) {
                    globalMousePos = this->mapToGlobal(QPoint(0, 0));
                }

                onShowContextMenu(globalMousePos);
            }
            break;
        case Command::ToggleSortRandom:
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
        case Command::ToggleInfoOverlay:
            m_bShowInfo = !m_bShowInfo;
            this->update();
            break;
        case Command::ChangeFolderLanguage:
            ChangeFolderLanguage();
            break;
        case Command::OpenDefaultEditor:
            OpenDefaultEditor();
            break;
        case Command::RenameFile:
            {
                const QString currentPath = CurrentFileName(false);
                if (currentPath.isEmpty()) {
                    break;   // nothing displayed, nothing to rename
                }

                // Watcher pausieren, bis der Block endet: Die eigene Umbenennung soll kein
                // "Ordner geaendert" bzw. "Datei geaendert" ausloesen (unnoetiges Neueinlesen).
                CDirectoryWatcher::ScopedPause pause(*m_pDirectoryWatcher);
                RenameDialog dlg(currentPath, this);
                if (dlg.exec() == QDialog::Accepted) {
                    // No OpenFile(): the image on screen stays valid. The file list, the image cache,
                    // bookmarks, window title and directory watcher are told the new name instead.
                    OnCurrentFileRenamed(currentPath, dlg.newFilePath());
                }
            }
            break;
        case Command::Help:
            // (frueher fest verdrahtet in keyPressEvent: if (vkCode == Qt::Key_F1) ExecuteCommand(IDM_HELP);
            //  jetzt regulaerer Eintrag der Standard-keymap, dadurch umbelegbar.)
            // Der alte switch hatte fuer IDM_HELP keinen case - die Hilfe ist noch nicht umgesetzt.
            break;
        case Command::DoNothing:
        case Command::Count:
            // DoNothing: Taste ist bewusst "belegt, aber ohne Funktion". Count ist kein echter Befehl.
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
                OpenFile(candidatePath);
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

    // Vollbildmodus verlassen (falls aktiv)
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

    QDesktopServices::openUrl(QUrl::fromLocalFile(sCurrentFileName));
}

void CMainDlg::ExploreFile() {
    const QString sCurrentFileName = CurrentFileName(false);
    if (sCurrentFileName.isEmpty()) {
        return;
    }

    QFileInfo fileInfo(sCurrentFileName);

    QString targetPath = sCurrentFileName;
    QString targetFile = "";
    if (fileInfo.isFile()) {
        targetPath = fileInfo.absoluteDir().path();
        targetFile = sCurrentFileName;
    }

    QDir appDir(QCoreApplication::applicationDirPath());
    QString mkFolderWidgetPath = appDir.filePath("mkFolderWidget");
    QString fileManager;

    if (QFile::exists(mkFolderWidgetPath) && (fileManager.isEmpty() || fileManager == "mkFolderWidget")) {
        QStringList args;
        args << QDir::toNativeSeparators(targetPath);
        if (!targetFile.isEmpty()) {
            args << "-f" << QDir::toNativeSeparators(targetFile);
        }
        QProcess::startDetached(mkFolderWidgetPath, args);
        return;
    }

    QProcess::startDetached("dbus-send", {
                                             "--session",
                                             "--print-reply",
                                             "--dest=org.freedesktop.FileManager1",
                                             "/org/freedesktop/FileManager1",
                                             "org.freedesktop.FileManager1.ShowItems",
                                             "array:string:" + QUrl::fromLocalFile(sCurrentFileName).toString(),
                                             "string:\"\""
                                         });
}

void CMainDlg::OpenFile(const QString& fileName) {
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
    m_pImageProvider->NotifyNotUsed(m_pCurrentImage);
    m_pImageProvider->ClearAllRequests();

    m_pCurrentImage = m_pImageProvider->RequestImage(m_pFileList,
                                                    CImageProvider::FORWARD,
                                                    m_pFileList->Current(),
                                                    nFrameIndex,
                                                    CreateProcessParams(false),
                                                    m_bOutOfMemoryLastImage,
                                                    m_bExceptionErrorLastImage);

    m_nLastLoadError = GetLoadErrorAfterOpenFile();

    AfterNewImageLoaded(true, false);
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        StartAnimation();
    }
    m_startMouse = QPoint(-1, -1);
    MouseOff();
    this->update();
}

void CMainDlg::StartDragging(int nX, int nY, bool bDragWithZoomNavigator) {
    m_startMouse = QPoint(-1, -1);
    m_bDragging = true;
    m_nCapturedX = nX;
    m_nCapturedY = nY;
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
    CImageProvider::EReadAheadDirection eDirection = CImageProvider::FORWARD;

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
                eDirection = CImageProvider::BACKWARD;

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
                eDirection = CImageProvider::BACKWARD;
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
/* Debugging */ qDebug() << "GotoImage(" << "ePos=" << ePos << ", nFlags=" << nFlags << ") Pos4b  bCheckIfSameImage:" << bCheckIfSameImage << "bNoWrapAroundEdgeFrame:" << bNoWrapAroundEdgeFrame << "m_pFileList->ChangedSinceCheckpoint():" << m_pFileList->ChangedSinceCheckpoint();
        return; // not placed on a new image, don't do anything

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

    m_pImageProvider->NotifyNotUsed(m_pCurrentImage);
    // Clear from cache only for POS_Current POS_AwayFromCurrent, which are involved in reloading on file/directory changes and deletion.
    if (ePos == POS_Current || ePos == POS_AwayFromCurrent) {
        // When (ePos == POS_AwayFromCurrent), it will additionally releaseLockedFile in the load thread
        m_pImageProvider->ClearRequest(m_pCurrentImage, ePos == POS_AwayFromCurrent);
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

    m_pCurrentImage = m_pImageProvider->RequestImage(m_pFileList,
                                                    (ePos == POS_AwayFromCurrent) ? CImageProvider::NONE : eDirection,
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
        this->repaint(); // this will force to wait until really redrawn, preventing images to be processed but not shown
    }

    if (!(nFlags & NO_REMOVE_KEY_MSG)) {
        DiscardPendingInput();
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

                CSettingsProvider::This().SetBookModePageHeight(m_nBookModePageHeight); // Will be written to disk during SaveCustomSettings()
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
    
    qDebug() << "CMainDlg::CreateProcessParams() nClientWidth:" << nClientWidth << "nClientHeight:" << nClientHeight;

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
    if (m_slideshowTimerId != 0) killTimer(m_slideshowTimerId);
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
        ExecuteCommand({ Cmd::Command::KeepParameters, 0 });
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
                ExecuteCommand({ Cmd::Command::KeepParameters, 0 });
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

    if (m_zoomTimerId != 0) killTimer(m_zoomTimerId);
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

            // Verwendung von QPoint statt Win32-Structs
            m_startMouse = QPoint(-1, -1);
            m_bMouseOn = false;
        }
    }
}
void CMainDlg::MouseOn() {
    if (!m_bMouseOn) {
        this->unsetCursor(); // oder this->setCursor(Qt::ArrowCursor);
        m_bMouseOn = true;
    }
}

void CMainDlg::InitParametersForNewImage() {
    if (!m_bKeepParams) {
        ResetParamsToDefault();
    } else if (!m_bUserZoom) {
        m_dZoom = -1;
    }

    if (!m_bIsAnimationPlaying) {
        m_bInZooming = m_bTemporaryLowQ = m_bShowZoomFactor = false;
    }
}

void CMainDlg::AfterNewImageLoaded(bool bSynchronize, bool bAfterStartup, bool bMoveBack) {
    UpdateWindowTitle();
    m_pDirectoryWatcher->SetCurrentFile(CurrentFileName(false));
    if (bSynchronize) {
        // after loading an image, the per image processing parameters must be synchronized with
        // the current processing parameters
        if (m_pCurrentImage != NULL) {
            if (!m_bKeepParams) {
                // [GF] Do not restore these to how they were when the image was loaded before. If the user has intentionally changed these settings since that time, the current settings should be respected.

                m_nRotation = m_pCurrentImage->GetInitialRotation();
                m_dZoom = m_pCurrentImage->GetInititialZoom();
                m_offsets = m_pCurrentImage->GetInitialOffsets();

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

// Get current filename or filepath
QString CMainDlg::CurrentFileName(bool bFileNameOnly) {
    if (m_pFileList != nullptr) {
        return bFileNameOnly ? m_pFileList->CurrentFileTitle() : m_pFileList->Current();
    } else {
        return QString();
    }
}

// Full path of the current file for display (native separators). For an image in an archive the name of the
// file within the archive is appended, e.g. "/path/to/archive.zip/image.jpg".
QString CMainDlg::CurrentDisplayPath() {
    QString sPath = CurrentFileName(false);
    if (!sPath.isEmpty() && m_pCurrentImage != nullptr && m_pCurrentImage->InArchive() &&
        !m_pCurrentImage->ArchiveFileName().isEmpty()) {
        sPath += QLatin1Char('/') + m_pCurrentImage->ArchiveFileName();
    }
    return QDir::toNativeSeparators(sPath);
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


void CMainDlg::UpdateWindowTitle(bool bForce) {
    static QString s_PrevFileExt;
    static QString s_PrevTitleText;

    const bool bShowFullPathInTitle = CSettingsProvider::This().ShowFullPathInTitle();
    // full path: for images in an archive including the name of the file within the archive
    const QString sCurrentFileName = bShowFullPathInTitle ? CurrentDisplayPath() : CurrentFileName(true);

    // 1. Fenstertitel aktualisieren
    QString sWindowText;
    if (sCurrentFileName.isEmpty()) {
        sWindowText = APP_TITLE;
    } else {
        QString visibleName = QDir::toNativeSeparators(sCurrentFileName);

        QString indexStr;
        if (m_pCurrentImage != nullptr) {
            indexStr = QString(GetMultiframeIndex(m_pCurrentImage));
        }

        sWindowText = QStringLiteral("%1%2 - %3")
                          .arg(visibleName)
                          .arg(indexStr)
                          .arg(APP_TITLE);
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

        if (!sCurrentFilePath.isEmpty()) {
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

double CMainDlg::GetZoomMultiplier(CBaseImage* pImage, const QRect& clientRect) {
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
    Qt::KeyboardModifiers modifiers = QGuiApplication::queryKeyboardModifiers();
    bool bAlt = modifiers.testFlag(Qt::AltModifier);
    bool bCtrl = modifiers.testFlag(Qt::ControlModifier);
    bool bShift = modifiers.testFlag(Qt::ShiftModifier);

    const std::optional<Cmd::CommandInvocation> inv = m_pKeyMap->GetCommandForKey(nMouseButtonCode, bAlt, bCtrl, bShift);
    // DoNothing zaehlt als "belegt": die Maustaste wird verbraucht, tut aber nichts (siehe ExecuteCommand).
    if (inv) {
        if (bExecuteCommand) {
            ExecuteCommand(*inv);
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
    if (m_bTerminated == true) {
        return;
    }

    m_bTerminated = true;

    // geometry() liefert die ClientRect Fensterkoordinaten (ohne Window-Manager-Rahmen),
    // die setGeometry() beim Start auch fehlerfrei wiederherstellen kann.
    m_windowRectOnClose = geometry();

    /*
    if (!m_bMovieMode) {
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
    */

    SaveBookmark();
    CSettingsProvider::This().SaveCustomSettings();

    StopMovieMode();
    StopAnimation();

    delete m_pImageProvider; // delete this early to properly shut down the loading threads
    m_pImageProvider = nullptr;
    m_pCurrentImage = nullptr;

    close();
}
    
void CMainDlg::StartAnimation() {
    if (m_bIsAnimationPlaying) {
        return;
    }
    m_eProcFlagsBeforeMovie = CreateDefaultProcessingFlags(m_bKeepParams);
    m_bKeepParametersBeforeAnimation = IsKeepParams();
    if (!m_bKeepParametersBeforeAnimation) {
        ExecuteCommand({ Cmd::Command::KeepParameters, 0 });
    }

    m_bAutoContrast = false;
    m_bIsAnimationPlaying = true;
    m_nLastSlideShowImageTickCount = QDateTime::currentMSecsSinceEpoch();

    m_nLastAnimationOffset = 0;
    
    /*
    int usedFrameTime;
    if (m_customAnimationFrameTime < 250) {
        usedFrameTime = m_customAnimationFrameTime;
    } else {
        usedFrameTime = m_pCurrentImage->FrameTimeMs();
    }
    */
    
    int nTimerDelay = std::max(10, m_pCurrentImage->FrameTimeMs());
    m_nExpectedNextAnimationTickCount = QDateTime::currentMSecsSinceEpoch() + nTimerDelay;
    if (m_animationTimerId != 0) killTimer(m_animationTimerId);
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
        ExecuteCommand({ Cmd::Command::KeepParameters, 0 });
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

// ---------------------------------------------------------------------------------------------
// Bookmark helpers
//
// LoadBookmark(), SaveBookmark() and MigrateBookmarkOnRename() MUST derive the key in bookmarks.ini the
// same way, otherwise a bookmark written by one is not found by the others. Therefore the rules live
// here once (they used to be duplicated in LoadBookmark() and SaveBookmark()).
// ---------------------------------------------------------------------------------------------

static bool IsBookmarkArchive(const QFileInfo& fileInfo) {
    const QString fileExt = fileInfo.suffix().toLower();
    return fileExt == QStringLiteral("cbz") || fileExt == QStringLiteral("cbr") || fileExt == QStringLiteral("cb7");
}

// '[' and ']' are replaced because they are special characters in INI files.
static QString BookmarkEscape(QString sText) {
    sText.replace(QLatin1Char('['), QLatin1Char('<'));
    sText.replace(QLatin1Char(']'), QLatin1Char('>'));
    return sText;
}

// Key in bookmarks.ini: archive file name without extension, otherwise the name of the parent folder.
static QString BookmarkKeyName(const QFileInfo& fileInfo) {
    QString keyName = IsBookmarkArchive(fileInfo) ? fileInfo.completeBaseName()
                                                  : fileInfo.dir().dirName();

    // Remove language tags ([en], [de], [ch], [ko]) at the end, so all language versions share a bookmark
    static const QRegularExpression langTagRx(QStringLiteral(R"(\s+\[(en|de|ch|ko)\]$)"), QRegularExpression::CaseInsensitiveOption);
    keyName.remove(langTagRx);

    return BookmarkEscape(keyName);
}

// Moves the bookmark of a renamed file to its new name. Call after the file has been renamed, and
// before SaveBookmark() can run for the new name.
//
//  - Archives (.cbz, ...): the key is the archive name, so a rename changes the key. The entry is moved
//    to the new key, otherwise the reading position is lost and the old entry stays in the file forever.
//    If the new key already exists (another book with the same name) nothing is touched: moving would
//    destroy that bookmark, and keeping the old entry loses nothing.
//  - Folders (/manga/, /comics/): the key is the folder (unchanged by a rename), the VALUE is the file
//    name of the last viewed image. If it is the renamed file, the value is updated, otherwise
//    LoadBookmark() would later point to a file that no longer exists.
static void MigrateBookmarkOnRename(const QString& sOldPath, const QString& sNewPath) {
    const QString bookmarkFilePath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                         .filePath(QStringLiteral("bookmarks.ini"));
    if (!QFileInfo::exists(bookmarkFilePath)) {
        return;   // no bookmarks yet: nothing to migrate (and no file created just for this)
    }

    const QFileInfo oldInfo(sOldPath);
    const QFileInfo newInfo(sNewPath);
    const QString oldKey = BookmarkKeyName(oldInfo);
    const QString newKey = BookmarkKeyName(newInfo);

    QSettings iniSettings(bookmarkFilePath, QSettings::IniFormat);
    iniSettings.beginGroup(QStringLiteral("Bookmarks"));

    if (IsBookmarkArchive(oldInfo)) {
        if (oldKey != newKey && iniSettings.contains(oldKey) && !iniSettings.contains(newKey)) {
            iniSettings.setValue(newKey, iniSettings.value(oldKey));
            iniSettings.remove(oldKey);
        }
    } else {
        // same folder, so oldKey == newKey
        if (iniSettings.value(oldKey).toString() == BookmarkEscape(oldInfo.fileName())) {
            iniSettings.setValue(oldKey, BookmarkEscape(newInfo.fileName()));
        }
    }

    iniSettings.endGroup();
}

QString CMainDlg::LoadBookmark(const QString& filePath) {
    if (filePath.isEmpty()) {
        return QString();
    }

    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString bookmarkFilePath = QDir(settingsPath).filePath(QStringLiteral("bookmarks.ini"));

    if (!QFileInfo::exists(bookmarkFilePath)) {
        return QString();
    }

    const QString keyName = BookmarkKeyName(QFileInfo(filePath));

    // Auslesen der INI-Datei
    QSettings iniSettings(bookmarkFilePath, QSettings::IniFormat);
    iniSettings.beginGroup(QStringLiteral("Bookmarks"));

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
    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QDir bookmarkDir(settingsPath);
    if (!bookmarkDir.exists()) {
        if (!bookmarkDir.mkpath(QStringLiteral("."))) { // Erstellt Verzeichnis inkl. Unterordner
            return;
        }
    }

    const QString bookmarkFilePath = bookmarkDir.filePath(QStringLiteral("bookmarks.ini"));

    // 2. Pfadanalyse mit QFileInfo & QString-Methoden
    const QFileInfo fileInfo(filePath);
    const bool isArchive = IsBookmarkArchive(fileInfo);
    const QString keyNameCleaned = BookmarkKeyName(fileInfo);   // same rules as LoadBookmark()

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
    iniSettings.beginGroup(QStringLiteral("Bookmarks"));

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

// Brings all state that depends on the file name up to date after the DISPLAYED file has been renamed
// on disk (by RenameDialog). The image itself is not touched or reloaded, so it stays in the cache.
void CMainDlg::OnCurrentFileRenamed(const QString& sOldPath, const QString& sNewPath) {
    if (sNewPath.isEmpty() || m_pFileList == nullptr) {
        return;
    }

    m_pFileList->FileHasRenamed(sOldPath, sNewPath);
    if (m_pImageProvider != nullptr) {
        m_pImageProvider->FileHasRenamed(sOldPath, sNewPath);
    }

    // Only for books: archives are stored under their name, folders under the file name of the last image.
    if (IsBookModeFile(sNewPath)) {
        MigrateBookmarkOnRename(sOldPath, sNewPath);
    }

    if (QDir::cleanPath(m_sStartupFile) == QDir::cleanPath(sOldPath)) {
        m_sStartupFile = sNewPath;
    }

    UpdateWindowTitle();
    m_pDirectoryWatcher->SetCurrentFile(sNewPath);
    update();
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

double CMainDlg::MaxRatio(double valA, double valB) {
    double dRatio = (valA / valB);
    if (dRatio < 1.0)
        dRatio = 1 / dRatio;
    return dRatio;
}

void CMainDlg::SetToast(const QString& strToast, int nDurationMs)
{
    if (m_toastTimerId != 0) killTimer(m_toastTimerId);
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
QString CMainDlg::GetMultiframeIndex(CBaseImage* pImage) {
    const bool isMultiFrame = (pImage != nullptr) && (pImage->NumberOfFrames() > 1);
    
    if (isMultiFrame && !pImage->IsAnimation()) {
        return QStringLiteral(" [%1/%2]")
            .arg(pImage->FrameIndex() + 1)
            .arg(pImage->NumberOfFrames());
    }
    
    return QString();
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

    QMessageBox msgBox(this);
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

//######################################################################################
// SLOTS

// private slot
void CMainDlg::onDisplayedFileChangedOnDisk() {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged() &&
        m_pCurrentImage != NULL &&
        m_pFileList != NULL &&
        m_pFileList->CanOpenCurrentFileForReading()) {
        ExecuteCommand({ Cmd::Command::Reload, 0 });
    }
}

// private slot
void CMainDlg::onActiveDirectoryFilelistChanged() {
    if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged()) {
        if (m_pFileList != NULL && m_pFileList->CurrentFileExists()) {
            m_pFileList->Reload(NULL, false);
            update(); // Qt-Methode zum Neuzeichnen
        }

        if (m_pCurrentImage != NULL) {
            GotoImage(POS_Current, KEEP_PARAMETERS);
        }
    }
}

// private slot. Reacts to emits in OnAnotherInstanceStarted() or nativeEvent WM_COPYDATA handler which change m_sStartupFile
void CMainDlg::onLoadFileAsynch() {
    if (isEnabled() && !m_sStartupFile.isEmpty()) {
        StopMovieMode();
        StopAnimation();
        MouseOn();
        OpenFile(m_sStartupFile);
    }
}

bool CMainDlg::ShouldAcceptFile(const QString& testFilePath) {
    bool bSameImageFolder = false;
    const QString sCurrentFile = CurrentFileName(false);

    if (!sCurrentFile.isEmpty()) {
        if (m_pCurrentImage && m_pCurrentImage->ContainerHasMultipleImages()) {
            if (testFilePath.compare(sCurrentFile, Qt::CaseInsensitive) == 0) {
                if (m_bFullScreenMode) {
                    m_bFullScreenMode = false;
                    UpdateWindowTitle(true); // Aktualisiert den Titel / das Icon der Titelleiste
                    this->MouseOn();
                    m_dZoom = -1;
                    StartLowQTimer(ZOOM_TIMEOUT);
                }
                return true; // We tell the client that we handle it, client should exit.
            } else {
                return false; // We tell the client that it should open the file.
            }
        } else {
            const QString copyDataPath = QFileInfo(testFilePath).path();
            const QString currentPath  = QFileInfo(sCurrentFile).path();
            bSameImageFolder = (copyDataPath.compare(currentPath, Qt::CaseInsensitive) == 0);
        }
    }

    const bool bNoCurrentFile = sCurrentFile.isEmpty();
    const bool bSingleFullScreen = m_bFullScreenMode && CSettingsProvider::This().SingleFullScreenInstance();
    const bool bSingleInstanceAlways = (CSettingsProvider::This().SingleInstanceMode() == Helpers::SI_Always);
    const bool bSingleInstancePerFolder = (CSettingsProvider::This().SingleInstanceMode() == Helpers::SI_PerFolder) && bSameImageFolder;

    if (bNoCurrentFile || bSingleFullScreen || bSingleInstanceAlways || bSingleInstancePerFolder) {
        if (m_bFullScreenMode) {
            m_bFullScreenMode = false;
            UpdateWindowTitle(true); // Aktualisiert den Titel / das Icon der Titelleiste
            this->MouseOn();
        }

        m_sStartupFile = testFilePath;
        QMetaObject::invokeMethod(this, &CMainDlg::onLoadFileAsynch, Qt::QueuedConnection);
        return true; // We tell the client that we handle it, client should exit.
    }

    return false;
}

void CMainDlg::focusOutEvent(QFocusEvent* event) {
    m_bPanKeyUp = m_bPanKeyDown = m_bPanKeyLeft = m_bPanKeyRight = false;
    QMainWindow::focusOutEvent(event);
}

bool CMainDlg::event(QEvent* e) {
    if (m_bDropInput) {
        switch (e->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            e->accept();
            return true;            // verworfen
        default:
            break;                  // Release/Move durchlassen
        }
    }
    return QMainWindow::event(e);
}

void CMainDlg::DiscardPendingInput() {
    m_bDropInput = true;
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    m_bDropInput = false;
}


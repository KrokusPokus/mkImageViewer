#ifndef MAINDLG_H
#define MAINDLG_H

#include "resource.h"
#include "MessageDef.h"
#include "ProcessParams.h"
#include "Helpers.h"

#include <QMainWindow>
#include <QCoreApplication>
#include <QFileIconProvider>
#include <QHash>
#include <QString>

class CFileList;
class CJPEGProvider;
class CJPEGImage;
class CKeyMap;
class CDirectoryWatcher;

class QPaintEvent;
class QResizeEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QCloseEvent;
class QDropEvent;

// The main dialog is a full screen modal dialog with no border and no window title.
// This dialog is the main window of the JPEGView application.
class CMainDlg : public QMainWindow {
    Q_OBJECT // Zwingend für Qt-Widgets/Klassen mit Signals & Slots

public slots:
    void loadFileAsynch();
    void onDisplayedFileChangedOnDisk();
    void onActiveDirectoryFilelistChanged();
    void onRefreshView();

public:
    enum EImagePosition {
        POS_First,
        POS_Last,
        POS_Next,
        POS_NextSlideShow,
        POS_NextFrame,
        POS_Previous,
        POS_Current,
        POS_AwayFromCurrent,
//      POS_Next_Folder,
//      POS_Previous_Folder,
        POS_NextFile,
        POS_PreviousFile
    };

    CMainDlg(const QString& sStartupFile);
    ~CMainDlg();

    // Called by the different controller classes
    bool IsFullScreenMode() { return m_bFullScreenMode; }
    bool IsKeepParams() { return m_bKeepParams; }
    int GetRotation() { return m_nRotation; }
    CJPEGImage* GetCurrentImage() { return m_pCurrentImage; }
    QString CurrentFileName(bool bFileNameOnly);
    const QRect& WindowRectOnClose() { return m_windowRectOnClose; } // only valid after having closed the window
    double GetZoomMultiplier(CJPEGImage* pImage, const QRect& clientRect);
    Helpers::EAutoZoomMode GetAutoZoomMode() { return m_bFullScreenMode ? m_eAutoZoomModeFullscreen : m_eAutoZoomModeWindowed; }
    CPoint GetOffsets() { return m_offsets; }
    CImageProcessingParams* GetImageProcessingParams() { return m_pImageProcParams; }
    EProcessingFlags CreateDefaultProcessingFlags(bool bKeepParams = false);
    void DisplayErrors(CJPEGImage* pCurrentImage, const QRect& clientRect, QPainter& painter);

    void UpdateWindowTitle(bool bForce = false);
    void MouseOff();
    void MouseOn();
    void GotoImage(EImagePosition ePos);
    void ReloadImage(bool keepParameters, bool updateWindow = true);
    void ResetZoomTo100Percents(bool bZoomToMouse);
    void ResetZoomToFitScreen(bool bFillWithCrop, bool bAllowEnlarge, bool bAdjustWindowSize);
    bool PerformPan(int dx, int dy, bool bAbsolute);
    void StartDragging(int nX, int nY, bool bDragWithZoomNavigator);
    void DoDragging();
    void EndDragging();
    void SetCursorForMoveSection();
    void ExecuteCommand(int nCommand);
    void AdjustWindowToImage(bool bAfterStartup);
    bool IsAdjustWindowToImage();
    bool IsImageExactlyFittingWindow();

private:

    QString m_sStartupFile; // file passed on command line
    int m_nAutoStartSlideShow; // if positive: Auto start slide show with given interval in seconds, passed on command line
    bool m_bAutoExit;
    Helpers::ESorting m_eForcedSorting; // forced sorting mode on command line
    CFileList* m_pFileList; // used for navigation
    CDirectoryWatcher* m_pDirectoryWatcher = NULL; // notifies the main window when the current file changed or a file in the current directory was added or deleted
    CJPEGProvider * m_pJPEGProvider; // reads image (of any format, not only JPEGs) files, using read ahead
    CJPEGImage * m_pCurrentImage; // currently displayed image
    bool m_bOutOfMemoryLastImage; // true if the last image could not be requested because not enough memory
    bool m_bExceptionErrorLastImage; // true if the last image could not be requested because of an unhandled exception
    int m_nLastLoadError; // one of EFileLoadError
    
    // Current parameter set
    int m_nRotation; // this can only be 0, 90, 180 or 270
    int m_nUserRotation; // Rotation delta from user, can only be 0, 90, 180 or 270
    bool m_bUserZoom;
    bool m_bUserPan; // user has zoomed and panned away from default values
    bool m_bResizeForNewImage;
    double m_dZoom, m_dRealizedZoom;
    double m_dStartZoom; // zoom when start zoomin in zoom mode
    double m_dZoomAtResizeStart; // zoom factor when user started resizing JPEGView main window
    double m_dZoomMult;
    bool m_bZoomMode;
    bool m_bZoomModeOnLeftMouse;
    Helpers::EAutoZoomMode m_eAutoZoomModeWindowed;
    Helpers::EAutoZoomMode m_eAutoZoomModeFullscreen;
    Helpers::EAutoZoomMode m_autoZoomFitToScreen;
    Helpers::ESorting m_eSortModeBeforeRandom;
    bool m_isUserFitToScreen;

    CImageProcessingParams* m_pImageProcParams;
    bool m_bHQResampling;
    bool m_bAutoContrast;
    bool m_bKeepParams;

    // used to enable switch between two sets of parameters with CTRL-A
    CImageProcessingParams* m_pImageProcParams2;
    EProcessingFlags m_eProcessingFlags2;

    // set of parameters used when m_bKeepParams is true
    CImageProcessingParams* m_pImageProcParamsKept;
    EProcessingFlags m_eProcessingFlagsKept;
    double m_dZoomKept;
    QPoint m_offsetKept;
    bool m_bCurrentImageInParamDB;
    bool m_bCurrentImageIsSpecialProcessing;

    bool m_bDragging;
    bool m_bDoDragging;
    bool m_bMovieMode;
    double m_dMovieFPS;
    bool m_bProcFlagsTouched;
    EProcessingFlags m_eProcFlagsBeforeMovie;
    QPoint m_offsets; // Note: These offsets are center of image based
    QPoint m_DIBOffsets;
    int m_nCapturedX, m_nCapturedY;
    int m_nMouseX, m_nMouseY;
    bool m_bFullScreenMode;
    bool m_bAutoFitWndToImage;
    bool m_bLockPaint;
    int m_nCurrentTimeout;
    POINT m_startMouse;
    QSize m_virtualImageSize;
    bool m_bInZooming;
    bool m_bTemporaryLowQ;
    bool m_bShowZoomFactor;
    bool m_bSpanVirtualDesktop;
    bool m_bPanMouseCursorSet;
    bool m_bMouseOn;
    bool m_bKeepParametersBeforeAnimation;
    bool m_bIsAnimationPlaying;
    int m_nLastAnimationOffset;
    qint64 m_nExpectedNextAnimationTickCount;
    int m_nMonitor;
    WINDOWPLACEMENT m_storedWindowPlacement;
    QRect m_monitorRect;
    QRect m_clientRect;
    QRect m_windowRectOnClose;
    CKeyMap* m_pKeyMap;
    Helpers::ETransitionEffect m_eTransitionEffect;
    int m_nTransitionTime;
    qint64 m_nLastSlideShowImageTickCount;
    Helpers::ETransparencyMode m_eTransparencyMode; //for transparent background
    bool m_bUseEmbeddedColorProfiles;
    bool m_bSelectZoom;  // keeps track of select-to-zoom mode when CTRL+SHIFT+LMouse

    void ExploreFile();
    void OpenFile(const QString& fileName, bool bAfterStartup);
    void GotoImage(EImagePosition ePos, int nFlags);
    bool PerformZoom(double dValue, bool bExponent, bool bZoomToMouse, bool bAdjustWindowToImage);
    double GetZoomFactorForFitToScreen(bool bFillWithCrop, bool bAllowEnlarge);
    CProcessParams CreateProcessParams(bool bNoProcessingAfterLoad);
    void ResetParamsToDefault();
    void StartSlideShowTimer(int nMilliSeconds);
    void StopSlideShowTimer(void);
    void StartMovieMode(double dFPS);
    void StopMovieMode();
    void StartLowQTimer(int nTimeout);
    void InitParametersForNewImage();
    void ExchangeProcessingParams();
    void SaveParameters();
    void AfterNewImageLoaded(bool bSynchronize, bool bAfterStartup, bool bMoveBack = false);

    QRect GetZoomTextRect();
    QRect GetToastTextRect();
    void EditINIFile(bool bGlobalINI);
    int GetLoadErrorAfterOpenFile();
    void CheckIfApplyAutoFitWndToImage(bool bInInitDialog);
    void PrefetchDIB(const QRect& clientRect);
    bool HandleMouseButtonByKeymap(int nMouseButton, bool bExecuteCommand = true);
    bool UseSlideShowTransitionEffect();
    //void PaintToDC(QPainter& painter);
    //void AnimateTransition();
    void CleanupAndTerminate();
    
    // this is for animated GIFs
    void StartAnimation();
    void AdjustAnimationFrameTime();
    void StopAnimation();

    BOOL m_bDWMenabled;
    typedef HRESULT (WINAPI *MyDwmIsCompositionEnabledType)(BOOL*);
    typedef HRESULT (WINAPI *MyDwmFlushType)(void);
    MyDwmFlushType m_DynDwmFlush;

    bool m_bIsRunningInWine;

    WINDOWPLACEMENT m_storedWindowPlacement2;    // position for windowed mode
    bool m_bShowInfo;
    bool m_bPanTimerActive;
    int m_nBookModePageHeight;
    bool  m_bAllowSmoothPanning;
    QPoint m_offsets_custom;

    void ChangeFolderLanguage();
    void OpenDefaultEditor();
    bool showDeleteConfirmationDialog(const QStringList &pathList, bool bRecycleOnly);

    QString LoadBookmark(const QString& sFilePath);
    void SaveBookmark();
    bool IsBookModeFile(const QString& filePath);
    float MaxRatio(float valA, float valB);
    QString GetMultiframeIndex(CJPEGImage* pImage);    // Gets an index string of the form [a/b] for multiframe images, empty string for single frame images
    QString FormatFileSize(qint64 nFileSize);
    QString formatOutputZoomFactor(double m_dZoom);
    bool isRunningInWine();
    bool IsLinuxBinaryAvailable(const QString& unixPath);
    QString GetUnixPathFromWine(const QString& szDosPath);
    void onShowContextMenu(const QPoint &pos);

    // [sdneon] Toast stuff
    QString m_strToast;
    void SetToast(const QString& strToast, int nDurationMs = 3000);
    void ToastSortingMode(Helpers::ESorting nSortMode, bool bUpCounting);
    
    int m_customAnimationFrameTime;

    struct IconResult {
        HICON hBig = nullptr;
        HICON hSmall = nullptr;
    };
    inline static QHash<QString, IconResult> s_iconCache; // Klassenweiter Cache pro Datei-Endung

    QFileIconProvider m_iconProvider;

    // Member-Variablen zur Verwaltung der Qt-Timer-IDs
    int m_animationTimerId = 0;
    int m_slideshowTimerId = 0;
    int m_zoomTimerId = 0;
    int m_zoomTextTimerId = 0;
    int m_toastTimerId = 0;
    int m_panTimerId = 0;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void timerEvent(QTimerEvent* event) override;
};


#endif // MAINDLG_H

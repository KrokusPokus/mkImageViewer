#ifndef MAINDLG_H
#define MAINDLG_H

#define KEY_MAGIC 2978465

#include "ProcessParams.h"
#include "Helpers.h"
#include "Commands.h"

#include <QMainWindow>
#include <QCoreApplication>
#include <QFileIconProvider>
#include <QHash>
#include <QString>

class CFileList;
class CImageProvider;
class CBaseImage;
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
// This dialog is the main window of the application.
class CMainDlg : public QMainWindow {
    Q_OBJECT // Zwingend für Qt-Widgets/Klassen mit Signals & Slots

private slots:
    void onDisplayedFileChangedOnDisk();
    void onActiveDirectoryFilelistChanged();

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
    bool IsMaximizedMode() { return isMaximized(); }
    bool IsKeepParams() { return m_bKeepParams; }
    int GetRotation() { return m_nRotation; }
    CBaseImage* GetCurrentImage() { return m_pCurrentImage; }
    QString CurrentFileName(bool bFileNameOnly);
    QString CurrentDisplayPath();
    const QRect& WindowRectOnClose() { return m_windowRectOnClose; } // only valid after having closed the window
    double GetZoomMultiplier(CBaseImage* pImage, const QRect& clientRect);
    Helpers::EAutoZoomMode GetAutoZoomMode() { return m_bFullScreenMode ? m_eAutoZoomModeFullscreen : m_eAutoZoomModeWindowed; }
    CPoint GetOffsets() { return m_offsets; }
    CImageProcessingParams* GetImageProcessingParams() { return m_pImageProcParams; }
    EProcessingFlags CreateDefaultProcessingFlags(bool bKeepParams = false);
    // Current processing parameters as passed to the image processing, with the black and white mode applied
    CImageProcessingParams GetEffectiveProcessingParams() const;
    void DisplayErrors(CBaseImage* pCurrentImage, const QRect& clientRect, QPainter& painter);

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
    void ExecuteCommand(const Cmd::CommandInvocation& inv);
    bool ShouldAcceptFile(const QString& filePath);

private:

    QString m_sStartupFile; // file passed on command line
    CFileList* m_pFileList; // used for navigation
    CDirectoryWatcher* m_pDirectoryWatcher = NULL; // notifies the main window when the current file changed or a file in the current directory was added or deleted
    CImageProvider * m_pImageProvider; // reads image (of any format, not only JPEGs) files, using read ahead
    CBaseImage * m_pCurrentImage; // currently displayed image
    // Result of the last image request: position (file, frame, number of frames) and why the image could not be
    // loaded. The position is known also without an image, so navigation (e.g. through the pages of an archive)
    // does not depend on whether the current page could be read.
    CImagePosition m_currentPosition;
    ELoadError m_eLoadErrorReason = ELoadError::None;
    // Takes over image, position and reason of an image request
    void TakeLoadResult(const CImageLoadResult& result);
    // Position of the current file of the file list (frame 0 of 1 if nothing is known about it)
    CImagePosition CurrentPosition() const;
    int m_nLastLoadError; // one of EFileLoadError
    
    // Current parameter set
    int m_nRotation; // this can only be 0, 90, 180 or 270
    int m_nUserRotation; // Rotation delta from user, can only be 0, 90, 180 or 270
    bool m_bUserZoom;
    bool m_bUserPan; // user has zoomed and panned away from default values
    bool m_bResizeForNewImage;
    double m_dZoom, m_dRealizedZoom;
    double m_dStartZoom; // zoom when start zoomin in zoom mode
    double m_dZoomAtResizeStart; // zoom factor when user started resizing main window
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
    bool m_bBWMode = false; // black and white mode (saturation 0), applies to all images until toggled off
    bool m_bKeepParams;

    // set of parameters used when m_bKeepParams is true
    CImageProcessingParams* m_pImageProcParamsKept;
    EProcessingFlags m_eProcessingFlagsKept;
    double m_dZoomKept;
    QPoint m_offsetKept;
    bool m_bCurrentImageIsSpecialProcessing;

    bool m_bDragging;
    bool m_bDoDragging;
    bool m_bMovieMode;
    double m_dMovieFPS;
    QPoint m_offsets; // Note: These offsets are center of image based
    QPoint m_DIBOffsets;
    int m_nCapturedX, m_nCapturedY;
    int m_nMouseX, m_nMouseY;
    bool m_bFullScreenMode;
    bool m_bLockPaint;
    int m_nCurrentTimeout;
    QPoint m_startMouse;
    QSize m_virtualImageSize;
    bool m_bInZooming;
    bool m_bTemporaryLowQ;
    bool m_bShowZoomFactor;
    bool m_bSpanVirtualDesktop;
    bool m_bMouseOn;
    bool m_bIsAnimationPlaying;
    int m_nLastAnimationOffset;
    qint64 m_nExpectedNextAnimationTickCount;
    int m_nMonitor;
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
    void OpenFile(const QString& fileName);
    void GotoImage(EImagePosition ePos, int nFlags);
    bool PerformZoom(double dValue, bool bExponent, bool bZoomToMouse);
    double GetZoomFactorForFitToScreen(bool bFillWithCrop, bool bAllowEnlarge);
    CProcessParams CreateProcessParams(bool bNoProcessingAfterLoad);
    void ResetParamsToDefault();
    void StartSlideShowTimer(int nMilliSeconds);
    void StopSlideShowTimer(void);
    void StartMovieMode(double dFPS);
    void StopMovieMode();
    // Slide show with more than kMovieFps images per second ("movie"): animations are not played there
    bool IsFastMovieMode() const;
    // Zoom, offsets and rotation are kept from image to image while a movie or an animation runs.
    // F3/F4 (resampling quality, auto contrast) are never changed by these modes, they are up to the user.
    void UpdateKeepParams();
    void StartLowQTimer(int nTimeout);
    void InitParametersForNewImage();
    // Keep zoom, offsets, rotation and processing parameters between images (only used internally while an
    // animation or the movie mode is running). Switching it on takes over the current state.
    void SetKeepParams(bool bKeep);
    void AfterNewImageLoaded(bool bSynchronize, bool bAfterStartup, bool bMoveBack = false);
    void onLoadFileAsynch(); // WM_COPYDATAhandler and OnAnotherInstanceStarted() call this via invokeMethod()
    void OnCurrentFileRenamed(const QString& sOldPath, const QString& sNewPath);

    QRect GetZoomTextRect();
    QRect GetToastTextRect();
    int GetLoadErrorAfterOpenFile();
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

    bool m_bIsRunningInWine;

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
    double MaxRatio(double valA, double valB);
    QString GetMultiframeIndex(const CImagePosition& position);    // Gets an index string of the form [a/b] for multiframe images, empty string for single frame images
    QString formatOutputZoomFactor(double m_dZoom);
    void onShowContextMenu(const QPoint &pos);

    // [sdneon] Toast stuff
    QString m_strToast;
    void SetToast(const QString& strToast, int nDurationMs = 3000);
    void ToastSortingMode(Helpers::ESorting nSortMode, bool bUpCounting);
    
    int m_customAnimationFrameTime;
    bool m_bTerminated{false};

    QFileIconProvider m_iconProvider;

    // Member-Variablen zur Verwaltung der Qt-Timer-IDs
    int m_animationTimerId = 0;
    int m_slideshowTimerId = 0;
    int m_zoomTimerId = 0;
    int m_zoomTextTimerId = 0;
    int m_toastTimerId = 0;
    int m_panTimerId = 0;

    bool m_bPanKeyUp = false;
    bool m_bPanKeyDown = false;
    bool m_bPanKeyLeft = false;
    bool m_bPanKeyRight = false;
    bool m_bDropInput = false;
    void DiscardPendingInput();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent*) override;
};

#endif // MAINDLG_H

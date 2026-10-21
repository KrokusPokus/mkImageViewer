#ifndef MAINDLG_H
#define MAINDLG_H

#include "resource.h"
#include "MessageDef.h"
#include "ProcessParams.h"
#include "Helpers.h"

#include <QCoreApplication>
#include <QFileIconProvider>
#include <QHash>
#include <QString>

class CFileList;
class CJPEGProvider;
class CJPEGImage;
class CKeyMap;
class CDirectoryWatcher;

enum EMouseEvent;

// The main dialog is a full screen modal dialog with no border and no window title.
// This dialog is the main window of the JPEGView application.
class CMainDlg : public CDialogImpl<CMainDlg> {
    Q_DECLARE_TR_FUNCTIONS(TableItemDelegate)

public:
    enum { IDD = IDD_MAINDLG };

    // Used in GotoImage() call
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

    BEGIN_MSG_MAP(CMainDlg)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNCHitTest)
        MESSAGE_HANDLER(WM_NCLBUTTONDOWN, OnNCLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDblClk)
        MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
        MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
        MESSAGE_HANDLER(WM_MBUTTONDOWN, OnMButtonDown)
        MESSAGE_HANDLER(WM_MBUTTONUP, OnMButtonUp)
        MESSAGE_HANDLER(WM_XBUTTONDOWN, OnXButtonDown)
        MESSAGE_HANDLER(WM_XBUTTONDBLCLK, OnXButtonDown)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_SYSKEYDOWN, OnSysKeyDown)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
        MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
        MESSAGE_HANDLER(WM_IMAGE_LOAD_COMPLETED, OnImageLoadCompleted)
        MESSAGE_HANDLER(WM_DISPLAYED_FILE_CHANGED_ON_DISK, OnDisplayedFileChangedOnDisk)
        MESSAGE_HANDLER(WM_ACTIVE_DIRECTORY_FILELIST_CHANGED, OnActiveDirectoryFilelistChanged)
        MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_LOAD_FILE_ASYNCH, OnLoadFileAsynch)
        MESSAGE_HANDLER(WM_COPYDATA, OnAnotherInstanceStarted)
/*GF*/  MESSAGE_HANDLER(WM_RBUTTONDBLCLK, OnRButtonDown)
/*GF*/  MESSAGE_HANDLER(WM_MBUTTONDBLCLK, OnMButtonDown)
/*GF*/  MESSAGE_HANDLER(WM_REFRESHVIEW, OnRefreshView)        // (to refresh the view from another app when window is not in foreground)
        COMMAND_ID_HANDLER(IDOK, OnOK)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

// Handler prototypes (uncomment arguments if needed):
//    LRESULT MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
//    LRESULT CommandHandler(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
//    LRESULT NotifyHandler(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/)


    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnEraseBackground(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnSize(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnGetMinMaxInfo(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnNCLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnNCHitTest(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnRButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnRButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnLButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnLButtonDblClk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnMButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnMButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnXButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnMouseMove(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnMouseWheel(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnKeyDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnSysKeyDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnGetDlgCode(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnTimer(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnContextMenu(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnCtlColorEdit(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnImageLoadCompleted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnDisplayedFileChangedOnDisk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnActiveDirectoryFilelistChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnDropFiles(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnAnotherInstanceStarted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnLoadFileAsynch(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
    LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
/*GF*/    LRESULT OnRefreshView(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);

    // Called by the different controller classes
    bool IsFullScreenMode() { return m_bFullScreenMode; }
    bool IsKeepParams() { return m_bKeepParams; }
    int GetRotation() { return m_nRotation; }
    CJPEGImage* GetCurrentImage() { return m_pCurrentImage; }
    QString CurrentFileName(bool bFileNameOnly);
    const CRect& WindowRectOnClose() { return m_windowRectOnClose; } // only valid after having closed the window
    double GetZoomMultiplier(CJPEGImage* pImage, const CRect& clientRect);
    Helpers::EAutoZoomMode GetAutoZoomMode() { return m_bFullScreenMode ? m_eAutoZoomModeFullscreen : m_eAutoZoomModeWindowed; }
    CPoint GetOffsets() { return m_offsets; }
    CImageProcessingParams* GetImageProcessingParams() { return m_pImageProcParams; }
    EProcessingFlags CreateDefaultProcessingFlags(bool bKeepParams = false);
    void DisplayErrors(CJPEGImage* pCurrentImage, const CRect& clientRect, CDC& dc);

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
    int TrackPopupMenu(CPoint pos, HMENU hMenu);
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
    CPoint m_offsetKept;
    bool m_bCurrentImageInParamDB;
    bool m_bCurrentImageIsSpecialProcessing;

    bool m_bDragging;
    bool m_bDoDragging;
    bool m_bMovieMode;
    double m_dMovieFPS;
    bool m_bProcFlagsTouched;
    EProcessingFlags m_eProcFlagsBeforeMovie;
    bool m_bInTrackPopupMenu;
    CPoint m_offsets; // Note: These offsets are center of image based
    CPoint m_DIBOffsets;
    int m_nCapturedX, m_nCapturedY;
    int m_nMouseX, m_nMouseY;
    bool m_bFullScreenMode;
    bool m_bAutoFitWndToImage;
    bool m_bLockPaint;
    int m_nCurrentTimeout;
    POINT m_startMouse;
    CSize m_virtualImageSize;
    bool m_bInZooming;
    bool m_bTemporaryLowQ;
    bool m_bShowZoomFactor;
    bool m_bSpanVirtualDesktop;
    bool m_bPanMouseCursorSet;
    bool m_bMouseOn;
    bool m_bKeepParametersBeforeAnimation;
    bool m_bIsAnimationPlaying;
    int m_nLastAnimationOffset;
    int m_nExpectedNextAnimationTickCount;
    int m_nMonitor;
    WINDOWPLACEMENT m_storedWindowPlacement;
    CRect m_monitorRect;
    CRect m_clientRect;
    CRect m_windowRectOnClose;
    CKeyMap* m_pKeyMap;
    Helpers::ETransitionEffect m_eTransitionEffect;
    int m_nTransitionTime;
    DWORD m_nLastSlideShowImageTickCount;
    Helpers::ETransparencyMode m_eTransparencyMode; //for transparent background
    bool m_bUseEmbeddedColorProfiles;
    bool m_bWindowBorderless;
    bool m_bAlwaysOnTop;
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

    void ToggleMonitor();
    CRect GetZoomTextRect();
    CRect GetToastTextRect();
    void EditINIFile(bool bGlobalINI);
    int GetLoadErrorAfterOpenFile();
    void CheckIfApplyAutoFitWndToImage(bool bInInitDialog);
    void PrefetchDIB(const CRect& clientRect);
    bool HandleMouseButtonByKeymap(int nMouseButton, bool bExecuteCommand = true);
    bool UseSlideShowTransitionEffect();
    void PaintToDC(CDC& dc);
    void AnimateTransition();
    void CleanupAndTerminate();
    LONG SetCurrentWindowStyle();
    
    // this is for animated GIFs
    void StartAnimation();
    void AdjustAnimationFrameTime();
    void StopAnimation();
    void ToggleAlwaysOnTop();

    HMODULE m_hmodDwmapi;
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
    CPoint m_offsets_custom;

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

    // [sdneon] Toast stuff
    QString m_strToast;
    void SetToast(const QString& strToast, DWORD nDurationMs = 3000);
    void ToastSortingMode(Helpers::ESorting nSortMode, bool bUpCounting);
    
    int m_customAnimationFrameTime;

    struct IconResult {
        HICON hBig = nullptr;
        HICON hSmall = nullptr;
    };
    inline static QHash<QString, IconResult> s_iconCache; // Klassenweiter Cache pro Datei-Endung

    QFileIconProvider m_iconProvider;
};


#endif // MAINDLG_H

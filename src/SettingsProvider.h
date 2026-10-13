#ifndef SETTINGSPROVIDER_H
#define SETTINGSPROVIDER_H

#include "Helpers.h"
#include "FileList.h"
#include "ProcessParams.h"

#include <unordered_map>
#include <string>

using IniHashMap = std::unordered_map<std::wstring, std::wstring>;

// INI settings
// All settings are first searched in a file name JPEGView.ini located in User/AppData/Roaming/JPEGView
// If not found there, the setting is searched in JPEGView.ini located in the EXE path of the application
class CSettingsProvider
{
public:
    // Singleton instance
    static CSettingsProvider& This();

    // The methods correspond to the INI file settings
    bool StoreToEXEPath() { return m_bStoreToEXEPath; }
    double Contrast() { return m_dContrast; }
    double Gamma() { return m_dGamma; }
    double Saturation() { return m_dSaturation; }
    double Sharpen() { return m_dSharpen; }
    double CyanRed() { return m_dCyanRed; }
    double MagentaGreen() { return m_dMagentaGreen; }
    double YellowBlue() { return m_dYellowBlue; }
    bool HighQualityResampling() { return m_bHQRS; }
    bool DefaultSelectionMode() { return m_bDefaultSelectionMode; }
    bool ShowFileName() { return m_bShowFileName; }
    bool ShowEXIFDateInTitle() { return m_bShowEXIFDateInTitle; }
    bool ShowFullPathInTitle() { return m_bShowFullPathInTitle; }
    bool ShowFullScreen() { return m_bShowFullScreen; }
    bool AutoFullScreen() { return m_bAutoFullScreen; }
    bool KeepParams() { return m_bKeepParams; }
    LPCTSTR Language() { return m_sLanguage; }
    Helpers::CPUType AlgorithmImplementation() { return m_eCPUAlgorithm; }
    int NumberOfCoresToUse() { return m_nNumCores; }
    EFilterType DownsamplingFilter() { return m_eDownsamplingFilter; }
    Helpers::ESorting Sorting() { return m_eSorting; }
    bool IsSortedAscending() { return m_bIsSortedAscending; }
    Helpers::ENavigationMode Navigation() { return m_eNavigation; }
    bool NavigateWithMouseWheel() { return m_bNavigateMouseWheel; }
    double MouseWheelZoomSpeed() { return m_dMouseWheelZoomSpeed; }
    Helpers::EAutoZoomMode AutoZoomMode() { return m_eAutoZoomMode; }
    Helpers::EAutoZoomMode AutoZoomModeFullscreen() { return m_eAutoZoomModeFullscreen; }
    int DisplayMonitor() { return m_nDisplayMonitor; }
    void SetMonitorOverride(int nMonitor) { m_nDisplayMonitor = nMonitor; }
    bool AutoContrastCorrection() { return m_bAutoContrastCorrection; }
    double AutoContrastAmount() { return m_dAutoContrastAmount; }
    float* ColorCorrectionAmounts(); // can't be declared inline due to compiler bug...sad but true
    double AutoBrightnessAmount() { return m_dAutoBrightnessAmount; }
    bool LocalDensityCorrection() { return m_bLocalDensityCorrection; }
    double BrightenShadows() { return m_dBrightenShadows; }
    double DarkenHighlights() { return m_dDarkenHighlights; }
    double BrightenShadowsSteepness() { return m_dBrightenShadowsSteepness; }
    CImageProcessingParams LandscapeModeParams(const CImageProcessingParams& templParams);
    bool LandscapeMode() { return m_bLandscapeMode; }
    int MaxSlideShowFileListSize() { return m_nMaxSlideShowFileListSize; }
    Helpers::ETransitionEffect SlideShowTransitionEffect() { return m_eSlideShowTransitionEffect; }
    int SlideShowEffectTimeMs() { return m_nSlideShowEffectTimeMs; }
    bool ForceGDIPlus() { return m_bForceGDIPlus; }
    Helpers::ESingleInstanceMode SingleInstanceMode() { return m_eSingleInstanceMode; }
    bool SingleFullScreenInstance() { return m_bSingleFullScreenInstance; }
    LPCTSTR FileEndingsRAW() { return m_sFileEndingsRAW; }
    void AddTemporaryRAWFileEnding(LPCTSTR sEnding) { m_sFileEndingsRAW += CString(_T(";*.")) + sEnding; }
    Helpers::EDeleteConfirmation DeleteConfirmation() { return m_eDeleteConfirmation; }
    bool AllowFileDeletion() { return m_bAllowFileDeletion; }
    bool WrapAroundFolder() { return m_bWrapAroundFolder && m_eNavigation == Helpers::NM_LoopDirectory; }
    bool ExchangeXButtons() { return m_bExchangeXButtons; }
    bool AutoRotateEXIF() { return m_bAutoRotateEXIF; }
    bool UseEmbeddedColorProfiles() { return m_bUseEmbeddedColorProfiles; }
    LPCTSTR ACCExclude() { return m_sACCExclude; }
    LPCTSTR ACCInclude() { return m_sACCInclude; }
    LPCTSTR LDCExclude() { return m_sLDCExclude; }
    LPCTSTR LDCInclude() { return m_sLDCInclude; }
    CRect DefaultWindowRect() { return m_defaultWindowRect; }
    CRect StickyWindowRect() { return m_stickyWindowRect; }
    bool StickyWindowSize() { return m_bStickyWindowSize; }
    bool DefaultWndToImage() { return m_bDefaultWndToImage; }
    bool DefaultMaximized() { return m_bDefaultMaximized; }
    bool ExplicitWindowRect() { return m_bExplicitWindowRect; }
    COLORREF ColorBackground() { return m_colorBackground; }
    COLORREF ColorGUI() { return m_colorGUI; }
    COLORREF ColorTransparency() { return m_colorTransparency; }
    LPCTSTR DefaultGUIFont() { return m_defaultGUIFont; }
    LPCTSTR FileNameFont() { return m_fileNameFont; }
    bool ReloadWhenDisplayedImageChanged() { return m_bReloadWhenDisplayedImageChanged; }
    bool AllowEditGlobalSettings() { return m_bAllowEditGlobalSettings; }
    CSize MinimalWindowSize() { return m_minimalWindowSize; }
    int MinimalDisplayTime() { return m_minimalDisplayTime; }
    Helpers::EIniEditor IniEditor() { return m_eIniEditor; }
    LPCTSTR CustomIniEditor() { return m_sIniEditor; }
    bool WindowBorderlessOnStartup() { return m_bWindowBorderlessOnStartup; }
    bool WindowAlwaysOnTopOnStartup() { return m_bWindowAlwaysOnTopOnStartup; }

    double ZoomPauseFactor() { return m_zoomPauseFactor; }  // while internally this is represented in doubles, using a whole number percent simplifies it for the user... configuring doubles is not user friendly at all

    // Returns if a user INI file exists
    bool ExistsUserINI();
    // Copies the user INI file (in AppData/Roaming) from the INI file template JPEGView.ini.tpl
    void CopyUserINIFromTemplate();

    // This will only save a subset of settings to the INI file located in AppData\JPEGView\JPEGView.ini.
    // Note that this INI file has precedence over the INI file at the program directory
    void SaveSettings(const CImageProcessingParams& procParams,
        EProcessingFlags eProcFlags,
        Helpers::ENavigationMode eNavigationMode, Helpers::ESorting eFileSorting, bool isSortedAscending,
        Helpers::EAutoZoomMode eAutoZoomMode, Helpers::EAutoZoomMode eAutoZoomModeFullScreen,
        bool bShowNavPanel, bool bShowFileName, bool bShowFileInfo,
        Helpers::ETransitionEffect eSlideShowTransitionEffect);

    // Saves the sticky window size to the INI file
    void SaveStickyWindowRect(CRect rect);
    
    // Update user settings with settings from INI file template
    void UpdateUserSettings();

    // Gets the path where the global INI file and the EXE is located
    LPCTSTR GetEXEPath() { return m_sEXEPath; }

    // Get the file name with path of the global INI file (in EXE path)
    LPCTSTR GetGlobalINIFileName() { return m_sIniNameGlobal; }
    // Get the file name with path of the user INI file (in AppData path)
    LPCTSTR GetUserINIFileName() { return m_sIniNameUser; }
    // Get the file name (without path) of the user INI file
    LPCTSTR GetINIFileTitle() { return m_sIniFileTitle; }

/*GF*/    int BookModePageHeight() { return m_nBookModePageHeight; }
/*GF*/    bool BookModeLaunchFullscreen() { return m_bBookModeLaunchFullscreen; }
/*GF*/    bool AllowSmoothPanning() { return m_bSmoothPanning; }
/*GF*/    bool SmartPanningKeys() { return m_bSmartPanningKeys; }
/*GF*/    bool TitleBarUseFileIcon() { return m_bTitleBarUseFileIcon; }
/*GF*/    int CacheRange() { return m_nCacheRange; }
/*GF*/    Helpers::ETransparencyMode TransparencyMode() {return m_eTransparencyMode; }
/*GF*/    void SetBookModePageHeight(int nBookModePageHeight) { m_nBookModePageHeight = nBookModePageHeight; }
/*GF*/    void SetUseEmbeddedColorProfiles(int bUseEmbeddedColorProfiles) { m_bUseEmbeddedColorProfiles = bUseEmbeddedColorProfiles; }
/*GF*/    void SaveCustomSettings();

private:
    static CSettingsProvider* sm_instance;
    CString m_sEXEPath;
    CString m_sIniNameGlobal;
    CString m_sIniNameUser;
    CString m_sIniFileTitle;
    bool m_bUserINIExists;
    bool m_bStoreToEXEPath;

    double m_dContrast;
    double m_dGamma;
    double m_dSaturation;
    double m_dSharpen;
    double m_dCyanRed;
    double m_dMagentaGreen;
    double m_dYellowBlue;
    bool m_bHQRS;
    bool m_bDefaultSelectionMode;
    bool m_bShowFileName;
    bool m_bShowEXIFDateInTitle;
    bool m_bShowFullPathInTitle;
    bool m_bShowFullScreen;
    bool m_bAutoFullScreen;
    bool m_bKeepParams;
    CString m_sLanguage;
    Helpers::CPUType m_eCPUAlgorithm;
    int m_nNumCores;
    EFilterType m_eDownsamplingFilter;
    Helpers::ESorting m_eSorting;
    bool m_bIsSortedAscending;
    Helpers::ENavigationMode m_eNavigation;
    bool m_bNavigateMouseWheel;
    double m_dMouseWheelZoomSpeed;
    Helpers::EAutoZoomMode m_eAutoZoomMode;
    Helpers::EAutoZoomMode m_eAutoZoomModeFullscreen;
    int m_nDisplayMonitor;
    bool m_bAutoContrastCorrection;
    double m_dAutoContrastAmount;
    float m_fColorCorrections[6];
    double m_dAutoBrightnessAmount;
    bool m_bLocalDensityCorrection;
    double m_dBrightenShadows;
    double m_dDarkenHighlights;
    double m_dBrightenShadowsSteepness;
    CString m_sLandscapeModeParams;
    bool m_bLandscapeMode;
    int m_nMaxSlideShowFileListSize;
    Helpers::ETransitionEffect m_eSlideShowTransitionEffect;
    int m_nSlideShowEffectTimeMs;
    bool m_bForceGDIPlus;
    Helpers::ESingleInstanceMode m_eSingleInstanceMode;
    bool m_bSingleFullScreenInstance;
    CString m_sFileEndingsRAW;
    bool m_bWrapAroundFolder;
    Helpers::EDeleteConfirmation m_eDeleteConfirmation;
    bool m_bAllowFileDeletion;
    bool m_bExchangeXButtons;
    bool m_bAutoRotateEXIF;
    bool m_bUseEmbeddedColorProfiles;
    CString m_sACCExclude;
    CString m_sACCInclude;
    CString m_sLDCExclude;
    CString m_sLDCInclude;
    CRect m_defaultWindowRect;
    CRect m_stickyWindowRect;
    bool m_bDefaultMaximized;
    bool m_bDefaultWndToImage;
    bool m_bStickyWindowSize;
    bool m_bExplicitWindowRect;
    COLORREF m_colorBackground;
    COLORREF m_colorGUI;
    COLORREF m_colorTransparency;
    CString m_defaultGUIFont;
    CString m_fileNameFont;
    bool m_bReloadWhenDisplayedImageChanged;
    bool m_bAllowEditGlobalSettings;
    CSize m_minimalWindowSize;
    int m_minimalDisplayTime;
    Helpers::EIniEditor  m_eIniEditor;
    CString m_sIniEditor;
    int m_zoomPauseFactor;
    bool m_bWindowBorderlessOnStartup;
    bool m_bWindowAlwaysOnTopOnStartup;

    TCHAR* m_pIniGlobalSectionBuffer;
    TCHAR* m_pIniUserSectionBuffer;
    IniHashMap* m_pGlobalKeys;
    IniHashMap* m_pUserKeys;

    CString GetINITemplateName();
    void MakeSureUserINIExists();
    void ReadWriteableINISettings();

    LPCTSTR ReadUserIniString(LPCTSTR key);
    LPCTSTR ReadGlobalIniString(LPCTSTR key);
    LPCTSTR ReadIniString(LPCTSTR key, LPCTSTR fileName, IniHashMap*& keyMap, TCHAR*& pBuffer);
    void ReadIniFile(LPCTSTR fileName, IniHashMap* keyMap, TCHAR*& pBuffer);

    CString GetString(LPCTSTR sKey, LPCTSTR sDefault);
    int GetInt(LPCTSTR sKey, int nDefault, int nMin, int nMax);
    double GetDouble(LPCTSTR sKey, double dDefault, double dMin, double dMax);
    bool GetBool(LPCTSTR sKey, bool bDefault);
    CRect GetRect(LPCTSTR sKey, const CRect& defaultRect);
    CSize GetSize(LPCTSTR sKey, const CSize& defaultSize);
    COLORREF GetColor(LPCTSTR sKey, COLORREF defaultColor);
    Helpers::EAutoZoomMode GetAutoZoomMode(LPCTSTR sKey, Helpers::EAutoZoomMode defaultZoomMode);
    LPCTSTR GetAutoZoomModeString(Helpers::EAutoZoomMode autoZoomMode);
    void WriteString(LPCTSTR sKey, LPCTSTR sString);
    void WriteDouble(LPCTSTR sKey, double dValue);
    void WriteBool(LPCTSTR sKey, bool bValue);
    void WriteInt(LPCTSTR sKey, int nValue);

    CSettingsProvider(void);

/*GF*/    int m_nBookModePageHeight;
/*GF*/    bool m_bBookModeLaunchFullscreen;
/*GF*/    bool m_bSmoothPanning;
/*GF*/    bool m_bSmartPanningKeys;
/*GF*/    bool m_bTitleBarUseFileIcon;
/*GF*/    int m_nCacheRange;
/*GF*/    Helpers::ETransparencyMode m_eTransparencyMode;
/*GF*/    int NumConcurrentThreads(void);
};


#endif // SETTINGSPROVIDER_H

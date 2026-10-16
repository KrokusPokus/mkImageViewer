#ifndef SETTINGSPROVIDER_H
#define SETTINGSPROVIDER_H

#include "Helpers.h"
//#include "FileList.h"
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
    bool AllowSmoothPanning() { return m_bSmoothPanning; }
    bool AutoContrastCorrection() { return m_bAutoContrastCorrection; }
    bool AutoFullScreen() { return m_bAutoFullScreen; }
    bool AutoRotateEXIF() { return m_bAutoRotateEXIF; }
    bool BookModeLaunchFullscreen() { return m_bBookModeLaunchFullscreen; }
    bool DefaultMaximized() { return m_bDefaultMaximized; }
    bool DefaultWndToImage() { return m_bDefaultWndToImage; }
    bool ExchangeXButtons() { return m_bExchangeXButtons; }
    bool ExistsUserINI();
    bool ExplicitWindowRect() { return m_bExplicitWindowRect; }
    bool HighQualityResampling() { return m_bHQRS; }
    bool IsSortedAscending() { return m_bIsSortedAscending; }
    bool KeepParams() { return m_bKeepParams; }
    bool NavigateWithMouseWheel() { return m_bNavigateMouseWheel; }
    bool ReloadWhenDisplayedImageChanged() { return m_bReloadWhenDisplayedImageChanged; }
    bool ShowFullPathInTitle() { return m_bShowFullPathInTitle; }
    bool ShowFullScreen() { return m_bShowFullScreen; }
    bool SingleFullScreenInstance() { return m_bSingleFullScreenInstance; }
    bool SmartPanningKeys() { return m_bSmartPanningKeys; }
    bool StickyWindowSize() { return m_bStickyWindowSize; }
    bool StoreToEXEPath() { return m_bStoreToEXEPath; }
    bool TitleBarUseFileIcon() { return m_bTitleBarUseFileIcon; }
    bool UseEmbeddedColorProfiles() { return m_bUseEmbeddedColorProfiles; }
    bool WindowAlwaysOnTopOnStartup() { return m_bWindowAlwaysOnTopOnStartup; }
    bool WindowBorderlessOnStartup() { return m_bWindowBorderlessOnStartup; }
    bool WrapAroundFolder() { return m_bWrapAroundFolder && m_eNavigation == Helpers::NM_LoopDirectory; }
    COLORREF ColorBackground() { return m_colorBackground; }
    COLORREF ColorGUI() { return m_colorGUI; }
    COLORREF ColorTransparency() { return m_colorTransparency; }
    CRect DefaultWindowRect() { return m_defaultWindowRect; }
    CRect StickyWindowRect() { return m_stickyWindowRect; }
    CSize MinimalWindowSize() { return m_minimalWindowSize; }
    double AutoBrightnessAmount() { return m_dAutoBrightnessAmount; }
    double AutoContrastAmount() { return m_dAutoContrastAmount; }
    double Contrast() { return m_dContrast; }
    double Gamma() { return m_dGamma; }
    double MouseWheelZoomSpeed() { return m_dMouseWheelZoomSpeed; }
    double Saturation() { return m_dSaturation; }
    double ZoomPauseFactor() { return m_zoomPauseFactor; }  // while internally this is represented in doubles, using a whole number percent simplifies it for the user... configuring doubles is not user friendly at all
    EFilterType DownsamplingFilter() { return m_eDownsamplingFilter; }
    float* ColorCorrectionAmounts(); // can't be declared inline due to compiler bug...sad but true
    Helpers::CPUType AlgorithmImplementation() { return m_eCPUAlgorithm; }
    Helpers::EAutoZoomMode AutoZoomMode() { return m_eAutoZoomMode; }
    Helpers::EAutoZoomMode AutoZoomModeFullscreen() { return m_eAutoZoomModeFullscreen; }
    Helpers::EDeleteConfirmation DeleteConfirmation() { return m_eDeleteConfirmation; }
    Helpers::EIniEditor IniEditor() { return m_eIniEditor; }
    Helpers::ENavigationMode Navigation() { return m_eNavigation; }
    Helpers::ESingleInstanceMode SingleInstanceMode() { return m_eSingleInstanceMode; }
    Helpers::ESorting Sorting() { return m_eSorting; }
    Helpers::ETransitionEffect SlideShowTransitionEffect() { return m_eSlideShowTransitionEffect; }
    Helpers::ETransparencyMode TransparencyMode() {return m_eTransparencyMode; }
    int BookModePageHeight() { return m_nBookModePageHeight; }
    int CacheRange() { return m_nCacheRange; }
    int DisplayMonitor() { return m_nDisplayMonitor; }
    int MaxSlideShowFileListSize() { return m_nMaxSlideShowFileListSize; }
    int NumberOfCoresToUse() { return m_nNumCores; }
    int SlideShowEffectTimeMs() { return m_nSlideShowEffectTimeMs; }
    LPCTSTR CustomIniEditor() { return m_sIniEditor; }
    LPCTSTR DefaultGUIFont() { return m_defaultGUIFont; }
    LPCTSTR GetEXEPath() { return m_sEXEPath; }
    LPCTSTR GetGlobalINIFileName() { return m_sIniNameGlobal; }    // Get the file name with path of the global INI file (in EXE path)
    LPCTSTR GetINIFileTitle() { return m_sIniFileTitle; }          // Get the file name (without path) of the user INI file
    LPCTSTR GetUserINIFileName() { return m_sIniNameUser; }        // Get the file name with path of the user INI file (in AppData path)
    LPCTSTR Language() { return m_sLanguage; }
    LPCTSTR ZoomFactorFont() { return m_zoomFactorFont; }
    void CopyUserINIFromTemplate();
    void SaveCustomSettings();
    void SaveStickyWindowRect(CRect rect);
    void SetBookModePageHeight(int nBookModePageHeight) { m_nBookModePageHeight = nBookModePageHeight; }
    void SetMonitorOverride(int nMonitor) { m_nDisplayMonitor = nMonitor; }
    void SetUseEmbeddedColorProfiles(int bUseEmbeddedColorProfiles) { m_bUseEmbeddedColorProfiles = bUseEmbeddedColorProfiles; }

    // This will only save a subset of settings to the INI file located in AppData\JPEGView\JPEGView.ini.
    // Note that this INI file has precedence over the INI file at the program directory
    void SaveSettings(const CImageProcessingParams& procParams,
        EProcessingFlags eProcFlags,
        Helpers::ENavigationMode eNavigationMode, Helpers::ESorting eFileSorting, bool isSortedAscending,
        Helpers::EAutoZoomMode eAutoZoomMode, Helpers::EAutoZoomMode eAutoZoomModeFullScreen,
        Helpers::ETransitionEffect eSlideShowTransitionEffect);

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
    bool m_bHQRS;
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
    int m_nMaxSlideShowFileListSize;
    Helpers::ETransitionEffect m_eSlideShowTransitionEffect;
    int m_nSlideShowEffectTimeMs;
    Helpers::ESingleInstanceMode m_eSingleInstanceMode;
    bool m_bSingleFullScreenInstance;
    bool m_bWrapAroundFolder;
    Helpers::EDeleteConfirmation m_eDeleteConfirmation;
    bool m_bExchangeXButtons;
    bool m_bAutoRotateEXIF;
    bool m_bUseEmbeddedColorProfiles;
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
    CString m_zoomFactorFont;
    bool m_bReloadWhenDisplayedImageChanged;
    CSize m_minimalWindowSize;
    Helpers::EIniEditor  m_eIniEditor;
    CString m_sIniEditor;
    int m_zoomPauseFactor;
    bool m_bWindowBorderlessOnStartup;
    bool m_bWindowAlwaysOnTopOnStartup;

    int m_nBookModePageHeight;
    bool m_bBookModeLaunchFullscreen;
    bool m_bSmoothPanning;
    bool m_bSmartPanningKeys;
    bool m_bTitleBarUseFileIcon;
    int m_nCacheRange;
    Helpers::ETransparencyMode m_eTransparencyMode;
    
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
};


#endif // SETTINGSPROVIDER_H

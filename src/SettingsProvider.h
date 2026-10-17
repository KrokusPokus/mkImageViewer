#ifndef SETTINGSPROVIDER_H
#define SETTINGSPROVIDER_H

#include "Helpers.h"
#include "ProcessParams.h"

#include <QString>
#include <QRect>
#include <QSize>
#include <QColor>
#include <QSettings>
#include <QDialog>
#include <memory>

// Interne Qt-Typen für Kompatibilität mit bestehenden Signaturen bereitstellen/konvertieren
// Falls CRect, CSize, COLORREF globale Typen/Klassen sind, nutzen wir deren Schnittstelle oder Casts.
#ifndef COLORREF
using COLORREF = unsigned long;
#endif

class QTabWidget;
class QWidget;

// Qt6 Einstellungen-Dialog
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Liest alle Werte aus SettingsProvider in die UI-Steuerelemente
    void loadFromSettings();

    // Speichert alle Änderungen aus der UI zurück in SettingsProvider und INI
    void saveToSettings();

private:
    QTabWidget* m_tabWidget;

    // UI-Controls (Member für den Zugriff beim Speichern/Laden)
    // Programm & System
    class QComboBox* m_comboLanguage;
    class QComboBox* m_comboDeleteConfirmation;
    class QComboBox* m_comboSingleInstance;
    class QComboBox* m_comboCpuType;
    class QSpinBox*  m_spinCpuCores;
    class QSpinBox*  m_spinCacheRange;
    class QComboBox* m_comboIniEditor;
    class QLineEdit* m_editCustomIniEditor;
    class QCheckBox* m_checkStoreToExe;

    // Fenster & Verhalten
    class QComboBox* m_comboShowFullScreen;
    class QCheckBox* m_checkSingleFullScreenInstance;
    class QComboBox* m_comboDefaultWindowRect;
    class QSpinBox*  m_spinMinWidth;
    class QSpinBox*  m_spinMinHeight;
    class QCheckBox* m_checkWindowBorderless;
    class QCheckBox* m_checkWindowAlwaysOnTop;
    class QCheckBox* m_checkShowFilePathInTitle;
    class QCheckBox* m_checkTitleBarUseFileIcon;
    class QSpinBox*  m_spinDisplayMonitor;

    // Erscheinungsbild & GUI
    class QLineEdit* m_editDefaultGuiFont;
    class QLineEdit* m_editZoomFactorFont;
    class QPushButton* m_btnBgColor;
    class QPushButton* m_btnGuiColor;
    class QPushButton* m_btnTransColor;
    class QComboBox* m_comboTransparencyMode;
    QColor m_colorBg;
    QColor m_colorGui;
    QColor m_colorTrans;

    // Navigation
    class QComboBox* m_comboFileDisplayOrder;
    class QCheckBox* m_checkFileSortAscending;
    class QComboBox* m_comboFolderNavigation;
    class QCheckBox* m_checkWrapAroundFolder;
    class QCheckBox* m_checkNavigateMouseWheel;
    class QDoubleSpinBox* m_spinMouseWheelZoomSpeed;
    class QCheckBox* m_checkExchangeXButtons;

    // Bildanzeige & Zoom
    class QCheckBox* m_checkReloadWhenImageChanged;
    class QCheckBox* m_checkHighQualityResampling;
    class QComboBox* m_comboDownSamplingFilter;
    class QCheckBox* m_checkAutoRotateExif;
    class QCheckBox* m_checkUseEmbeddedColorProfiles;
    class QComboBox* m_comboAutoZoomMode;
    class QComboBox* m_comboAutoZoomModeFullscreen;
    class QSpinBox*  m_spinZoomPausePercent;
    class QSpinBox*  m_spinBookModePageHeight;
    class QCheckBox* m_checkBookModeLaunchFullscreen;
    class QCheckBox* m_checkSmoothPanning;
    class QCheckBox* m_checkSmartPanningKeys;

    // Bildkorrektur
    class QDoubleSpinBox* m_spinContrast;
    class QDoubleSpinBox* m_spinGamma;
    class QDoubleSpinBox* m_spinSaturation;
    class QCheckBox* m_checkAutoContrastCorrection;
    class QDoubleSpinBox* m_spinAutoContrastAmount;
    class QDoubleSpinBox* m_spinAutoBrightnessAmount;
    class QLineEdit* m_editColorCorrection;

    // Diashow
    class QSpinBox*  m_spinMaxSlideShowFileListSize;
    class QComboBox* m_comboSlideShowTransitionEffect;
    class QSpinBox*  m_spinSlideShowEffectTimeMs;
    class QCheckBox* m_checkKeepParams;

    // Hilfsmethoden zum Erstellen der Tabs
    QWidget* createProgramTab();
    QWidget* createWindowTab();
    QWidget* createGuiTab();
    QWidget* createNavigationTab();
    QWidget* createDisplayTab();
    QWidget* createCorrectionTab();
    QWidget* createSlideshowTab();

    void pickColor(QColor& target, QPushButton* button);
};

// INI settings
class CSettingsProvider
{
public:
    static CSettingsProvider& This();

    bool ShowSettingsDialog(QWidget* parent = nullptr);

    bool AllowSmoothPanning() const { return m_bSmoothPanning; }
    bool AutoContrastCorrection() const { return m_bAutoContrastCorrection; }
    bool AutoFullScreen() const { return m_bAutoFullScreen; }
    bool AutoRotateEXIF() const { return m_bAutoRotateEXIF; }
    bool BookModeLaunchFullscreen() const { return m_bBookModeLaunchFullscreen; }
    bool DefaultMaximized() const { return m_bDefaultMaximized; }
    bool DefaultWndToImage() const { return m_bDefaultWndToImage; }
    bool ExchangeXButtons() const { return m_bExchangeXButtons; }
    bool ExistsUserINI() const;
    bool ExplicitWindowRect() const { return m_bExplicitWindowRect; }
    bool HighQualityResampling() const { return m_bHQRS; }
    bool IsSortedAscending() const { return m_bIsSortedAscending; }
    bool KeepParams() const { return m_bKeepParams; }
    bool NavigateWithMouseWheel() const { return m_bNavigateMouseWheel; }
    bool ReloadWhenDisplayedImageChanged() const { return m_bReloadWhenDisplayedImageChanged; }
    bool ShowFullPathInTitle() const { return m_bShowFullPathInTitle; }
    bool ShowFullScreen() const { return m_bShowFullScreen; }
    bool SingleFullScreenInstance() const { return m_bSingleFullScreenInstance; }
    bool SmartPanningKeys() const { return m_bSmartPanningKeys; }
    bool StickyWindowSize() const { return m_bStickyWindowSize; }
    bool StoreToEXEPath() const { return m_bStoreToEXEPath; }
    bool TitleBarUseFileIcon() const { return m_bTitleBarUseFileIcon; }
    bool UseEmbeddedColorProfiles() const { return m_bUseEmbeddedColorProfiles; }
    bool WindowAlwaysOnTopOnStartup() const { return m_bWindowAlwaysOnTopOnStartup; }
    bool WindowBorderlessOnStartup() const { return m_bWindowBorderlessOnStartup; }
    bool WrapAroundFolder() const { return m_bWrapAroundFolder && m_eNavigation == Helpers::NM_LoopDirectory; }

    // Ursprüngliche Rückgabetypen für vollständige Drop-in Compatibility
    COLORREF ColorBackground() const { return ToColorref(m_colorBackground); }
    COLORREF ColorGUI() const { return ToColorref(m_colorGUI); }
    COLORREF ColorTransparency() const { return ToColorref(m_colorTransparency); }
    CRect DefaultWindowRect() const { return ToCRect(m_defaultWindowRect); }
    CRect StickyWindowRect() const { return ToCRect(m_stickyWindowRect); }
    CSize MinimalWindowSize() const { return ToCSize(m_minimalWindowSize); }

    double AutoBrightnessAmount() const { return m_dAutoBrightnessAmount; }
    double AutoContrastAmount() const { return m_dAutoContrastAmount; }
    double Contrast() const { return m_dContrast; }
    double Gamma() const { return m_dGamma; }
    double MouseWheelZoomSpeed() const { return m_dMouseWheelZoomSpeed; }
    double Saturation() const { return m_dSaturation; }
    double ZoomPauseFactor() const { return m_zoomPauseFactor; }  // while internally this is represented in doubles, using a whole number percent simplifies it for the user... configuring doubles is not user friendly at all

    EFilterType DownsamplingFilter() const { return m_eDownsamplingFilter; }
    const float* ColorCorrectionAmounts(); // can't be declared inline due to compiler bug...sad but true
    Helpers::CPUType AlgorithmImplementation() const { return m_eCPUAlgorithm; }
    Helpers::EAutoZoomMode AutoZoomMode() const { return m_eAutoZoomMode; }
    Helpers::EAutoZoomMode AutoZoomModeFullscreen() const { return m_eAutoZoomModeFullscreen; }
    Helpers::EDeleteConfirmation DeleteConfirmation() const { return m_eDeleteConfirmation; }
    Helpers::EIniEditor IniEditor() const { return m_eIniEditor; }
    Helpers::ENavigationMode Navigation() const { return m_eNavigation; }
    Helpers::ESingleInstanceMode SingleInstanceMode() const { return m_eSingleInstanceMode; }
    Helpers::ESorting Sorting() const { return m_eSorting; }
    Helpers::ETransitionEffect SlideShowTransitionEffect() const { return m_eSlideShowTransitionEffect; }
    Helpers::ETransparencyMode TransparencyMode() const { return m_eTransparencyMode; }

    int BookModePageHeight() const { return m_nBookModePageHeight; }
    int CacheRange() const { return m_nCacheRange; }
    int DisplayMonitor() const { return m_nDisplayMonitor; }
    int MaxSlideShowFileListSize() const { return m_nMaxSlideShowFileListSize; }
    int NumberOfCoresToUse() const { return m_nNumCores; }
    int SlideShowEffectTimeMs() const { return m_nSlideShowEffectTimeMs; }

    // LPCTSTR Rückgabe über internen QString mittels utf16()
    LPCTSTR CustomIniEditor() const { return reinterpret_cast<LPCTSTR>(m_sIniEditor.utf16()); }
    LPCTSTR DefaultGUIFont() const { return reinterpret_cast<LPCTSTR>(m_defaultGUIFont.utf16()); }
    LPCTSTR GetEXEPath() const { return reinterpret_cast<LPCTSTR>(m_sEXEPath.utf16()); }
    LPCTSTR GetGlobalINIFileName() const { return reinterpret_cast<LPCTSTR>(m_sIniNameGlobal.utf16()); }    // Get the file name with path of the global INI file (in EXE path)
    LPCTSTR GetUserINIFileName() const { return reinterpret_cast<LPCTSTR>(m_sIniNameUser.utf16()); }        // Get the file name with path of the user INI file (in AppData path)
    LPCTSTR Language() const { return reinterpret_cast<LPCTSTR>(m_sLanguage.utf16()); }
    LPCTSTR ZoomFactorFont() const { return reinterpret_cast<LPCTSTR>(m_zoomFactorFont.utf16()); }

    void CopyUserINIFromTemplate();
    void SaveCustomSettings();
    void SaveStickyWindowRect(CRect rect);
    void SetBookModePageHeight(int nBookModePageHeight) { m_nBookModePageHeight = nBookModePageHeight; }
    void SetUseEmbeddedColorProfiles(int bUseEmbeddedColorProfiles) { m_bUseEmbeddedColorProfiles = (bUseEmbeddedColorProfiles != 0); }

    // This will only save a subset of settings to the INI file located in AppData\JPEGView\JPEGView.ini.
    // Note that this INI file has precedence over the INI file at the program directory
    void SaveSettings(const CImageProcessingParams& procParams,
                      EProcessingFlags eProcFlags,
                      Helpers::ENavigationMode eNavigationMode, Helpers::ESorting eFileSorting, bool isSortedAscending,
                      Helpers::EAutoZoomMode eAutoZoomMode, Helpers::EAutoZoomMode eAutoZoomModeFullScreen,
                      Helpers::ETransitionEffect eSlideShowTransitionEffect);

    // Lädt alle INI-Einstellungen erneut ein
    void ReloadAllSettings();

    friend class SettingsDialog;

private:
    static std::unique_ptr<CSettingsProvider> sm_instance;

    // Hilfskonvertierungen für ursprüngliche Typen (MFC / Win32 Kompatibilität)
    static COLORREF ToColorref(const QColor& c) { return RGB(c.red(), c.green(), c.blue()); }
    static CRect ToCRect(const QRect& r) { return CRect(r.left(), r.top(), r.right(), r.bottom()); }
    static CSize ToCSize(const QSize& s) { return CSize(s.width(), s.height()); }

    QString m_sEXEPath;
    QString m_sIniNameGlobal;
    QString m_sIniNameUser;
    QString m_sIniFileTitle;
    bool m_bUserINIExists{false};
    bool m_bStoreToEXEPath{false};

    double m_dContrast{0.0};
    double m_dGamma{1.0};
    double m_dSaturation{1.0};
    bool m_bHQRS{true};
    bool m_bShowFullPathInTitle{false};
    bool m_bShowFullScreen{true};
    bool m_bAutoFullScreen{false};
    bool m_bKeepParams{false};
    QString m_sLanguage;
    Helpers::CPUType m_eCPUAlgorithm;
    int m_nNumCores{1};
    int m_nNumCoresIni{0};
    EFilterType m_eDownsamplingFilter;
    Helpers::ESorting m_eSorting;
    bool m_bIsSortedAscending{true};
    Helpers::ENavigationMode m_eNavigation;
    bool m_bNavigateMouseWheel{false};
    double m_dMouseWheelZoomSpeed{1.0};
    Helpers::EAutoZoomMode m_eAutoZoomMode;
    Helpers::EAutoZoomMode m_eAutoZoomModeFullscreen;
    int m_nDisplayMonitor{-1};
    bool m_bAutoContrastCorrection{false};
    double m_dAutoContrastAmount{0.5};
    float m_fColorCorrections[6];
    double m_dAutoBrightnessAmount{0.2};
    int m_nMaxSlideShowFileListSize{200};
    Helpers::ETransitionEffect m_eSlideShowTransitionEffect;
    int m_nSlideShowEffectTimeMs{200};
    Helpers::ESingleInstanceMode m_eSingleInstanceMode;
    bool m_bSingleFullScreenInstance{true};
    bool m_bWrapAroundFolder{true};
    Helpers::EDeleteConfirmation m_eDeleteConfirmation;
    bool m_bExchangeXButtons{true};
    bool m_bAutoRotateEXIF{true};
    bool m_bUseEmbeddedColorProfiles{false};
    QRect m_defaultWindowRect;
    QRect m_stickyWindowRect;
    bool m_bDefaultMaximized{false};
    bool m_bDefaultWndToImage{false};
    bool m_bStickyWindowSize{false};
    bool m_bExplicitWindowRect{false};
    QColor m_colorBackground;
    QColor m_colorGUI;
    QColor m_colorTransparency;
    QString m_defaultGUIFont;
    QString m_zoomFactorFont;
    bool m_bReloadWhenDisplayedImageChanged{true};
    QSize m_minimalWindowSize;
    Helpers::EIniEditor m_eIniEditor;
    QString m_sIniEditor;
    double m_zoomPauseFactor{1.0};
    bool m_bWindowBorderlessOnStartup{false};
    bool m_bWindowAlwaysOnTopOnStartup{false};

    int m_nBookModePageHeight{122};
    bool m_bBookModeLaunchFullscreen{true};
    bool m_bSmoothPanning{true};
    bool m_bSmartPanningKeys{true};
    bool m_bTitleBarUseFileIcon{true};
    int m_nCacheRange{2};
    Helpers::ETransparencyMode m_eTransparencyMode;

    QString GetINITemplateName() const;
    void MakeSureUserINIExists();
    void ReadWriteableINISettings();

    QString GetString(const QString& sKey, const QString& sDefault) const;
    int GetInt(const QString& sKey, int nDefault, int nMin, int nMax) const;
    double GetDouble(const QString& sKey, double dDefault, double dMin, double dMax) const;
    bool GetBool(const QString& sKey, bool bDefault) const;
    QRect GetRect(const QString& sKey, const QRect& defaultRect) const;
    QSize GetSize(const QString& sKey, const QSize& defaultSize) const;
    QColor GetColor(const QString& sKey, const QColor& defaultColor) const;
    Helpers::EAutoZoomMode GetAutoZoomMode(const QString& sKey, Helpers::EAutoZoomMode defaultZoomMode) const;

    void WriteString(const QString& sKey, const QString& sString);
    void WriteDouble(const QString& sKey, double dValue);
    void WriteBool(const QString& sKey, bool bValue);
    void WriteInt(const QString& sKey, int nValue);

    CSettingsProvider();
};

#endif // SETTINGSPROVIDER_H

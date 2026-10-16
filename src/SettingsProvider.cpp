#include "SettingsProvider.h"

//#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QStringTokenizer>

#include <thread>    // Needed for std::thread::hardware_concurrency()
#include <algorithm>

static const char* DEFAULT_INI_FILE_NAME = "JPEGView.ini";
static const char* SECTION_NAME = "JPEGView";

static float COLOR_CORR_DEFAULT_FACTORS[6] = {0.2f, 0.1f, 0.3f, 0.3f, 0.3f, 0.15f};

std::unique_ptr<CSettingsProvider> CSettingsProvider::sm_instance = nullptr;

CSettingsProvider& CSettingsProvider::This() {
    if (!sm_instance) {
        sm_instance.reset(new CSettingsProvider());
    }
    return *sm_instance;
}

float* CSettingsProvider::ColorCorrectionAmounts() { 
    return (m_fColorCorrections[0] < 0) ? COLOR_CORR_DEFAULT_FACTORS : m_fColorCorrections;
}

CSettingsProvider::CSettingsProvider() {
    // parse command line to find the process startup path
    // win32 variant
    wchar_t pathBuffer[MAX_PATH];
    ::GetModuleFileNameW(NULL, pathBuffer, MAX_PATH);
    QString exeFilePath = QString::fromWCharArray(pathBuffer);

    QFileInfo exeInfo(exeFilePath);
    m_sEXEPath = exeInfo.absolutePath();
    if (!m_sEXEPath.endsWith('/') && !m_sEXEPath.endsWith('\\')) {
        m_sEXEPath += '/';
    }
    /*
    // Qt variant
    m_sEXEPath = QCoreApplication::applicationDirPath();
    if (!m_sEXEPath.endsWith('/') && !m_sEXEPath.endsWith('\\')) {
        m_sEXEPath += '/';
    }
    */

    // Check if a custom INI file name is specified on the command line
    m_sIniFileTitle = DEFAULT_INI_FILE_NAME;

    // Global INI file (at EXE location)
    m_sIniNameGlobal = m_sEXEPath + m_sIniFileTitle; // use current dir as startup path

    // Read if the user INI file shall be used
    m_sIniNameUser = m_sIniNameGlobal;
    m_bStoreToEXEPath = GetBool("StoreToEXEPath", false);

    if (!m_bStoreToEXEPath) {
        // User INI file
        QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!appDataPath.endsWith('/') && !appDataPath.endsWith('\\')) {
            appDataPath += '/';
        }
        m_sIniNameUser = appDataPath + m_sIniFileTitle;
        m_bUserINIExists = QFileInfo::exists(m_sIniNameUser);
    } else {
        Helpers::SetJPEGViewAppDataPath(reinterpret_cast<LPCTSTR>(m_sEXEPath.utf16()));
    }

    /*
    qDebug() << "m_sEXEPath:" << m_sEXEPath;
    qDebug() << "m_sIniFileTitle:" << m_sIniFileTitle;
    qDebug() << "m_sIniNameGlobal:" << m_sIniNameGlobal;
    qDebug() << "m_sIniNameUser:" << m_sIniNameUser;
    qDebug() << "m_bStoreToEXEPath:" << m_bStoreToEXEPath;
    qDebug() << "m_bUserINIExists:" << m_bUserINIExists;
    */

    // Read settings that can be written with SaveSettings()
    ReadWriteableINISettings();

    m_bAutoFullScreen = GetString("ShowFullScreen", "").compare("auto", Qt::CaseInsensitive) == 0;
    m_bShowFullScreen = m_bAutoFullScreen ? true : GetBool("ShowFullScreen", true);
    m_bShowFullPathInTitle = GetBool("ShowFilePathInTitle", false);

    QString sCPU = GetString("CPUType", "AutoDetect");
    if (sCPU.compare("Generic", Qt::CaseInsensitive) == 0) {
        m_eCPUAlgorithm = Helpers::CPU_Generic;
    } else if (sCPU.compare("MMX", Qt::CaseInsensitive) == 0) {
        m_eCPUAlgorithm = Helpers::CPU_MMX;
    } else if (sCPU.compare("SSE", Qt::CaseInsensitive) == 0) {
        m_eCPUAlgorithm = Helpers::CPU_SSE;
    } else if (sCPU.compare("AVX2", Qt::CaseInsensitive) == 0) {
        m_eCPUAlgorithm = Helpers::CPU_AVX2;
    } else {
        m_eCPUAlgorithm = Helpers::ProbeCPU();
    }

    m_nNumCores = GetInt("CPUCoresUsed", 0, 0, 128);
    if (m_nNumCores == 0) {
        m_nNumCores = static_cast<int>(std::thread::hardware_concurrency());
        m_nNumCores = std::clamp(m_nNumCores, 1, 8);
        // Limiting to 8 cores, since benchmarks suggest performance improvements start to flatline around that point.
        // Performance can even become worse: 16 threads on 16 thread CPU performing worse than running just 14 threads. Or 8 threads, for that matter.
        // People can still try to use more cores by specifying a non-zero number for "CPUCoresUsed",
        // but I advise to do some benchmarking in this case to make sure you aren't actually shooting yourself in the foot.        
    }

    QString sDownSampling = GetString("DownSamplingFilter", "Catrom");
    if (sDownSampling.compare("None", Qt::CaseInsensitive) == 0) {
        m_eDownsamplingFilter = Filter_Downsampling_None;
    } else if (sDownSampling.compare("Hermite", Qt::CaseInsensitive) == 0) {
        m_eDownsamplingFilter = Filter_Downsampling_Hermite;
    } else if (sDownSampling.compare("Mitchell", Qt::CaseInsensitive) == 0) {
        m_eDownsamplingFilter = Filter_Downsampling_Mitchell;
    } else if (sDownSampling.compare("Catrom", Qt::CaseInsensitive) == 0) {
        m_eDownsamplingFilter = Filter_Downsampling_Catrom;
    } else if (sDownSampling.compare("Lanczos2", Qt::CaseInsensitive) == 0) {
        m_eDownsamplingFilter = Filter_Downsampling_Lanczos2;
    } else {
        m_eDownsamplingFilter = Filter_Downsampling_Catrom;
    }

    QString sSingleInstanceMode = GetString("SingleInstance", "PerFolder");
    if (sSingleInstanceMode.compare("Never", Qt::CaseInsensitive) == 0) {
        m_eSingleInstanceMode = Helpers::SI_Never;
    } else if (sSingleInstanceMode.compare("PerFolder", Qt::CaseInsensitive) == 0) {
        m_eSingleInstanceMode = Helpers::SI_PerFolder;
    } else if (sSingleInstanceMode.compare("Always", Qt::CaseInsensitive) == 0) {
        m_eSingleInstanceMode = Helpers::SI_Always;
    } else {
        m_eSingleInstanceMode = Helpers::SI_PerFolder;
    }

    QString sTransparencyMode = GetString("TransparencyMode", "Checkerboard");
    if (sTransparencyMode.compare("Blend", Qt::CaseInsensitive) == 0) {
        m_eTransparencyMode = Helpers::TP_BLEND;
    } else if (sTransparencyMode.compare("Checkerboard", Qt::CaseInsensitive) == 0) {
        m_eTransparencyMode = Helpers::TP_CHECKERBOARD;
    } else if (sTransparencyMode.compare("BlendInverse", Qt::CaseInsensitive) == 0) {
        m_eTransparencyMode = Helpers::TP_BLEND_INVERSE;
    } else {
        m_eTransparencyMode = Helpers::TP_CHECKERBOARD;
    }

    m_bNavigateMouseWheel = GetBool("NavigateWithMouseWheel", false);
    m_dMouseWheelZoomSpeed = GetDouble("MouseWheelZoomSpeed", 1.0, 0.1, 10.0);

    QString sDeleteConfirmation = GetString("DeleteConfirmation", "OnlyWhenNoRecycleBin");
    if (sDeleteConfirmation.compare("OnlyWhenNoRecycleBin", Qt::CaseInsensitive) == 0) {
        m_eDeleteConfirmation = Helpers::DC_OnlyWhenNoRecycleBin;
    } else if (sDeleteConfirmation.compare("Never", Qt::CaseInsensitive) == 0) {
        m_eDeleteConfirmation = Helpers::DC_Never;
    } else {
        m_eDeleteConfirmation = Helpers::DC_Always;
    }

    m_nMaxSlideShowFileListSize = GetInt("MaxSlideShowFileListSizeKB", 200, 100, 10000);
    m_nSlideShowEffectTimeMs = GetInt("SlideShowEffectTime", 200, 100, 5000);
    m_bSingleFullScreenInstance = GetBool("SingleFullScreenInstance", true);
    m_bWrapAroundFolder = GetBool("WrapAroundFolder", true);
    m_bWindowBorderlessOnStartup = GetBool("WindowBorderlessOnStartup", false);
    m_bWindowAlwaysOnTopOnStartup = GetBool("WindowAlwaysOnTopOnStartup", false);
    m_zoomPauseFactor = GetInt("ZoomPausePercent", 100, 0, 6553500) / 100.0;  // can't have a % larger than the MAX_IMAGE_DIMENSION %, and convert to a scale factor (double/double division) only once
    m_bExchangeXButtons = GetBool("ExchangeXButtons", true);
    m_bAutoRotateEXIF = GetBool("AutoRotateEXIF", true);
    m_bUseEmbeddedColorProfiles = GetBool("UseEmbeddedColorProfiles", false);
    m_nDisplayMonitor = GetInt("DisplayMonitor", -1, -1, 16);
    m_dAutoContrastAmount = GetDouble("AutoContrastCorrectionAmount", 0.5, 0.0, 1.0);
    m_dAutoBrightnessAmount = GetDouble("AutoBrightnessCorrectionAmount", 0.2, 0.0, 1.0);

    m_defaultWindowRect = GetRect("DefaultWindowRect", QRect(0, 0, 0, 0));
    m_stickyWindowRect = GetRect("StickyWindowRect", QRect(0, 0, 0, 0));
    m_bDefaultMaximized = false;
    m_bDefaultWndToImage = false;
    m_bStickyWindowSize = false;
    m_bExplicitWindowRect = false;

    if (m_defaultWindowRect.isEmpty()) {
        QString sAuto = GetString("DefaultWindowRect", "");
        if (sAuto.compare("max", Qt::CaseInsensitive) == 0) {
            m_bDefaultMaximized = true;
        } else if (sAuto.compare("image", Qt::CaseInsensitive) == 0) {
            m_bDefaultWndToImage = true;
        } else if (sAuto.compare("sticky", Qt::CaseInsensitive) == 0) {
            m_bStickyWindowSize = true;
            m_bExplicitWindowRect = !m_stickyWindowRect.isEmpty();
        }
    } else {
        m_bExplicitWindowRect = true;
    }

    m_colorBackground = GetColor("BackgroundColor", QColor(0, 0, 0));
    m_colorGUI = GetColor("GUIColor", QColor(243, 242, 231));
    m_colorTransparency = GetColor("TransparencyColor", m_colorBackground);

    m_defaultGUIFont = GetString("DefaultGUIFont", "Default");
    m_zoomFactorFont = GetString("ZoomFactorFont", "Default");

    m_bReloadWhenDisplayedImageChanged = GetBool("ReloadWhenDisplayedImageChanged", true);

    m_sIniEditor = GetString("IniEditor", "notepad");
    if (m_sIniEditor.compare("notepad", Qt::CaseInsensitive) == 0) {
        m_eIniEditor = Helpers::INI_Notepad;
    } else if (m_sIniEditor.compare("system", Qt::CaseInsensitive) == 0) {
        m_eIniEditor = Helpers::INI_System;
    } else {
        m_eIniEditor = Helpers::INI_Custom;
    }

    m_minimalWindowSize = GetSize("MinimalWindowSize", QSize(320, 240));

    m_sLanguage = GetString("Language", "auto");

    // Pure Qt-basiertes Parsing für ColorCorrections
    QString sColorCorrections = GetString("ColorCorrection", "");
    std::fill(std::begin(m_fColorCorrections), std::end(m_fColorCorrections), -1.0f);
    if (!sColorCorrections.isEmpty()) {
        // Erwartetes Format: "R: 0.2 G: 0.1 B: 0.3 C: 0.3 M: 0.3 Y: 0.15"
        const auto parts = sColorCorrections.split(' ', Qt::SkipEmptyParts);
        if (parts.size() == 12) {
            bool ok = true;
            float values[6];
            for (int i = 0; i < 6; ++i) {
                values[i] = parts[i * 2 + 1].toFloat(&ok);
                if (!ok) break;
            }
            if (ok) {
                for (int i = 0; i < 6; ++i) {
                    m_fColorCorrections[i] = std::clamp(values[i], 0.0f, 1.0f);
                }
            }
        }
    }

// -------------------------------------------------------------------------------------------
// [GF] Custom non-writable ini settings of this mod

    m_bBookModeLaunchFullscreen = GetBool("BookModeLaunchFullscreen", true);
    m_bSmoothPanning = GetBool("SmoothPanning", true);
    m_bSmartPanningKeys = GetBool("SmartPanningKeys", true);
    m_bTitleBarUseFileIcon = GetBool("TitleBarUseFileIcon", true);

    // Distance in file list for which decoded images will be kept in cache to speed up access.
    // The default of 2 means: Two pages forward + two pages backward.
    m_nCacheRange = GetInt("CacheRange", 2, 0, 8);
}

void CSettingsProvider::ReadWriteableINISettings() {
    m_dContrast = GetDouble("Contrast", 0.0, -0.5, 0.5);
    m_dGamma = GetDouble("Gamma", 1.0, 0.1, 10.0);
    m_dSaturation = GetDouble("Saturation", 1.0, 0.0, 2.0);
    m_bAutoContrastCorrection = GetBool("AutoContrastCorrection", false);

    QString sNavigation = GetString("FolderNavigation", "LoopFolder");
    if (sNavigation.compare("LoopSameFolderLevel", Qt::CaseInsensitive) == 0) {
        m_eNavigation = Helpers::NM_LoopSameDirectoryLevel;
    } else if (sNavigation.compare("LoopSubFolders", Qt::CaseInsensitive) == 0) {
        m_eNavigation = Helpers::NM_LoopSubDirectories;
    } else {
        m_eNavigation = Helpers::NM_LoopDirectory;
    }

    QString sSorting = GetString("FileDisplayOrder", "LastModDate");
    if (sSorting.compare("CreationDate", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_CreationTime;
    } else if (sSorting.compare("FileName", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_FileName;
    } else if (sSorting.compare("Random", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_Random;
    } else if (sSorting.compare("FileSize", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_FileSize;
    } else {
        m_eSorting = Helpers::FS_LastModTime;
    }

    m_bIsSortedAscending = GetBool("FileSortAscending", true);
    m_eAutoZoomMode = GetAutoZoomMode("AutoZoomMode", Helpers::ZM_FitToScreenNoZoom);
    m_eAutoZoomModeFullscreen = GetAutoZoomMode("AutoZoomModeFullscreen", m_eAutoZoomMode);

    m_bHQRS = GetBool("HighQualityResampling", true);
    m_bKeepParams = GetBool("KeepParameters", false);
    m_eSlideShowTransitionEffect = ConvertTransitionEffectFromString(GetString("SlideShowTransitionEffect", ""));

// -------------------------------------------------------------------------------------------
// [GF] Custom writable ini settings of this mod

    m_nBookModePageHeight = GetInt("BookModePageHeight", 122, 100, 1000);

// -------------------------------------------------------------------------------------------
}

void CSettingsProvider::SaveCustomSettings() {
    MakeSureUserINIExists();
    WriteInt("BookModePageHeight", m_nBookModePageHeight);
    m_bUserINIExists = true;
}

void CSettingsProvider::SaveSettings(const CImageProcessingParams& procParams, 
                                     EProcessingFlags eProcFlags,
                                     Helpers::ENavigationMode eNavigationMode, 
                                     Helpers::ESorting eFileSorting, 
                                     bool isSortedAscending,
                                     Helpers::EAutoZoomMode eAutoZoomMode, 
                                     Helpers::EAutoZoomMode eAutoZoomModeFullScreen,
                                     Helpers::ETransitionEffect eSlideShowTransitionEffect) {
    MakeSureUserINIExists();

    WriteDouble("Contrast", procParams.Contrast);
    WriteDouble("Gamma", procParams.Gamma);
    WriteDouble("Saturation", procParams.Saturation);
    WriteBool("AutoContrastCorrection", GetProcessingFlag(eProcFlags, PFLAG_AutoContrast));

    QString sNavigation = "LoopFolder";
    if (eNavigationMode == Helpers::NM_LoopSameDirectoryLevel) {
        sNavigation = "LoopSameFolderLevel";
    } else if (eNavigationMode == Helpers::NM_LoopSubDirectories) {
        sNavigation = "LoopSubFolders";
    }
    WriteString("FolderNavigation", sNavigation);

    QString sSorting = "FileName";
    if (eFileSorting == Helpers::FS_CreationTime) {
        sSorting = "CreationDate";
    } else if (eFileSorting == Helpers::FS_LastModTime) {
        sSorting = "LastModDate";
    } else if (eFileSorting == Helpers::FS_Random) {
        sSorting = "Random";
    } else if (eFileSorting == Helpers::FS_FileSize) {
        sSorting = "FileSize";
    }
    WriteString("FileDisplayOrder", sSorting);

    WriteBool("FileSortAscending", isSortedAscending);

    WriteString("AutoZoomMode", GetAutoZoomModeString(eAutoZoomMode));
    WriteString("AutoZoomModeFullscreen", GetAutoZoomModeString(eAutoZoomModeFullScreen));

    WriteBool("HighQualityResampling", GetProcessingFlag(eProcFlags, PFLAG_HighQualityResampling));
    WriteBool("KeepParameters", GetProcessingFlag(eProcFlags, PFLAG_KeepParams));

    WriteString("SlideShowTransitionEffect", ConvertTransitionEffectToString(eSlideShowTransitionEffect));

    m_bUserINIExists = true;
    ReadWriteableINISettings();
}

void CSettingsProvider::SaveStickyWindowRect(CRect rect) {
    if (ToCRect(m_stickyWindowRect) != rect && !rect.IsRectEmpty()) {
        QString sRect = QString("%1 %2 %3 %4")
                            .arg(rect.left)
                            .arg(rect.top)
                            .arg(rect.right)
                            .arg(rect.bottom);

        MakeSureUserINIExists();
        WriteString("StickyWindowRect", sRect);
        m_bUserINIExists = true;
    }
}

bool CSettingsProvider::ExistsUserINI() const {
    return QFileInfo::exists(m_sIniNameUser);
}

void CSettingsProvider::CopyUserINIFromTemplate() {
    if (m_bStoreToEXEPath) {
        return; // no user INI file
    }

    QFileInfo userFile(m_sIniNameUser);
    QDir().mkpath(userFile.absolutePath());
    QFile::copy(GetINITemplateName(), m_sIniNameUser);
}

void CSettingsProvider::MakeSureUserINIExists() {
    if (m_bStoreToEXEPath) {
        return; // no user INI file needed
    }

    // Create JPEGView appdata directory and copy INI file if it does not exist
    QFileInfo userFile(m_sIniNameUser);
    QDir().mkpath(userFile.absolutePath());
    if (!QFileInfo::exists(m_sIniNameUser)) {
        QFile::copy(GetINITemplateName(), m_sIniNameUser);
    }
}

QString CSettingsProvider::GetINITemplateName() const {
    return m_sEXEPath + DEFAULT_INI_FILE_NAME + ".tpl";
}

QString CSettingsProvider::GetString(const QString& sKey, const QString& sDefault) const {
    if (m_bUserINIExists && QFileInfo::exists(m_sIniNameUser)) {
        QSettings userSettings(m_sIniNameUser, QSettings::IniFormat);
        userSettings.beginGroup(SECTION_NAME);
        if (userSettings.contains(sKey)) {
            return userSettings.value(sKey).toString();
        }
        userSettings.endGroup();
    }

    if (QFileInfo::exists(m_sIniNameGlobal)) {
        QSettings globalSettings(m_sIniNameGlobal, QSettings::IniFormat);
        globalSettings.beginGroup(SECTION_NAME);
        if (globalSettings.contains(sKey)) {
            return globalSettings.value(sKey).toString();
        }
        globalSettings.endGroup();
    }

    return sDefault;
}

int CSettingsProvider::GetInt(const QString& sKey, int nDefault, int nMin, int nMax) const {
    QString s = GetString(sKey, "");
    if (s.isEmpty()) {
        return nDefault;
    }
    bool ok = false;
    int nValue = s.toInt(&ok);
    if (!ok) return nDefault;
    return std::clamp(nValue, nMin, nMax);
}

double CSettingsProvider::GetDouble(const QString& sKey, double dDefault, double dMin, double dMax) const {
    QString s = GetString(sKey, "");
    if (s.isEmpty()) {
        return dDefault;
    }
    bool ok = false;
    double dValue = s.toDouble(&ok);
    if (!ok) return dDefault;
    return std::clamp(dValue, dMin, dMax);
}

bool CSettingsProvider::GetBool(const QString& sKey, bool bDefault) const {
    QString s = GetString(sKey, "");
    if (s.isEmpty()) {
        return bDefault;
    }
    if (s.compare("true", Qt::CaseInsensitive) == 0) {
        return true;
    } else if (s.compare("false", Qt::CaseInsensitive) == 0) {
        return false;
    }
    return bDefault;
}

QRect CSettingsProvider::GetRect(const QString& sKey, const QRect& defaultRect) const {
    QString s = GetString(sKey, "");
    if (s.isEmpty()) {
        return defaultRect;
    }

    // Liest z.B. "302 93 1412 950" sauber ein
    const auto tokens = s.split(' ', Qt::SkipEmptyParts);
    if (tokens.size() == 4) {
        bool ok1, ok2, ok3, ok4;
        int left = tokens[0].toInt(&ok1);
        int top = tokens[1].toInt(&ok2);
        int right = tokens[2].toInt(&ok3);
        int bottom = tokens[3].toInt(&ok4);
        if (ok1 && ok2 && ok3 && ok4) {
            return QRect(QPoint(left, top), QPoint(right, bottom)).normalized();
        }
    }
    return defaultRect;
}

QSize CSettingsProvider::GetSize(const QString& sKey, const QSize& defaultSize) const {
    QString s = GetString(sKey, "");
    if (s.isEmpty()) {
        return defaultSize;
    }

    // Liest z.B. "320 240" sauber ein
    const auto tokens = s.split(' ', Qt::SkipEmptyParts);
    if (tokens.size() == 2) {
        bool ok1, ok2;
        int w = tokens[0].toInt(&ok1);
        int h = tokens[1].toInt(&ok2);
        if (ok1 && ok2) {
            return QSize(std::max(1, w), std::max(1, h));
        }
    }
    return defaultSize;
}

QColor CSettingsProvider::GetColor(const QString& sKey, const QColor& defaultColor) const {
    QString sColor = GetString(sKey, "");
    if (sColor.isEmpty()) {
        return defaultColor;
    }

    // Liest z.B. "0 0 0" oder "243 242 231" sauber ein
    const auto tokens = sColor.split(' ', Qt::SkipEmptyParts);
    if (tokens.size() == 3) {
        bool ok1, ok2, ok3;
        int r = tokens[0].toInt(&ok1);
        int g = tokens[1].toInt(&ok2);
        int b = tokens[2].toInt(&ok3);
        if (ok1 && ok2 && ok3) {
            return QColor(r, g, b);
        }
    }
    return defaultColor;
}

Helpers::EAutoZoomMode CSettingsProvider::GetAutoZoomMode(const QString& sKey, Helpers::EAutoZoomMode defaultZoomMode) const {
    QString sAutoZoomMode = GetString(sKey, "FitNoZoom");
    if (sAutoZoomMode.compare("Fit", Qt::CaseInsensitive) == 0) {
        return Helpers::ZM_FitToScreen;
    } else if (sAutoZoomMode.compare("Fill", Qt::CaseInsensitive) == 0) {
        return Helpers::ZM_FillScreen;
    } else if (sAutoZoomMode.compare("FillNoZoom", Qt::CaseInsensitive) == 0) {
        return Helpers::ZM_FillScreenNoZoom;
    }
    return defaultZoomMode;
}

QString CSettingsProvider::GetAutoZoomModeString(Helpers::EAutoZoomMode autoZoomMode) const {
    switch (autoZoomMode) {
        case Helpers::ZM_FillScreen:       return "Fill";
        case Helpers::ZM_FitToScreen:      return "Fit";
        case Helpers::ZM_FillScreenNoZoom: return "FillNoZoom";
        case Helpers::ZM_None:             return "None";
        case Helpers::ZM_BookMode:         return "BookMode";
        default:                           return "FitNoZoom";
    }
}

void CSettingsProvider::WriteString(const QString& sKey, const QString& sString) {
    QSettings settings(m_sIniNameUser, QSettings::IniFormat);
    settings.beginGroup(SECTION_NAME);
    if (settings.value(sKey).toString() != sString) {
        settings.setValue(sKey, sString);
    }
    settings.endGroup();
}

void CSettingsProvider::WriteDouble(const QString& sKey, double dValue) {
    WriteString(sKey, QString::number(dValue, 'f', 2));
}

void CSettingsProvider::WriteBool(const QString& sKey, bool bValue) {
    WriteString(sKey, bValue ? "true" : "false");
}

void CSettingsProvider::WriteInt(const QString& sKey, int nValue) {
    WriteString(sKey, QString::number(nValue));
}

Helpers::ETransitionEffect CSettingsProvider::ConvertTransitionEffectFromString(QStringView str) {
    if (str.isEmpty()) {
        return Helpers::TE_None;
    }

    // Statische Map für schnelle O(1)-Zuordnung (Case-Insensitive)
    static const QHash<QString, Helpers::ETransitionEffect> effectMap = {
        { "blend",    Helpers::TE_Blend },
        { "sliderl",  Helpers::TE_SlideRL },
        { "slidelr",  Helpers::TE_SlideLR },
        { "slidetb",  Helpers::TE_SlideTB },
        { "slidebt",  Helpers::TE_SlideBT },
        { "rollrl",   Helpers::TE_RollRL },
        { "rolllr",   Helpers::TE_RollLR },
        { "rolltb",   Helpers::TE_RollTB },
        { "rollbt",   Helpers::TE_RollBT },
        { "scrollrl", Helpers::TE_ScrollRL },
        { "scrolllr", Helpers::TE_ScrollLR },
        { "scrolltb", Helpers::TE_ScrollTB },
        { "scrollbt", Helpers::TE_ScrollBT }
    };

    return effectMap.value(str.toString().toLower(), Helpers::TE_None);
}

QString CSettingsProvider::ConvertTransitionEffectToString(Helpers::ETransitionEffect effect) {
    static const QMap<Helpers::ETransitionEffect, QString> stringMap = {
        { Helpers::TE_None,     "None" },
        { Helpers::TE_Blend,    "Blend" },
        { Helpers::TE_SlideRL,  "SlideRL" },
        { Helpers::TE_SlideLR,  "SlideLR" },
        { Helpers::TE_SlideTB,  "SlideTB" },
        { Helpers::TE_SlideBT,  "SlideBT" },
        { Helpers::TE_RollRL,   "RollRL" },
        { Helpers::TE_RollLR,   "RollLR" },
        { Helpers::TE_RollTB,   "RollTB" },
        { Helpers::TE_RollBT,   "RollBT" },
        { Helpers::TE_ScrollRL, "ScrollRL" },
        { Helpers::TE_ScrollLR, "ScrollLR" },
        { Helpers::TE_ScrollTB, "ScrollTB" },
        { Helpers::TE_ScrollBT, "ScrollBT" }
    };

    return stringMap.value(effect, "None");
}

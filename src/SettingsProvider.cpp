#include "SettingsProvider.h"

#include <cstdint>

// Einziger minimaler Header-Abfang für x86-Intrinsics über alle Compiler hinweg
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include <thread>    // Needed for std::thread::hardware_concurrency()
#include <algorithm>

namespace {

    constexpr const char* DEFAULT_INI_FILE_NAME = "mkImageViewer.ini";
    constexpr const char* SECTION_NAME = "mkImageViewer";

    constexpr float COLOR_CORR_DEFAULT_FACTORS[6] = {0.2f, 0.1f, 0.3f, 0.3f, 0.3f, 0.15f};

    // Scrollbereich um jeden Tab legen für kleine Bildschirmauflösungen
    QWidget* makeScrollable(QWidget* content) {
        QScrollArea* area = new QScrollArea();
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidget(content);
        return area;
    }


    void updateButtonColor(QPushButton* button, const QColor& color) {
        button->setText(color.name());
        button->setStyleSheet(QString("background-color: %1; color: %2;")
                                  .arg(color.name())
                                  .arg((color.lightness() < 128) ? "#FFFFFF" : "#000000"));
    }


    QString GetAutoZoomModeString(Helpers::EAutoZoomMode autoZoomMode) {
        switch (autoZoomMode) {
        case Helpers::ZM_FillScreen:       return "Fill";
        case Helpers::ZM_FitToScreen:      return "Fit";
        case Helpers::ZM_FillScreenNoZoom: return "FillNoZoom";
        case Helpers::ZM_None:             return "None";
        case Helpers::ZM_BookMode:         return "BookMode";
        default:                           return "FitNoZoom";
        }
    }


    Helpers::ETransitionEffect ConvertTransitionEffectFromString(QStringView str) {
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

    QString ConvertTransitionEffectToString(Helpers::ETransitionEffect effect) {
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

    // Tests if the CPU supports AVX2, SSE, MMX(2) - Einheitlich für alle x86-Compiler
    Helpers::CPUType ProbeCPU(void) {
        static Helpers::CPUType cpuType = Helpers::CPU_Unknown;

        if (cpuType != Helpers::CPU_Unknown) {
            return cpuType;
        }

        int abcd[4] = { 0 };

        // 1. Check if CPU supports AVX and the xgetbv instruction
#if defined(_MSC_VER)
        __cpuid(abcd, 1);
#else
        __get_cpuid(1, (unsigned int*)&abcd[0], (unsigned int*)&abcd[1], (unsigned int*)&abcd[2], (unsigned int*)&abcd[3]);
#endif

        if ((abcd[2] & 0x18000000) != 0x18000000) { // AVX and OSXSAVE bits
            return (cpuType = Helpers::CPU_SSE);
        }
        if ((abcd[2] & 0x04000000) == 0) { // XSAVE bit, support for xgetbv
            return (cpuType = Helpers::CPU_SSE);
        }

        // 2. Check if operating system supports AVX(2) via xgetbv (Standard auf allen x86-Compilern)
        unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 6) != 6) {
            return (cpuType = Helpers::CPU_SSE);
        }

        // 3. Check if AVX2 instructions are supported
        const int AVX2BITMASK = 1 << 5;
#if defined(_MSC_VER)
        __cpuidex(abcd, 7, 0);
#else
        __cpuid_count(7, 0, abcd[0], abcd[1], abcd[2], abcd[3]);
#endif

        cpuType = (abcd[1] & AVX2BITMASK) ? Helpers::CPU_AVX2 : Helpers::CPU_SSE;
        return cpuType;
    }

} // namespace

std::unique_ptr<CSettingsProvider> CSettingsProvider::sm_instance = nullptr;

CSettingsProvider& CSettingsProvider::This() {
    if (!sm_instance) {
        sm_instance.reset(new CSettingsProvider());
    }
    return *sm_instance;
}

bool CSettingsProvider::ShowSettingsDialog(QWidget* parent) {
    // Prüfen, ob bereits eine QApplication oder QCoreApplication existiert.
    // Falls nicht, wird sie hier lokal für die Dauer des Dialogs erstellt.
    int argc = 0;
    char* argv[] = { nullptr };
    std::unique_ptr<QApplication> localApp;

    if (!qApp) {
        localApp = std::make_unique<QApplication>(argc, argv);
    }

    SettingsDialog dlg(parent);
    dlg.loadFromSettings();
    if (dlg.exec() == QDialog::Accepted) {
        dlg.saveToSettings();
        ReloadAllSettings();
        return true;
    }
    return false;
}

void CSettingsProvider::ReloadAllSettings() {
    ReadWriteableINISettings();
}

// ============================================================================
// SETTINGS DIALOG IMPLEMENTATION
// ============================================================================

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("mkImageViewer - Einstellungen"));
    resize(750, 580);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    m_tabWidget = new QTabWidget(this);

    m_tabWidget->addTab(createProgramTab(), tr("System"));
    m_tabWidget->addTab(createGuiTab(), tr("Fenster"));
    m_tabWidget->addTab(createNavigationTab(), tr("Navigation"));
    m_tabWidget->addTab(createDisplayTab(), tr("Anzeige"));
    m_tabWidget->addTab(createCorrectionTab(), tr("Farb-Korrektur"));
    m_tabWidget->addTab(createSlideshowTab(), tr("Diashow"));

    mainLayout->addWidget(m_tabWidget);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

// ----------------------------------------------------------------------------
// TAB 1: Programm & System
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createProgramTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_checkStoreToExe = new QCheckBox(tr("Einstellungen im App-Ordner speichern"));
    m_checkStoreToExe->setToolTip(tr("Benutzer muß Schreibrechte im App-Ordner haben!"));
    layout->addRow(m_checkStoreToExe);

    m_comboDeleteConfirmation = new QComboBox();
    m_comboDeleteConfirmation->addItems({"Always", "OnlyWhenNoRecycleBin", "Never"});
    m_comboDeleteConfirmation->setToolTip(tr("Rückfrage beim Löschen von Dateien."));
    layout->addRow(tr("Lösch-Bestätigung:"), m_comboDeleteConfirmation);

    m_comboSingleInstance = new QComboBox();
    m_comboSingleInstance->addItems({"Never", "PerFolder", "Always"});
    m_comboSingleInstance->setToolTip(tr("Verhindert, dass mehrere App-Fenster geöffnet werden können."));
    layout->addRow(tr("Einzelne Instanz erzwingen:"), m_comboSingleInstance);

    m_comboCpuType = new QComboBox();
    m_comboCpuType->addItems({"AutoDetect", "Generic", "MMX", "SSE", "AVX2"});
    m_comboCpuType->setToolTip(tr("CPU Befehlssatz-Erweiterung."));
    layout->addRow(tr("CPU-Typ:"), m_comboCpuType);

    m_spinCpuCores = new QSpinBox();
    m_spinCpuCores->setRange(0, 128);
    m_spinCpuCores->setToolTip(tr("Anzahl zu nutzender CPU-Threads (0 = Auto-Erkennung, max 8)."));
    layout->addRow(tr("CPU-Kerne (0 = Auto):"), m_spinCpuCores);

    m_spinCacheRange = new QSpinBox();
    m_spinCacheRange->setRange(0, 8);
    m_spinCacheRange->setToolTip(tr("Cache-Reichweite in der Dateiliste (Seiten vor/zurück)."));
    layout->addRow(tr("Cache-Reichweite:"), m_spinCacheRange);

    m_checkForceQImageReader = new QCheckBox(tr("Alle Bildformate mit QImageReader decodieren"));
    layout->addRow(m_checkForceQImageReader);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// TAB 3: Fenster & Erscheinungsbild
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createGuiTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_checkShowFullScreen = new QCheckBox(tr("Vollbildmodus"));
    layout->addRow(m_checkShowFullScreen);

    m_checkBookModeLaunchFullscreen = new QCheckBox(tr("Comics/Manga im Vollbildmodus starten"));
    layout->addRow(m_checkBookModeLaunchFullscreen);

    m_checkSingleFullScreenInstance = new QCheckBox(tr("Nur eine Vollbild-Instanz erlauben"));
    layout->addRow(m_checkSingleFullScreenInstance);

    m_comboDefaultWindowRect = new QComboBox();
    m_comboDefaultWindowRect->addItems({"sticky", "auto", "max"});
    m_comboDefaultWindowRect->setToolTip(tr("Standardposition und -größe im Fenster-Modus."));
    layout->addRow(tr("Fenstergröße beim Start:"), m_comboDefaultWindowRect);

    QHBoxLayout* minSizeLayout = new QHBoxLayout();
    m_spinMinWidth = new QSpinBox();
    m_spinMinWidth->setRange(100, 3840);
    m_spinMinHeight = new QSpinBox();
    m_spinMinHeight->setRange(100, 2160);
    minSizeLayout->addWidget(new QLabel(tr("Breite:")));
    minSizeLayout->addWidget(m_spinMinWidth);
    minSizeLayout->addWidget(new QLabel(tr("Höhe:")));
    minSizeLayout->addWidget(m_spinMinHeight);
    layout->addRow(tr("Minimale Fenstergröße:"), minSizeLayout);

    m_checkShowFilePathInTitle = new QCheckBox(tr("Vollständigen Dateipfad in Titelleiste anzeigen"));
    layout->addRow(m_checkShowFilePathInTitle);

    m_checkTitleBarUseFileIcon = new QCheckBox(tr("Dateityp-Icon in der Titelleiste nutzen"));
    layout->addRow(m_checkTitleBarUseFileIcon);

    m_spinDisplayMonitor = new QSpinBox();
    m_spinDisplayMonitor->setRange(-1, 16);
    m_spinDisplayMonitor->setToolTip(tr("-1 = Größte Auflösung, 0 = Primär, 1..n = Monitor Index"));
    layout->addRow(tr("Start-Monitor Index:"), m_spinDisplayMonitor);

    m_editDefaultGuiFont = new QLineEdit();
    m_editDefaultGuiFont->setToolTip(tr("Format: \"font name\" fontSize [bold], z.B. \"Arial\" 9.0 bold"));
    layout->addRow(tr("GUI Schriftart:"), m_editDefaultGuiFont);

    m_editZoomFactorFont = new QLineEdit();
    layout->addRow(tr("Zoomfaktor Schriftart:"), m_editZoomFactorFont);

    m_btnBgColor = new QPushButton();
    connect(m_btnBgColor, &QPushButton::clicked, this, [this]() { pickColor(m_colorBg, m_btnBgColor); });
    layout->addRow(tr("Hintergrundfarbe:"), m_btnBgColor);

    m_btnGuiColor = new QPushButton();
    connect(m_btnGuiColor, &QPushButton::clicked, this, [this]() { pickColor(m_colorGui, m_btnGuiColor); });
    layout->addRow(tr("GUI Farbe:"), m_btnGuiColor);

    m_btnTransColor = new QPushButton();
    connect(m_btnTransColor, &QPushButton::clicked, this, [this]() { pickColor(m_colorTrans, m_btnTransColor); });
    layout->addRow(tr("Transparenzfarbe:"), m_btnTransColor);

    m_comboTransparencyMode = new QComboBox();
    m_comboTransparencyMode->addItems({"Checkerboard", "Blend", "BlendInverse"});
    layout->addRow(tr("Transparenzmodus:"), m_comboTransparencyMode);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// TAB 4: Datei-Navigation
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createNavigationTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_comboFileDisplayOrder = new QComboBox();
    m_comboFileDisplayOrder->addItems({"FileName", "LastModDate", "FileSize", "Random"});
    layout->addRow(tr("Sortierung der Dateien:"), m_comboFileDisplayOrder);

    m_checkFileSortAscending = new QCheckBox(tr("Aufsteigend sortieren (FileSortAscending)"));
    layout->addRow(m_checkFileSortAscending);

    m_comboFolderNavigation = new QComboBox();
    m_comboFolderNavigation->addItems({"LoopFolder", "LoopSameFolderLevel", "LoopSubFolders"});
    layout->addRow(tr("Ordner-Navigation:"), m_comboFolderNavigation);

    m_checkWrapAroundFolder = new QCheckBox(tr("Am Ordnerende zum Anfang springen (WrapAroundFolder)"));
    layout->addRow(m_checkWrapAroundFolder);

    m_checkNavigateMouseWheel = new QCheckBox(tr("Mausrad zur Ordner-Navigation nutzen"));
    layout->addRow(m_checkNavigateMouseWheel);

    m_spinMouseWheelZoomSpeed = new QDoubleSpinBox();
    m_spinMouseWheelZoomSpeed->setRange(0.1, 10.0);
    m_spinMouseWheelZoomSpeed->setSingleStep(0.1);
    layout->addRow(tr("Mausrad-Zoom-Geschwindigkeit:"), m_spinMouseWheelZoomSpeed);

    m_checkSmoothPanning = new QCheckBox(tr("Weiches Scrolling"));
    layout->addRow(m_checkSmoothPanning);

    m_checkSmartPanningKeys = new QCheckBox(tr("Pfeiltasten navigieren, sobald Scrolling nicht möglich"));
    layout->addRow(m_checkSmartPanningKeys);


    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// TAB 5: Bildanzeige & Zoom
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createDisplayTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_checkReloadWhenImageChanged = new QCheckBox(tr("Bild neu laden wenn es extern verändert wurde"));
    layout->addRow(m_checkReloadWhenImageChanged);

    m_checkHighQualityResampling = new QCheckBox(tr("Hohe Qualität beim Resampling (HQRS)"));
    layout->addRow(m_checkHighQualityResampling);

    m_comboDownSamplingFilter = new QComboBox();
    m_comboDownSamplingFilter->addItems({"None", "Hermite", "Mitchell", "Catrom", "Lanczos2"});
    layout->addRow(tr("Downsampling-Filter:"), m_comboDownSamplingFilter);

    m_checkAutoRotateExif = new QCheckBox(tr("Bilder automatisch gemäß EXIF drehen"));
    layout->addRow(m_checkAutoRotateExif);

    m_checkUseEmbeddedColorProfiles = new QCheckBox(tr("Eingebettete ICC-Farbprofile anwenden"));
    layout->addRow(m_checkUseEmbeddedColorProfiles);

    m_comboAutoZoomMode = new QComboBox();
    m_comboAutoZoomMode->addItems({"Fit", "FitNoZoom", "Fill", "FillNoZoom", "BookMode", "None"});
    layout->addRow(tr("Auto-Zoom (Fenstermodus):"), m_comboAutoZoomMode);

    m_comboAutoZoomModeFullscreen = new QComboBox();
    m_comboAutoZoomModeFullscreen->addItems({"Fit", "FitNoZoom", "Fill", "FillNoZoom", "BookMode", "None"});
    layout->addRow(tr("Auto-Zoom (Vollbildmodus):"), m_comboAutoZoomModeFullscreen);

    m_spinZoomPausePercent = new QSpinBox();
    m_spinZoomPausePercent->setRange(0, 10000);
    m_spinZoomPausePercent->setSuffix(" %");
    m_spinZoomPausePercent->setToolTip(tr("Pause beim kontinuierlichen Zoomen bei diesem Prozentwert (0 = Deaktivieren)."));
    layout->addRow(tr("Zoom-Pause bei:"), m_spinZoomPausePercent);

    m_spinBookModePageHeight = new QSpinBox();
    m_spinBookModePageHeight->setRange(100, 1000);
    m_spinBookModePageHeight->setSuffix(" %");
    layout->addRow(tr("Buchmodus Seitenhöhe:"), m_spinBookModePageHeight);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// TAB 6: Korrektur & Farbe
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createCorrectionTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_spinContrast = new QDoubleSpinBox();
    m_spinContrast->setRange(-0.5, 0.5);
    m_spinContrast->setSingleStep(0.05);
    layout->addRow(tr("Kontrast (-0.5 .. 0.5):"), m_spinContrast);

    m_spinGamma = new QDoubleSpinBox();
    m_spinGamma->setRange(0.1, 10.0);
    m_spinGamma->setSingleStep(0.1);
    layout->addRow(tr("Gamma (0.1 .. 10.0):"), m_spinGamma);

    m_spinSaturation = new QDoubleSpinBox();
    m_spinSaturation->setRange(0.0, 2.0);
    m_spinSaturation->setSingleStep(0.1);
    layout->addRow(tr("Sättigung (0.0 .. 2.0):"), m_spinSaturation);

    m_checkAutoContrastCorrection = new QCheckBox(tr("Automatische Kontrastkorrektur aktivieren"));
    layout->addRow(m_checkAutoContrastCorrection);

    m_spinAutoContrastAmount = new QDoubleSpinBox();
    m_spinAutoContrastAmount->setRange(0.0, 1.0);
    m_spinAutoContrastAmount->setSingleStep(0.05);
    layout->addRow(tr("Auto-Kontrast Stärke:"), m_spinAutoContrastAmount);

    m_spinAutoBrightnessAmount = new QDoubleSpinBox();
    m_spinAutoBrightnessAmount->setRange(0.0, 1.0);
    m_spinAutoBrightnessAmount->setSingleStep(0.05);
    layout->addRow(tr("Auto-Helligkeit Stärke:"), m_spinAutoBrightnessAmount);

    m_editColorCorrection = new QLineEdit();
    m_editColorCorrection->setToolTip(tr("Format: R: 0.2 G: 0.1 B: 0.35 C: 0.1 M: 0.3 Y: 0.15"));
    layout->addRow(tr("Farbkanalkorrektur:"), m_editColorCorrection);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// TAB 7: Diashow
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createSlideshowTab() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = new QFormLayout(panel);

    m_spinMaxSlideShowFileListSize = new QSpinBox();
    m_spinMaxSlideShowFileListSize->setRange(100, 10000);
    m_spinMaxSlideShowFileListSize->setSuffix(" KB");
    layout->addRow(tr("Max. Dateilistengröße:"), m_spinMaxSlideShowFileListSize);

    m_comboSlideShowTransitionEffect = new QComboBox();
    m_comboSlideShowTransitionEffect->addItems({
        "None", "Blend", "SlideRL", "SlideLR", "SlideTB", "SlideBT",
        "RollRL", "RollLR", "RollTB", "RollBT", "ScrollRL", "ScrollLR", "ScrollTB", "ScrollBT"
    });
    layout->addRow(tr("Übergangseffekt:"), m_comboSlideShowTransitionEffect);

    m_spinSlideShowEffectTimeMs = new QSpinBox();
    m_spinSlideShowEffectTimeMs->setRange(100, 5000);
    m_spinSlideShowEffectTimeMs->setSuffix(" ms");
    layout->addRow(tr("Effektdauer:"), m_spinSlideShowEffectTimeMs);

    m_checkKeepParams = new QCheckBox(tr("Parameter beim Bildwechsel beibehalten (KeepParameters)"));
    layout->addRow(m_checkKeepParams);

    return makeScrollable(panel);
}

void SettingsDialog::pickColor(QColor& target, QPushButton* button) {
    QColor c = QColorDialog::getColor(target, this, tr("Farbe auswählen"));
    if (c.isValid()) {
        target = c;
        updateButtonColor(button, target);
    }
}

void SettingsDialog::loadFromSettings() {
    CSettingsProvider& settings = CSettingsProvider::This();

    // Tab 1
    m_comboDeleteConfirmation->setCurrentIndex(static_cast<int>(settings.m_eDeleteConfirmation));
    m_comboSingleInstance->setCurrentIndex(static_cast<int>(settings.m_eSingleInstanceMode));
    m_spinCpuCores->setValue(settings.m_nNumCoresIni);
    m_spinCacheRange->setValue(settings.m_nCacheRange);
    m_checkStoreToExe->setChecked(settings.m_bStoreToEXEPath);

    // Tab 3
    m_checkShowFullScreen->setChecked(settings.m_bShowFullScreen);
    m_checkBookModeLaunchFullscreen->setChecked(settings.m_bBookModeLaunchFullscreen);
    m_checkSingleFullScreenInstance->setChecked(settings.m_bSingleFullScreenInstance);
    m_spinMinWidth->setValue(settings.m_minimalWindowSize.width());
    m_spinMinHeight->setValue(settings.m_minimalWindowSize.height());
    m_checkShowFilePathInTitle->setChecked(settings.m_bShowFullPathInTitle);
    m_checkTitleBarUseFileIcon->setChecked(settings.m_bTitleBarUseFileIcon);
    m_spinDisplayMonitor->setValue(settings.m_nDisplayMonitor);
    m_editDefaultGuiFont->setText(settings.m_defaultGUIFont);
    m_editZoomFactorFont->setText(settings.m_zoomFactorFont);
    m_colorBg = settings.m_colorBackground;
    m_colorGui = settings.m_colorGUI;
    m_colorTrans = settings.m_colorTransparency;
    updateButtonColor(m_btnBgColor, m_colorBg);
    updateButtonColor(m_btnGuiColor, m_colorGui);
    updateButtonColor(m_btnTransColor, m_colorTrans);

    // Tab 4
    m_comboFileDisplayOrder->setCurrentIndex(static_cast<int>(settings.m_eSorting));
    m_checkFileSortAscending->setChecked(settings.m_bIsSortedAscending);
    m_comboFolderNavigation->setCurrentIndex(static_cast<int>(settings.m_eNavigation));
    m_checkWrapAroundFolder->setChecked(settings.m_bWrapAroundFolder);
    m_checkNavigateMouseWheel->setChecked(settings.m_bNavigateMouseWheel);
    m_spinMouseWheelZoomSpeed->setValue(settings.m_dMouseWheelZoomSpeed);
    m_checkSmoothPanning->setChecked(settings.m_bSmoothPanning);
    m_checkSmartPanningKeys->setChecked(settings.m_bSmartPanningKeys);

    // Tab 5
    m_checkReloadWhenImageChanged->setChecked(settings.m_bReloadWhenDisplayedImageChanged);
    m_checkHighQualityResampling->setChecked(settings.m_bHQRS);
    m_comboDownSamplingFilter->setCurrentIndex(static_cast<int>(settings.m_eDownsamplingFilter));
    m_checkAutoRotateExif->setChecked(settings.m_bAutoRotateEXIF);
    m_checkUseEmbeddedColorProfiles->setChecked(settings.m_bUseEmbeddedColorProfiles);
    m_checkForceQImageReader->setChecked(settings.m_bForceQImageReader);
    m_comboAutoZoomMode->setCurrentText(GetAutoZoomModeString(settings.m_eAutoZoomMode));
    m_comboAutoZoomModeFullscreen->setCurrentText(GetAutoZoomModeString(settings.m_eAutoZoomModeFullscreen));
    m_spinZoomPausePercent->setValue(static_cast<int>(settings.m_zoomPauseFactor * 100));
    m_spinBookModePageHeight->setValue(settings.m_nBookModePageHeight);

    // Tab 6
    m_spinContrast->setValue(settings.m_dContrast);
    m_spinGamma->setValue(settings.m_dGamma);
    m_spinSaturation->setValue(settings.m_dSaturation);
    m_checkAutoContrastCorrection->setChecked(settings.m_bAutoContrastCorrection);
    m_spinAutoContrastAmount->setValue(settings.m_dAutoContrastAmount);
    m_spinAutoBrightnessAmount->setValue(settings.m_dAutoBrightnessAmount);

    if (settings.m_fColorCorrections[0] >= 0) {
        m_editColorCorrection->setText(QString("R: %1 G: %2 B: %3 C: %4 M: %5 Y: %6")
                                           .arg(settings.m_fColorCorrections[0])
                                           .arg(settings.m_fColorCorrections[1])
                                           .arg(settings.m_fColorCorrections[2])
                                           .arg(settings.m_fColorCorrections[3])
                                           .arg(settings.m_fColorCorrections[4])
                                           .arg(settings.m_fColorCorrections[5]));
    } else {
        m_editColorCorrection->setText("");
    }

    // Tab 7
    m_spinMaxSlideShowFileListSize->setValue(settings.m_nMaxSlideShowFileListSize);
    m_comboSlideShowTransitionEffect->setCurrentText(ConvertTransitionEffectToString(settings.m_eSlideShowTransitionEffect));
    m_spinSlideShowEffectTimeMs->setValue(settings.m_nSlideShowEffectTimeMs);
    m_checkKeepParams->setChecked(settings.m_bKeepParams);
}

void SettingsDialog::saveToSettings() {
    CSettingsProvider& settings = CSettingsProvider::This();

    settings.WriteBool("AutoContrastCorrection", m_checkAutoContrastCorrection->isChecked());
    settings.WriteBool("AutoRotateEXIF", m_checkAutoRotateExif->isChecked());
    settings.WriteBool("ShowFullScreen", m_checkShowFullScreen->isChecked());
    settings.WriteBool("BookModeLaunchFullscreen", m_checkBookModeLaunchFullscreen->isChecked());
    settings.WriteBool("FileSortAscending", m_checkFileSortAscending->isChecked());
    settings.WriteBool("ForceQImageReader", m_checkForceQImageReader->isChecked());
    settings.WriteBool("HighQualityResampling", m_checkHighQualityResampling->isChecked());
    settings.WriteBool("KeepParameters", m_checkKeepParams->isChecked());
    settings.WriteBool("NavigateWithMouseWheel", m_checkNavigateMouseWheel->isChecked());
    settings.WriteBool("ReloadWhenDisplayedImageChanged", m_checkReloadWhenImageChanged->isChecked());
    settings.WriteBool("ShowFilePathInTitle", m_checkShowFilePathInTitle->isChecked());
    settings.WriteBool("SingleFullScreenInstance", m_checkSingleFullScreenInstance->isChecked());
    settings.WriteBool("SmartPanningKeys", m_checkSmartPanningKeys->isChecked());
    settings.WriteBool("SmoothPanning", m_checkSmoothPanning->isChecked());
    settings.WriteBool("StoreToEXEPath", m_checkStoreToExe->isChecked());
    settings.WriteBool("TitleBarUseFileIcon", m_checkTitleBarUseFileIcon->isChecked());
    settings.WriteBool("UseEmbeddedColorProfiles", m_checkUseEmbeddedColorProfiles->isChecked());
    settings.WriteBool("WrapAroundFolder", m_checkWrapAroundFolder->isChecked());
    settings.WriteDouble("AutoBrightnessCorrectionAmount", m_spinAutoBrightnessAmount->value());
    settings.WriteDouble("AutoContrastCorrectionAmount", m_spinAutoContrastAmount->value());
    settings.WriteDouble("Contrast", m_spinContrast->value());
    settings.WriteDouble("Gamma", m_spinGamma->value());
    settings.WriteDouble("MouseWheelZoomSpeed", m_spinMouseWheelZoomSpeed->value());
    settings.WriteDouble("Saturation", m_spinSaturation->value());
    settings.WriteInt("BookModePageHeight", m_spinBookModePageHeight->value());
    settings.WriteInt("CPUCoresUsed", m_spinCpuCores->value());
    settings.WriteInt("CacheRange", m_spinCacheRange->value());
    settings.WriteInt("DisplayMonitor", m_spinDisplayMonitor->value());
    settings.WriteInt("MaxSlideShowFileListSizeKB", m_spinMaxSlideShowFileListSize->value());
    settings.WriteInt("SlideShowEffectTime", m_spinSlideShowEffectTimeMs->value());
    settings.WriteInt("ZoomPausePercent", m_spinZoomPausePercent->value());
    settings.WriteString("AutoZoomMode", m_comboAutoZoomMode->currentText());
    settings.WriteString("AutoZoomModeFullscreen", m_comboAutoZoomModeFullscreen->currentText());
    settings.WriteString("BackgroundColor", QString("%1 %2 %3").arg(m_colorBg.red()).arg(m_colorBg.green()).arg(m_colorBg.blue()));
    settings.WriteString("CPUType", m_comboCpuType->currentText());
    settings.WriteString("ColorCorrection", m_editColorCorrection->text());
    settings.WriteString("DefaultGUIFont", m_editDefaultGuiFont->text());
    settings.WriteString("DefaultWindowRect", m_comboDefaultWindowRect->currentText());
    settings.WriteString("DeleteConfirmation", m_comboDeleteConfirmation->currentText());
    settings.WriteString("DownSamplingFilter", m_comboDownSamplingFilter->currentText());
    settings.WriteString("FileDisplayOrder", m_comboFileDisplayOrder->currentText());
    settings.WriteString("FolderNavigation", m_comboFolderNavigation->currentText());
    settings.WriteString("GUIColor", QString("%1 %2 %3").arg(m_colorGui.red()).arg(m_colorGui.green()).arg(m_colorGui.blue()));
    settings.WriteString("MinimalWindowSize", QString("%1 %2").arg(m_spinMinWidth->value()).arg(m_spinMinHeight->value()));
    settings.WriteString("SingleInstance", m_comboSingleInstance->currentText());
    settings.WriteString("SlideShowTransitionEffect", m_comboSlideShowTransitionEffect->currentText());
    settings.WriteString("TransparencyColor", QString("%1 %2 %3").arg(m_colorTrans.red()).arg(m_colorTrans.green()).arg(m_colorTrans.blue()));
    settings.WriteString("TransparencyMode", m_comboTransparencyMode->currentText());
    settings.WriteString("ZoomFactorFont", m_editZoomFactorFont->text());
}

const float* CSettingsProvider::ColorCorrectionAmounts() {
    return (m_fColorCorrections[0] < 0) ? COLOR_CORR_DEFAULT_FACTORS : m_fColorCorrections;
}

CSettingsProvider::CSettingsProvider() {
    // parse command line to find the process startup path
    m_sEXEPath = QCoreApplication::applicationDirPath();
    if (!m_sEXEPath.endsWith('/') && !m_sEXEPath.endsWith('\\')) {
        m_sEXEPath += '/';
    }

    // Check if a custom INI file name is specified on the command line
    m_sIniFileTitle = DEFAULT_INI_FILE_NAME;

    // Global INI file (at EXE location)
    m_sIniNameGlobal = m_sEXEPath + m_sIniFileTitle; // use current dir as startup path

    // Read if the user INI file shall be used
    m_sIniNameUser = m_sIniNameGlobal;
    m_bStoreToEXEPath = GetBool("StoreToEXEPath", false);

    if (!m_bStoreToEXEPath) {
        // User INI file
        QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!settingsPath.endsWith('/') && !settingsPath.endsWith('\\')) {
            settingsPath += '/';
        }
        m_sIniNameUser = settingsPath + m_sIniFileTitle;
        m_bUserINIExists = QFileInfo::exists(m_sIniNameUser);
    } else {
        Helpers::SetSettingsPath(m_sEXEPath);
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

    m_bShowFullScreen = GetBool("ShowFullScreen", true);
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
        m_eCPUAlgorithm = ProbeCPU();
    }

    m_nNumCoresIni = GetInt("CPUCoresUsed", 0, 0, 128);

    if (m_nNumCoresIni == 0) {
        m_nNumCores = static_cast<int>(std::thread::hardware_concurrency());
        m_nNumCores = std::clamp(m_nNumCores, 1, 8);
        // Limiting to 8 cores, since benchmarks suggest performance improvements start to flatline around that point.
        // Performance can even become worse: 16 threads on 16 thread CPU performing worse than running just 14 threads. Or 8 threads, for that matter.
        // People can still try to use more cores by specifying a non-zero number for "CPUCoresUsed",
        // but I advise to do some benchmarking in this case to make sure you aren't actually shooting yourself in the foot.
    } else {
        m_nNumCores = m_nNumCoresIni;
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
    m_zoomPauseFactor = GetInt("ZoomPausePercent", 100, 0, 6553500) / 100.0;  // can't have a % larger than the MAX_IMAGE_DIMENSION %, and convert to a scale factor (double/double division) only once
    m_bAutoRotateEXIF = GetBool("AutoRotateEXIF", true);
    m_bUseEmbeddedColorProfiles = GetBool("UseEmbeddedColorProfiles", false);
    m_bForceQImageReader = GetBool("ForceQImageReader", false);
    m_nDisplayMonitor = GetInt("DisplayMonitor", -1, -1, 16);
    m_dAutoContrastAmount = GetDouble("AutoContrastCorrectionAmount", 0.5, 0.0, 1.0);
    m_dAutoBrightnessAmount = GetDouble("AutoBrightnessCorrectionAmount", 0.2, 0.0, 1.0);

    m_defaultWindowRect = GetRect("DefaultWindowRect", QRect(0, 0, 0, 0));
    m_stickyWindowRect = GetRect("StickyWindowRect", QRect(0, 0, 0, 0));
    m_bDefaultMaximized = false;
    m_bStickyWindowSize = false;
    m_bExplicitWindowRect = false;

    if (m_defaultWindowRect.isEmpty()) {
        QString sAuto = GetString("DefaultWindowRect", "");
        if (sAuto.compare("max", Qt::CaseInsensitive) == 0) {
            m_bDefaultMaximized = true;
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

    m_minimalWindowSize = GetSize("MinimalWindowSize", QSize(320, 240));

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

    QString sSorting = GetString("FileDisplayOrder", "FileName");
    if (sSorting.compare("LastModDate", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_LastModTime;
    } else if (sSorting.compare("Random", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_Random;
    } else if (sSorting.compare("FileSize", Qt::CaseInsensitive) == 0) {
        m_eSorting = Helpers::FS_FileSize;
    } else {
        m_eSorting = Helpers::FS_FileName;
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
    if (eFileSorting == Helpers::FS_LastModTime) {
        sSorting = "LastModDate";
    } else if (eFileSorting == Helpers::FS_FileSize) {
        sSorting = "FileSize";
    } else if (eFileSorting == Helpers::FS_Random) {
        sSorting = "Random";
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

void CSettingsProvider::SaveStickyWindowRect(QRect rect) {
    if (m_stickyWindowRect != rect && !rect.isEmpty()) {
        QString sRect = QString("%1 %2 %3 %4")
                            .arg(rect.left())
                            .arg(rect.top())
                            .arg(rect.right())
                            .arg(rect.bottom());

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

    // Create app folder in appdata directory and copy INI file if it does not exist
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

void CSettingsProvider::WriteString(const QString& sKey, const QString& sString) {
    QSettings settings(m_sIniNameUser, QSettings::IniFormat);
    settings.beginGroup(SECTION_NAME);
    if (settings.value(sKey).toString() != sString) {
        settings.setValue(sKey, sString);
    }
    settings.endGroup();
}

void CSettingsProvider::WriteDouble(const QString& sKey, double dValue) {
    // 'g' entfernt trailing zeroes. 15 ist die Standard-Präzision für double.
    QString sVal = QString::number(dValue, 'g', 15);

    // Falls es eine Ganzzahl ist (kein '.' im String), mindestens eine Null anhängen
    if (!sVal.contains('.')) {
        sVal += ".0";
    }

    WriteString(sKey, sVal);
}

void CSettingsProvider::WriteBool(const QString& sKey, bool bValue) {
    WriteString(sKey, bValue ? "true" : "false");
}

void CSettingsProvider::WriteInt(const QString& sKey, int nValue) {
    WriteString(sKey, QString::number(nValue));
}

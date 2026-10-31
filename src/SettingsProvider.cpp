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
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <thread>    // Needed for std::thread::hardware_concurrency()
#include <algorithm>

namespace {

    // Form of a page: labels right aligned, fields in their natural size, the form centered at the top
    QFormLayout* createFormLayout(QWidget* panel) {
        QFormLayout* layout = new QFormLayout(panel);
        layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        layout->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
        layout->setContentsMargins(10, 10, 10, 10);
        return layout;
    }

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
        case Helpers::ZM_FitToScreenNoZoom: return "FitNoZoom";
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
        if ((abcd[2] & 0x00001000) == 0) { // FMA bit, the AVX2 resampling uses FMA instructions
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
    setWindowTitle(tr("Settings"));
    resize(750, 580);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QHBoxLayout* sectionsLayout = new QHBoxLayout();
    m_categoryList = new QListWidget(this);
    m_categoryList->setIconSize(QSize(24, 24));
    m_categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The selected section stays clearly marked when the list does not have the focus (the theme's inactive
    // selection is dimmed): inactive selection colors = active ones, the colors of the theme stay
    QPalette listPalette = m_categoryList->palette();
    listPalette.setColor(QPalette::Inactive, QPalette::Highlight, listPalette.color(QPalette::Active, QPalette::Highlight));
    listPalette.setColor(QPalette::Inactive, QPalette::HighlightedText, listPalette.color(QPalette::Active, QPalette::HighlightedText));
    m_categoryList->setPalette(listPalette);
    m_pages = new QStackedWidget(this);
    sectionsLayout->addWidget(m_categoryList);
    sectionsLayout->addWidget(m_pages, 1);
    mainLayout->addLayout(sectionsLayout);

    addPage(QStringLiteral("view-preview"), tr("Image"), createImagePage());
    addPage(QStringLiteral("color-management"), tr("Quality & Color"), createQualityPage());
    addPage(QStringLiteral("preferences-system-windows"), tr("Window"), createWindowPage());
    addPage(QStringLiteral("go-next-view"), tr("Navigation"), createNavigationPage());
    addPage(QStringLiteral("preferences-system"), tr("System"), createSystemPage());

    // The list is as wide as its longest entry
    m_categoryList->setFixedWidth(m_categoryList->sizeHintForColumn(0) + 2 * m_categoryList->frameWidth() + 8);
    connect(m_categoryList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(std::clamp(CSettingsProvider::This().LastSettingsPage(), 0, m_pages->count() - 1));
    // connected after the initial selection: only a page the user chooses is remembered
    connect(m_categoryList, &QListWidget::currentRowChanged, this, [](int nRow) {
        CSettingsProvider::This().SaveLastSettingsPage(nRow);
    });

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    QPushButton* btnRestoreDefaults = buttonBox->button(QDialogButtonBox::RestoreDefaults);
    btnRestoreDefaults->setToolTip(tr("Resets all options to their defaults; nothing is saved until OK"));
    connect(btnRestoreDefaults, &QPushButton::clicked, this, &SettingsDialog::restoreDefaults);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

void SettingsDialog::addPage(const QString& sIconName, const QString& sTitle, QWidget* page) {
    // Icon from the desktop theme (Breeze under KDE); no icon if the theme does not have it
    m_categoryList->addItem(new QListWidgetItem(QIcon::fromTheme(sIconName), sTitle));
    m_pages->addWidget(page);
}

// ----------------------------------------------------------------------------
// Helpers for the pages
// ----------------------------------------------------------------------------
namespace {
    // A choice of a combo box: text shown to the user, value written to the INI file
    struct Choice {
        QString sText;
        QString sIniValue;
    };

    QComboBox* createChoiceBox(std::initializer_list<Choice> choices) {
        QComboBox* combo = new QComboBox();
        for (const Choice& choice : choices) {
            combo->addItem(choice.sText, choice.sIniValue);
        }
        return combo;
    }

    // Selects the entry for an INI value (case-insensitive like the INI parser); sDefault if the value is unknown
    void selectChoice(QComboBox* combo, const QString& sIniValue, const QString& sDefault) {
        int nIndex = combo->findData(sIniValue, Qt::UserRole, Qt::MatchFixedString);
        if (nIndex < 0) {
            nIndex = combo->findData(sDefault, Qt::UserRole, Qt::MatchFixedString);
        }
        combo->setCurrentIndex(std::max(0, nIndex));
    }

    QString chosenIniValue(const QComboBox* combo) {
        return combo->currentData().toString();
    }

}

// ----------------------------------------------------------------------------
// Page "Image": zoom, rotation, background and transparency of the single image
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createImagePage() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = createFormLayout(panel);

    auto autoZoomChoices = [this]() {
        return createChoiceBox({ { tr("Fit"), "Fit" }, { tr("Fit, don't enlarge"), "FitNoZoom" },
                                 { tr("Fill"), "Fill" }, { tr("Fill, don't enlarge"), "FillNoZoom" },
                                 { tr("Book mode"), "BookMode" }, { tr("None (100 %)"), "None" } });
    };
    m_comboAutoZoomMode = autoZoomChoices();
    m_comboAutoZoomMode->setToolTip(tr("Zoom when an image is opened in the window (AutoZoomMode)"));
    m_comboAutoZoomModeFullscreen = autoZoomChoices();
    m_comboAutoZoomModeFullscreen->setToolTip(tr("Zoom when an image is opened in fullscreen (AutoZoomModeFullscreen)"));
    layout->addRow(tr("Auto zoom (window):"), m_comboAutoZoomMode);
    layout->addRow(tr("Auto zoom (fullscreen):"), m_comboAutoZoomModeFullscreen);

    m_spinZoomPausePercent = new QSpinBox();
    m_spinZoomPausePercent->setRange(0, 10000);
    m_spinZoomPausePercent->setSuffix(" %");
    m_spinZoomPausePercent->setToolTip(tr("Continuous zooming pauses briefly at this value, 0 = never (ZoomPausePercent)"));
    layout->addRow(tr("Zoom pause at:"), m_spinZoomPausePercent);

    m_spinBookModePageHeight = new QSpinBox();
    m_spinBookModePageHeight->setRange(100, 1000);
    m_spinBookModePageHeight->setSuffix(" %");
    m_spinBookModePageHeight->setToolTip(tr("BookModePageHeight"));
    layout->addRow(tr("Book mode page height:"), m_spinBookModePageHeight);

    m_checkAutoRotateExif = new QCheckBox(tr("Auto rotate according to EXIF"));
    m_checkAutoRotateExif->setToolTip(tr("AutoRotateEXIF"));
    layout->addRow("", m_checkAutoRotateExif);

    m_btnBgColor = new QPushButton();
    m_btnBgColor->setToolTip(tr("BackgroundColor"));
    connect(m_btnBgColor, &QPushButton::clicked, this, [this]() { pickColor(m_colorBg, m_btnBgColor); });
    layout->addRow(tr("Background color:"), m_btnBgColor);

    m_comboTransparencyMode = createChoiceBox({ { tr("Checkerboard"), "Checkerboard" },
                                                { tr("Transparency color"), "Blend" },
                                                { tr("Transparency color, inverted"), "BlendInverse" } });
    m_comboTransparencyMode->setToolTip(tr("What is shown behind transparent parts of the image (TransparencyMode)"));
    m_btnTransColor = new QPushButton();
    m_btnTransColor->setToolTip(tr("TransparencyColor"));
    connect(m_btnTransColor, &QPushButton::clicked, this, [this]() { pickColor(m_colorTrans, m_btnTransColor); });
    // The color right of the mode, hidden for the checkerboard (it does not use the color)
    QWidget* transparencyRow = new QWidget();
    QHBoxLayout* transparencyLayout = new QHBoxLayout(transparencyRow);
    transparencyLayout->setContentsMargins(0, 0, 0, 0);
    transparencyLayout->addWidget(m_comboTransparencyMode);
    QSizePolicy colorPolicy = m_btnTransColor->sizePolicy();
    colorPolicy.setRetainSizeWhenHidden(true);  // keeps its place: the centered form does not jump when it appears
    m_btnTransColor->setSizePolicy(colorPolicy);
    transparencyLayout->addWidget(m_btnTransColor);
    transparencyLayout->addStretch();
    auto updateTransparencyColor = [this]() {
        m_btnTransColor->setVisible(chosenIniValue(m_comboTransparencyMode) != QStringLiteral("Checkerboard"));
    };
    connect(m_comboTransparencyMode, &QComboBox::currentIndexChanged, this, updateTransparencyColor);
    updateTransparencyColor();
    layout->addRow(tr("Transparency:"), transparencyRow);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// Page "Quality & Color": resampling, color profiles, automatic correction
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createQualityPage() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = createFormLayout(panel);

    m_checkHighQualityResampling = new QCheckBox(tr("Use resampling"));
    m_checkHighQualityResampling->setToolTip(tr("HighQualityResampling"));
    m_comboDownSamplingFilter = createChoiceBox({ { tr("None (fast)"), "None" }, { tr("Hermite"), "Hermite" },
                                                  { tr("Mitchell"), "Mitchell" }, { tr("Catmull-Rom"), "Catrom" },
                                                  { tr("Lanczos-2"), "Lanczos2" } });
    m_comboDownSamplingFilter->setToolTip(tr("Filter used for downscaling (DownSamplingFilter)"));
    layout->addRow("", m_checkHighQualityResampling);
    layout->addRow(tr("Downsampling filter:"), m_comboDownSamplingFilter);

    m_checkUseEmbeddedColorProfiles = new QCheckBox(tr("Apply embedded ICC profiles"));
    m_checkUseEmbeddedColorProfiles->setToolTip(tr("UseEmbeddedColorProfiles"));
    layout->addRow("", m_checkUseEmbeddedColorProfiles);

    m_checkAutoContrastCorrection = new QCheckBox(tr("Enable auto contrast correction"));
    m_checkAutoContrastCorrection->setToolTip(tr("AutoContrastCorrection"));
    m_spinAutoContrastAmount = new QDoubleSpinBox();
    m_spinAutoContrastAmount->setRange(0.0, 1.0);
    m_spinAutoContrastAmount->setSingleStep(0.05);
    m_spinAutoContrastAmount->setToolTip(tr("AutoContrastCorrectionAmount"));
    m_spinAutoBrightnessAmount = new QDoubleSpinBox();
    m_spinAutoBrightnessAmount->setRange(0.0, 1.0);
    m_spinAutoBrightnessAmount->setSingleStep(0.05);
    m_spinAutoBrightnessAmount->setToolTip(tr("AutoBrightnessCorrectionAmount"));
    layout->addRow("", m_checkAutoContrastCorrection);
    layout->addRow(tr("Contrast correction strength"), m_spinAutoContrastAmount);
    layout->addRow(tr("Brightness correction strength"), m_spinAutoBrightnessAmount);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// Page "Window": startup and title bar
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createWindowPage() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = createFormLayout(panel);

    m_checkShowFullScreen = new QCheckBox(tr("Launch images fullscreen"));
    m_checkShowFullScreen->setToolTip(tr("ShowFullScreen"));
    m_checkBookModeLaunchFullscreen = new QCheckBox(tr("Launch comics/manga fullscreen"));
    m_checkBookModeLaunchFullscreen->setToolTip(tr("BookModeLaunchFullscreen"));
    layout->addRow("", m_checkShowFullScreen);
    layout->addRow("", m_checkBookModeLaunchFullscreen);

    m_spinDisplayMonitor = new QSpinBox();
    m_spinDisplayMonitor->setRange(-1, 16);
    m_spinDisplayMonitor->setToolTip(tr("-1 = highest resolution, 0 = primary monitor, 1… = monitor number (DisplayMonitor)"));
    layout->addRow(tr("Startup monitor:"), m_spinDisplayMonitor);

    m_checkShowFilePathInTitle = new QCheckBox(tr("Show full file path"));
    m_checkShowFilePathInTitle->setToolTip(tr("ShowFilePathInTitle"));
    m_checkShowAppNameInTitle = new QCheckBox(tr("Show application name"));
    m_checkShowAppNameInTitle->setToolTip(tr("\" - mkImageViewer\" at the end of the title (ShowAppNameInTitle)"));
    m_checkTitleBarUseFileIcon = new QCheckBox(tr("Use specific file type icon"));
    m_checkTitleBarUseFileIcon->setToolTip(tr("TitleBarUseFileIcon"));
    layout->addRow(tr("Title bar:"), m_checkShowFilePathInTitle);
    layout->addRow("", m_checkShowAppNameInTitle);
    layout->addRow("", m_checkTitleBarUseFileIcon);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// Page "Navigation": order, folders, mouse, keyboard, slideshow
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createNavigationPage() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = createFormLayout(panel);

    m_comboFileDisplayOrder = createChoiceBox({ { tr("File name"), "FileName" }, { tr("Modification date"), "LastModDate" },
                                                { tr("File size"), "FileSize" }, { tr("Random"), "Random" } });
    m_comboFileDisplayOrder->setToolTip(tr("FileDisplayOrder"));
    m_checkFileSortAscending = new QCheckBox(tr("Ascending"));
    m_checkFileSortAscending->setToolTip(tr("FileSortAscending"));
    layout->addRow(tr("Sorting:"), m_comboFileDisplayOrder);
    layout->addRow("", m_checkFileSortAscending);

    m_comboFolderNavigation = createChoiceBox({ { tr("This folder only"), "LoopFolder" },
                                                { tr("Folders on the same level"), "LoopSameFolderLevel" },
                                                { tr("Including subfolders"), "LoopSubFolders" } });
    m_comboFolderNavigation->setToolTip(tr("FolderNavigation"));
    m_checkWrapAroundFolder = new QCheckBox(tr("Wrap around at the end of the folder"));
    m_checkWrapAroundFolder->setToolTip(tr("WrapAroundFolder"));
    layout->addRow(tr("Folder:"), m_comboFolderNavigation);
    layout->addRow("", m_checkWrapAroundFolder);

    m_checkNavigateMouseWheel = new QCheckBox(tr("Use mouse wheel to switch images"));
    m_checkNavigateMouseWheel->setToolTip(tr("NavigateWithMouseWheel"));
    m_spinMouseWheelZoomSpeed = new QDoubleSpinBox();
    m_spinMouseWheelZoomSpeed->setRange(0.1, 10.0);
    m_spinMouseWheelZoomSpeed->setSingleStep(0.1);
    m_spinMouseWheelZoomSpeed->setToolTip(tr("MouseWheelZoomSpeed"));
    layout->addRow("Mouse wheel", m_checkNavigateMouseWheel);
    layout->addRow(tr("Zoom amount:"), m_spinMouseWheelZoomSpeed);

    m_checkSmoothPanning = new QCheckBox(tr("Smooth scrolling"));
    m_checkSmoothPanning->setToolTip(tr("SmoothPanning"));
    m_checkSmartPanningKeys = new QCheckBox(tr("Arrow keys switch images when there is nothing to scroll"));
    m_checkSmartPanningKeys->setToolTip(tr("SmartPanningKeys"));
    layout->addRow(tr("Keyboard:"), m_checkSmoothPanning);
    layout->addRow("", m_checkSmartPanningKeys);

    m_comboSlideShowTransitionEffect = createChoiceBox({
        { tr("None"), "None" }, { tr("Blend"), "Blend" },
        { tr("Slide from right"), "SlideRL" }, { tr("Slide from left"), "SlideLR" },
        { tr("Slide from top"), "SlideTB" }, { tr("Slide from bottom"), "SlideBT" },
        { tr("Roll from right"), "RollRL" }, { tr("Roll from left"), "RollLR" },
        { tr("Roll from top"), "RollTB" }, { tr("Roll from bottom"), "RollBT" },
        { tr("Scroll from right"), "ScrollRL" }, { tr("Scroll from left"), "ScrollLR" },
        { tr("Scroll from top"), "ScrollTB" }, { tr("Scroll from bottom"), "ScrollBT" } });
    m_comboSlideShowTransitionEffect->setToolTip(tr("SlideShowTransitionEffect"));
    m_spinSlideShowEffectTimeMs = new QSpinBox();
    m_spinSlideShowEffectTimeMs->setRange(100, 5000);
    m_spinSlideShowEffectTimeMs->setSuffix(" ms");
    m_spinSlideShowEffectTimeMs->setToolTip(tr("SlideShowEffectTime"));
    layout->addRow(tr("Slideshow transition effect:"), m_comboSlideShowTransitionEffect);
    layout->addRow(tr("Slideshow transition duration:"), m_spinSlideShowEffectTimeMs);

    return makeScrollable(panel);
}

// ----------------------------------------------------------------------------
// Page "System": instances, file monitoring, preloading, processor, decoder
// ----------------------------------------------------------------------------
QWidget* SettingsDialog::createSystemPage() {
    QWidget* panel = new QWidget();
    QFormLayout* layout = createFormLayout(panel);

    m_comboSingleInstance = createChoiceBox({ { tr("Never"), "Never" }, { tr("Per folder"), "PerFolder" }, { tr("Always"), "Always" } });
    m_comboSingleInstance->setToolTip(tr("An already open window takes over newly opened images (SingleInstance)"));
    m_checkSingleFullScreenInstance = new QCheckBox(tr("Only one fullscreen instance"));
    m_checkSingleFullScreenInstance->setToolTip(tr("SingleFullScreenInstance"));

    layout->addRow(tr("Single instance:"), m_comboSingleInstance);
    layout->addRow("", m_checkSingleFullScreenInstance);

    m_checkReloadWhenImageChanged = new QCheckBox(tr("Reload the image when it has changed"));
    m_checkReloadWhenImageChanged->setToolTip(tr("ReloadWhenDisplayedImageChanged"));
    layout->addRow("", m_checkReloadWhenImageChanged);

    m_spinCacheRange = new QSpinBox();
    m_spinCacheRange->setRange(0, 8);
    m_spinCacheRange->setToolTip(tr("Number of images preloaded forward and backward (CacheRange)"));
    layout->addRow(tr("Preloading range:"), m_spinCacheRange);

    m_comboCpuType = createChoiceBox({ { tr("Automatic"), "AutoDetect" }, { tr("Generic"), "Generic" },
                                       { tr("MMX"), "MMX" }, { tr("SSE2"), "SSE" }, { tr("AVX2"), "AVX2" } });
    m_comboCpuType->setToolTip(tr("Instruction set used for resampling (CPUType)"));
    m_spinCpuCores = new QSpinBox();
    m_spinCpuCores->setRange(0, 128);
    m_spinCpuCores->setToolTip(tr("0 = automatic, then at most 8 (CPUCoresUsed)"));
    layout->addRow(tr("CPU instruction set:"), m_comboCpuType);
    layout->addRow(tr("CPU threads:"), m_spinCpuCores);

    m_checkForceQImageReader = new QCheckBox(tr("Read all formats with Qt (QImageReader)"));
    m_checkForceQImageReader->setToolTip(tr("ForceQImageReader"));
    layout->addRow("", m_checkForceQImageReader);

    return makeScrollable(panel);
}

void SettingsDialog::pickColor(QColor& target, QPushButton* button) {
    QColor c = QColorDialog::getColor(target, this, tr("Select color"));
    if (c.isValid()) {
        target = c;
        updateButtonColor(button, target);
    }
}

void SettingsDialog::loadFromSettings() {
    loadFrom(CSettingsProvider::This());
}

void SettingsDialog::restoreDefaults() {
    // Only the controls change; the INI is written on OK, Cancel keeps the stored settings
    loadFrom(CSettingsProvider(CSettingsProvider::DefaultsOnly{}));
}

void SettingsDialog::loadFrom(const CSettingsProvider& settings) {
    // Combo boxes from the INI text, so the shown entry is exactly what is stored (the defaults are those of the parser)
    selectChoice(m_comboAutoZoomMode, GetAutoZoomModeString(settings.m_eAutoZoomMode), "Fit");
    selectChoice(m_comboAutoZoomModeFullscreen, GetAutoZoomModeString(settings.m_eAutoZoomModeFullscreen), "Fit");
    selectChoice(m_comboTransparencyMode, settings.GetString("TransparencyMode", "Checkerboard"), "Checkerboard");
    selectChoice(m_comboDownSamplingFilter, settings.GetString("DownSamplingFilter", "Catrom"), "Catrom");
    selectChoice(m_comboFileDisplayOrder, settings.GetString("FileDisplayOrder", "FileName"), "FileName");
    selectChoice(m_comboFolderNavigation, settings.GetString("FolderNavigation", "LoopFolder"), "LoopFolder");
    selectChoice(m_comboSlideShowTransitionEffect, ConvertTransitionEffectToString(settings.m_eSlideShowTransitionEffect), "None");
    selectChoice(m_comboSingleInstance, settings.GetString("SingleInstance", "PerFolder"), "PerFolder");
    selectChoice(m_comboCpuType, settings.GetString("CPUType", "AutoDetect"), "AutoDetect");

    // Image
    m_spinZoomPausePercent->setValue(static_cast<int>(settings.m_zoomPauseFactor * 100));
    m_spinBookModePageHeight->setValue(settings.m_nBookModePageHeight);
    m_checkAutoRotateExif->setChecked(settings.m_bAutoRotateEXIF);
    m_colorBg = settings.m_colorBackground;
    m_colorTrans = settings.m_colorTransparency;
    updateButtonColor(m_btnBgColor, m_colorBg);
    updateButtonColor(m_btnTransColor, m_colorTrans);

    // Quality & Color
    m_checkHighQualityResampling->setChecked(settings.m_bHQRS);
    m_checkUseEmbeddedColorProfiles->setChecked(settings.m_bUseEmbeddedColorProfiles);
    m_checkAutoContrastCorrection->setChecked(settings.m_bAutoContrastCorrection);
    m_spinAutoContrastAmount->setValue(settings.m_dAutoContrastAmount);
    m_spinAutoBrightnessAmount->setValue(settings.m_dAutoBrightnessAmount);

    // Window
    m_checkShowFullScreen->setChecked(settings.m_bShowFullScreen);
    m_checkBookModeLaunchFullscreen->setChecked(settings.m_bBookModeLaunchFullscreen);
    m_spinDisplayMonitor->setValue(settings.m_nDisplayMonitor);
    m_checkShowFilePathInTitle->setChecked(settings.m_bShowFullPathInTitle);
    m_checkShowAppNameInTitle->setChecked(settings.m_bShowAppNameInTitle);
    m_checkTitleBarUseFileIcon->setChecked(settings.m_bTitleBarUseFileIcon);

    // Navigation
    m_checkFileSortAscending->setChecked(settings.m_bIsSortedAscending);
    m_checkWrapAroundFolder->setChecked(settings.m_bWrapAroundFolder);
    m_checkNavigateMouseWheel->setChecked(settings.m_bNavigateMouseWheel);
    m_spinMouseWheelZoomSpeed->setValue(settings.m_dMouseWheelZoomSpeed);
    m_checkSmoothPanning->setChecked(settings.m_bSmoothPanning);
    m_checkSmartPanningKeys->setChecked(settings.m_bSmartPanningKeys);
    m_spinSlideShowEffectTimeMs->setValue(settings.m_nSlideShowEffectTimeMs);

    // System
    m_checkSingleFullScreenInstance->setChecked(settings.m_bSingleFullScreenInstance);
    m_checkReloadWhenImageChanged->setChecked(settings.m_bReloadWhenDisplayedImageChanged);
    m_spinCacheRange->setValue(settings.m_nCacheRange);
    m_spinCpuCores->setValue(settings.m_nNumCoresIni);
    m_checkForceQImageReader->setChecked(settings.m_bForceQImageReader);
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
    settings.WriteBool("NavigateWithMouseWheel", m_checkNavigateMouseWheel->isChecked());
    settings.WriteBool("ReloadWhenDisplayedImageChanged", m_checkReloadWhenImageChanged->isChecked());
    settings.WriteBool("ShowAppNameInTitle", m_checkShowAppNameInTitle->isChecked());
    settings.WriteBool("ShowFilePathInTitle", m_checkShowFilePathInTitle->isChecked());
    settings.WriteBool("SingleFullScreenInstance", m_checkSingleFullScreenInstance->isChecked());
    settings.WriteBool("SmartPanningKeys", m_checkSmartPanningKeys->isChecked());
    settings.WriteBool("SmoothPanning", m_checkSmoothPanning->isChecked());
    settings.WriteBool("TitleBarUseFileIcon", m_checkTitleBarUseFileIcon->isChecked());
    settings.WriteBool("UseEmbeddedColorProfiles", m_checkUseEmbeddedColorProfiles->isChecked());
    settings.WriteBool("WrapAroundFolder", m_checkWrapAroundFolder->isChecked());
    settings.WriteDouble("AutoBrightnessCorrectionAmount", m_spinAutoBrightnessAmount->value());
    settings.WriteDouble("AutoContrastCorrectionAmount", m_spinAutoContrastAmount->value());
    settings.WriteDouble("MouseWheelZoomSpeed", m_spinMouseWheelZoomSpeed->value());
    settings.WriteInt("BookModePageHeight", m_spinBookModePageHeight->value());
    settings.WriteInt("CPUCoresUsed", m_spinCpuCores->value());
    settings.WriteInt("CacheRange", m_spinCacheRange->value());
    settings.WriteInt("DisplayMonitor", m_spinDisplayMonitor->value());
    settings.WriteInt("SlideShowEffectTime", m_spinSlideShowEffectTimeMs->value());
    settings.WriteInt("ZoomPausePercent", m_spinZoomPausePercent->value());
    settings.WriteString("AutoZoomMode", chosenIniValue(m_comboAutoZoomMode));
    settings.WriteString("AutoZoomModeFullscreen", chosenIniValue(m_comboAutoZoomModeFullscreen));
    settings.WriteString("BackgroundColor", QString("%1 %2 %3").arg(m_colorBg.red()).arg(m_colorBg.green()).arg(m_colorBg.blue()));
    settings.WriteString("CPUType", chosenIniValue(m_comboCpuType));
    settings.WriteString("DownSamplingFilter", chosenIniValue(m_comboDownSamplingFilter));
    settings.WriteString("FileDisplayOrder", chosenIniValue(m_comboFileDisplayOrder));
    settings.WriteString("FolderNavigation", chosenIniValue(m_comboFolderNavigation));
    settings.WriteString("SingleInstance", chosenIniValue(m_comboSingleInstance));
    settings.WriteString("SlideShowTransitionEffect", chosenIniValue(m_comboSlideShowTransitionEffect));
    settings.WriteString("TransparencyColor", QString("%1 %2 %3").arg(m_colorTrans.red()).arg(m_colorTrans.green()).arg(m_colorTrans.blue()));
    settings.WriteString("TransparencyMode", chosenIniValue(m_comboTransparencyMode));
}

void CSettingsProvider::ReadStickyWindowRect() {
    m_stickyWindowRect = QRect(0, 0, 0, 0);

    if (m_stateFileFullPath.isEmpty() || !QFileInfo::exists(m_stateFileFullPath)) {
        return;
    }

    QSettings stateSettings(m_stateFileFullPath, QSettings::IniFormat);
    stateSettings.beginGroup("State");
    const QString s = stateSettings.value("StickyWindowRect").toString();
    stateSettings.endGroup();

    // Format: "left top right bottom", z.B. "302 93 1412 950"
    const auto tokens = s.split(' ', Qt::SkipEmptyParts);
    if (tokens.size() == 4) {
        bool ok1, ok2, ok3, ok4;
        const int left   = tokens[0].toInt(&ok1);
        const int top    = tokens[1].toInt(&ok2);
        const int right  = tokens[2].toInt(&ok3);
        const int bottom = tokens[3].toInt(&ok4);
        if (ok1 && ok2 && ok3 && ok4) {
            m_stickyWindowRect = QRect(QPoint(left, top), QPoint(right, bottom)).normalized();
        }
    }

    m_bExplicitWindowRect = !m_stickyWindowRect.isEmpty();
}

CSettingsProvider::CSettingsProvider() {
    // Load main settings

    // ----------------------------------------------------------------------------------
    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QDir settingsDir(settingsPath);
    if (!settingsDir.exists()) {
        if (!settingsDir.mkpath(QStringLiteral("."))) { // Erstellt Verzeichnis inkl. Unterordner
            return;
        }
    }
    
    m_settingsFileFullPath = QDir(settingsPath).filePath(QStringLiteral("settings.ini"));

    // ----------------------------------------------------------------------------------

    const QString statePath = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    const QDir stateDir(statePath);
    if (!stateDir.exists()) {
        if (!stateDir.mkpath(QStringLiteral("."))) { // Erstellt Verzeichnis inkl. Unterordner
            return;
        }
    }

    m_stateFileFullPath = QDir(statePath).filePath(QStringLiteral("state.ini"));

    ReadAllINISettings();
}

CSettingsProvider::CSettingsProvider(DefaultsOnly) {
    // Without a settings file path every Get...() returns its default
    ReadAllINISettings();
}

void CSettingsProvider::ReadAllINISettings() {
    // Read settings that can be written with SaveSettings()
    ReadWriteableINISettings();

    // Read sticky window position (StateLocation)
    ReadStickyWindowRect();
    
    m_bShowFullScreen = GetBool("ShowFullScreen", false);
    m_bShowFullPathInTitle = GetBool("ShowFilePathInTitle", true);
    m_bShowAppNameInTitle = GetBool("ShowAppNameInTitle", true);

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

    m_bNavigateMouseWheel = GetBool("NavigateWithMouseWheel", true);
    m_dMouseWheelZoomSpeed = GetDouble("MouseWheelZoomSpeed", 1.0, 0.1, 10.0);

    m_nSlideShowEffectTimeMs = GetInt("SlideShowEffectTime", 200, 100, 5000);
    m_bSingleFullScreenInstance = GetBool("SingleFullScreenInstance", true);
    m_bWrapAroundFolder = GetBool("WrapAroundFolder", true);
    m_zoomPauseFactor = GetInt("ZoomPausePercent", 100, 0, 6553500) / 100.0;  // can't have a % larger than the MAX_IMAGE_DIMENSION %, and convert to a scale factor (double/double division) only once
    m_bAutoRotateEXIF = GetBool("AutoRotateEXIF", true);
    m_bUseEmbeddedColorProfiles = GetBool("UseEmbeddedColorProfiles", true); // This can drastically slow down image loading/decoding.
    m_bForceQImageReader = GetBool("ForceQImageReader", false);
    m_nDisplayMonitor = GetInt("DisplayMonitor", -1, -1, 16);
    m_dAutoContrastAmount = GetDouble("AutoContrastCorrectionAmount", 0.5, 0.0, 1.0);
    m_dAutoBrightnessAmount = GetDouble("AutoBrightnessCorrectionAmount", 0.2, 0.0, 1.0);

    m_bDefaultMaximized = false;
    m_bStickyWindowSize = true;

    m_colorBackground = GetColor("BackgroundColor", QColor(0, 0, 0));
    m_colorTransparency = GetColor("TransparencyColor", QColor(255, 0, 255));

    m_bReloadWhenDisplayedImageChanged = GetBool("ReloadWhenDisplayedImageChanged", true);

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
    m_eAutoZoomMode = GetAutoZoomMode("AutoZoomMode", Helpers::ZM_FitToScreen);
    m_eAutoZoomModeFullscreen = GetAutoZoomMode("AutoZoomModeFullscreen", m_eAutoZoomMode);

    m_bHQRS = GetBool("HighQualityResampling", true);
    m_eSlideShowTransitionEffect = ConvertTransitionEffectFromString(GetString("SlideShowTransitionEffect", ""));

    // -------------------------------------------------------------------------------------------
    // [GF] Custom writable ini settings of this mod

    m_nBookModePageHeight = GetInt("BookModePageHeight", 122, 100, 1000);

    // -------------------------------------------------------------------------------------------
}

void CSettingsProvider::SaveCustomSettings() {
    WriteInt("BookModePageHeight", m_nBookModePageHeight);
}

void CSettingsProvider::SaveSettings(const CImageProcessingParams& procParams,
                                     EProcessingFlags eProcFlags,
                                     Helpers::ENavigationMode eNavigationMode,
                                     Helpers::ESorting eFileSorting,
                                     bool isSortedAscending,
                                     Helpers::EAutoZoomMode eAutoZoomMode,
                                     Helpers::EAutoZoomMode eAutoZoomModeFullScreen,
                                     Helpers::ETransitionEffect eSlideShowTransitionEffect) {

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

    WriteString("SlideShowTransitionEffect", ConvertTransitionEffectToString(eSlideShowTransitionEffect));

    ReadWriteableINISettings();
}

int CSettingsProvider::LastSettingsPage() const {
    if (m_stateFileFullPath.isEmpty()) {
        return 0;
    }
    QSettings stateSettings(m_stateFileFullPath, QSettings::IniFormat);
    return stateSettings.value(QStringLiteral("State/SettingsPage"), 0).toInt();
}

void CSettingsProvider::SaveLastSettingsPage(int nPage) {
    if (m_stateFileFullPath.isEmpty() || nPage < 0) {
        return;
    }
    QSettings stateSettings(m_stateFileFullPath, QSettings::IniFormat);
    stateSettings.setValue(QStringLiteral("State/SettingsPage"), nPage);
}

void CSettingsProvider::SaveStickyWindowRect(QRect rect) {
    qDebug() << "SaveStickyWindowRect() rect:" << rect << "to:" << m_stateFileFullPath;
    if (m_stateFileFullPath.isEmpty() || rect.isEmpty() || m_stickyWindowRect == rect) {
        return;
    }

    const QString sRect = QString("%1 %2 %3 %4")
                              .arg(rect.left())
                              .arg(rect.top())
                              .arg(rect.right())
                              .arg(rect.bottom());

    QSettings stateSettings(m_stateFileFullPath, QSettings::IniFormat);
    stateSettings.beginGroup("State");
    stateSettings.setValue("StickyWindowRect", sRect);
    stateSettings.endGroup();
    stateSettings.sync();

    m_stickyWindowRect = rect;
}

QString CSettingsProvider::GetString(const QString& sKey, const QString& sDefault) const {
    if (QFileInfo::exists(m_settingsFileFullPath)) {
        QSettings userSettings(m_settingsFileFullPath, QSettings::IniFormat);
        userSettings.beginGroup("Settings");
        if (userSettings.contains(sKey)) {
            return userSettings.value(sKey).toString();
        }
        userSettings.endGroup();
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
    // All values the settings dialog offers (GetAutoZoomModeString()); a missing key or an unknown value gives the default
    const QString sAutoZoomMode = GetString(sKey, GetAutoZoomModeString(defaultZoomMode));
    static const Helpers::EAutoZoomMode modes[] = { Helpers::ZM_FitToScreen, Helpers::ZM_FitToScreenNoZoom, Helpers::ZM_FillScreen,
                                                    Helpers::ZM_FillScreenNoZoom, Helpers::ZM_BookMode, Helpers::ZM_None };
    for (Helpers::EAutoZoomMode eMode : modes) {
        if (sAutoZoomMode.compare(GetAutoZoomModeString(eMode), Qt::CaseInsensitive) == 0) {
            return eMode;
        }
    }
    return defaultZoomMode;
}

void CSettingsProvider::WriteString(const QString& sKey, const QString& sString) {
    QSettings settings(m_settingsFileFullPath, QSettings::IniFormat);
    settings.beginGroup("Settings");
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

#include "Commands.h"

#include <QtGlobal>   // nur fuer QT_TRANSLATE_NOOP (expandiert zum blossen String-Literal)

#include <cassert>
#include <charconv>

// Markiert einen Anzeigetext fuer lupdate. Die Uebersetzung geschieht erst bei der Anzeige:
//     QCoreApplication::translate("Command", info.displayText)
#define CMD_TR(text) QT_TRANSLATE_NOOP("Command", text)

namespace Cmd {
namespace {

// ---------------------------------------------------------------------------------------------
// Kleine constexpr-Helfer
// ---------------------------------------------------------------------------------------------
constexpr char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

constexpr bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

template <class E>
constexpr int V(E e) { return static_cast<int>(e); }

constexpr CommandInfo make(Command id, std::string_view name, Category cat, const char* displayText) {
    CommandInfo c;
    c.id = id;
    c.name = name;
    c.category = cat;
    c.displayText = displayText;
    return c;
}

// ---------------------------------------------------------------------------------------------
// Benannte Argumentwerte
// ---------------------------------------------------------------------------------------------
constexpr ArgChoice kAutoZoomChoices[] = {
    { "None",       V(AutoZoomArg::None) },
    { "FitNoZoom",  V(AutoZoomArg::FitNoZoom) },
    { "FillNoZoom", V(AutoZoomArg::FillNoZoom) },
    { "Fit",        V(AutoZoomArg::Fit) },
    { "Fill",       V(AutoZoomArg::Fill) },
    { "BookMode",   V(AutoZoomArg::BookMode) },
};

constexpr ArgChoice kLoopModeChoices[] = {
    { "Folder",      V(LoopModeArg::Folder) },
    { "Recursively", V(LoopModeArg::Recursively) },
    { "Siblings",    V(LoopModeArg::Siblings) },
};

constexpr ArgChoice kSortModeChoices[] = {
    { "Name",             V(SortModeArg::Name) },
    { "ModificationDate", V(SortModeArg::ModificationDate) },
    { "Size",             V(SortModeArg::Size) },
    { "Random",           V(SortModeArg::Random) },
};

constexpr ArgChoice kSortOrderChoices[] = {
    { "Ascending",  V(SortOrderArg::Ascending) },
    { "Descending", V(SortOrderArg::Descending) },
};

constexpr ArgChoice kTransitionEffectChoices[] = {
    { "None",            V(TransitionEffectArg::None) },
    { "Blend",           V(TransitionEffectArg::Blend) },
    { "SlideFromRight",  V(TransitionEffectArg::SlideFromRight) },
    { "SlideFromLeft",   V(TransitionEffectArg::SlideFromLeft) },
    { "SlideFromTop",    V(TransitionEffectArg::SlideFromTop) },
    { "SlideFromBottom", V(TransitionEffectArg::SlideFromBottom) },
    { "RollFromRight",   V(TransitionEffectArg::RollFromRight) },
    { "RollFromLeft",    V(TransitionEffectArg::RollFromLeft) },
    { "RollFromTop",     V(TransitionEffectArg::RollFromTop) },
    { "RollFromBottom",  V(TransitionEffectArg::RollFromBottom) },
    { "ScrollFromRight", V(TransitionEffectArg::ScrollFromRight) },
    { "ScrollFromLeft",  V(TransitionEffectArg::ScrollFromLeft) },
    { "ScrollFromTop",   V(TransitionEffectArg::ScrollFromTop) },
    { "ScrollFromBottom",V(TransitionEffectArg::ScrollFromBottom) },
};

constexpr ArgChoice kTransitionSpeedChoices[] = {
    { "VeryFast", V(TransitionSpeedArg::VeryFast) },
    { "Fast",     V(TransitionSpeedArg::Fast) },
    { "Normal",   V(TransitionSpeedArg::Normal) },
    { "Slow",     V(TransitionSpeedArg::Slow) },
    { "VerySlow", V(TransitionSpeedArg::VerySlow) },
};

// ---------------------------------------------------------------------------------------------
// DIE Tabelle.
//
// Reihenfolge == Reihenfolge im enum Command (per static_assert erzwungen, siehe unten).
// Vergisst man einen Eintrag oder haengt einen zu viel an, kompiliert das Programm nicht:
//   - zu wenige Initializer: Restliche Eintraege haben id == DoNothing -> indexMatchesEnum() schlaegt fehl
//   - zu viele Initializer:  Compilerfehler "too many initializers"
//
// Neuen Befehl hinzufuegen = 1. enum-Eintrag, 2. Zeile hier an gleicher Position, 3. case im switch.
// ---------------------------------------------------------------------------------------------
constexpr std::array<CommandInfo, kCommandCount> kTable = {{
    make(Command::DoNothing, "DoNothing", Category::Window, CMD_TR("Do nothing")).legacy("IDM_DONOTHING"),

    // Navigation
    make(Command::NextImage,     "NextImage",     Category::Navigation, CMD_TR("Next image")).legacy("IDM_NEXT"),
    make(Command::PrevImage,     "PrevImage",     Category::Navigation, CMD_TR("Previous image")).legacy("IDM_PREV"),
    make(Command::FirstImage,    "FirstImage",    Category::Navigation, CMD_TR("First image")).legacy("IDM_FIRST"),
    make(Command::LastImage,     "LastImage",     Category::Navigation, CMD_TR("Last image")).legacy("IDM_LAST"),
    make(Command::NextFile,      "NextFile",      Category::Navigation, CMD_TR("Next file (leave multi-frame container)")).legacy("IDM_NEXTFILE"),
    make(Command::PrevFile,      "PrevFile",      Category::Navigation, CMD_TR("Previous file (leave multi-frame container)")).legacy("IDM_PREVFILE"),
    make(Command::NextFrame,     "NextFrame",     Category::Navigation, CMD_TR("Next frame")).legacy("IDM_NEXTFRAME"),
    make(Command::PlayPause,     "PlayPause",     Category::Navigation, CMD_TR("Play/pause animation")).legacy("IDM_PLAYPAUSE"),
    make(Command::PlayPauseNext, "PlayPauseNext", Category::Navigation, CMD_TR("Play/pause animation, otherwise next image")).legacy("IDM_PLAYPAUSENEXT"),

    // Datei
    make(Command::Reload,            "Reload",            Category::File, CMD_TR("Reload image")).legacy("IDM_RELOAD"),
    make(Command::ReloadFull,        "ReloadFull",        Category::File, CMD_TR("Reload image and file list")).legacy("IDM_RELOAD_FULL"),
    make(Command::CopyScreen,        "CopyScreen",        Category::File, CMD_TR("Copy image to clipboard (screen size)")).legacy("IDM_COPY"),
    make(Command::CopyFull,          "CopyFull",          Category::File, CMD_TR("Copy original image content to clipboard")).legacy("IDM_COPY_FULL"),
    make(Command::CopyPath,          "CopyPath",          Category::File, CMD_TR("Copy file path to clipboard")).legacy("IDM_COPY_PATH"),
    make(Command::CutFile,           "CutFile",           Category::File, CMD_TR("Cut file to clipboard")).legacy("IDM_CUT_FILE"),
    make(Command::DeleteFile,        "DeleteFile",        Category::File, CMD_TR("Delete file")).legacy("IDM_DELETE_FILE"),
    make(Command::RenameFile,        "RenameFile",        Category::File, CMD_TR("Rename file")).legacy("IDM_RENAME"),
    make(Command::ExploreFile,       "ExploreFile",       Category::File, CMD_TR("Show in folder")).legacy("IDM_EXPLORE"),
    make(Command::OpenDefaultEditor, "OpenDefaultEditor", Category::File, CMD_TR("Edit image in default editor")).legacy("IDM_OPEN_DEFAULT_EDITOR"),

    // Zoom / Ansicht
    make(Command::ZoomIn,  "ZoomIn",  Category::Zoom, CMD_TR("Zoom in")).legacy("IDM_ZOOM_INC"),
    make(Command::ZoomOut, "ZoomOut", Category::Zoom, CMD_TR("Zoom out")).legacy("IDM_ZOOM_DEC"),
    // Ersetzt IDM_ZOOM_400/200/100/50/25 (siehe kLegacyAliases). 100 = "echte Groesse" wie bisher.
    make(Command::ZoomTo, "ZoomTo", Category::Zoom, CMD_TR("Zoom to (percent)")).intArg(1, 6400, 100),
    make(Command::FitToScreen,          "FitToScreen",          Category::Zoom, CMD_TR("Fit to window")).legacy("IDM_FIT_TO_SCREEN"),
    make(Command::FitToScreenNoEnlarge, "FitToScreenNoEnlarge", Category::Zoom, CMD_TR("Fit to window (never upsize)")).legacy("IDM_FIT_TO_SCREEN_NO_ENLARGE"),
    make(Command::FillWithCrop,         "FillWithCrop",         Category::Zoom, CMD_TR("Fill with crop")).legacy("IDM_FILL_WITH_CROP"),
    make(Command::ToggleFitTo100,       "ToggleFitTo100",       Category::Zoom, CMD_TR("Toggle fit to window / 100 %")).legacy("IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS"),
    make(Command::ToggleFillWithCropTo100, "ToggleFillWithCropTo100", Category::Zoom, CMD_TR("Toggle fill with crop / 100 %")).legacy("IDM_TOGGLE_FILL_WITH_CROP_100_PERCENTS"),
    make(Command::ToggleAutoZoomFit,    "ToggleAutoZoomFit",    Category::Zoom, CMD_TR("Toggle auto zoom mode (fit / none / book)")).legacy("IDM_TOGGLE_AUTO_ZOOM_FIT"),
    make(Command::SetAutoZoom,          "SetAutoZoom",          Category::Zoom, CMD_TR("Auto zoom mode")).choiceArg(kAutoZoomChoices, V(AutoZoomArg::Fit)),
    make(Command::ZoomMode,             "ZoomMode",             Category::Zoom, CMD_TR("Toggle zoom mode on left mouse button")).legacy("IDM_ZOOM_MODE").menuOnly(),
    make(Command::PanUp,    "PanUp",    Category::View, CMD_TR("Pan up")).legacy("IDM_PAN_UP"),
    make(Command::PanDown,  "PanDown",  Category::View, CMD_TR("Pan down")).legacy("IDM_PAN_DOWN"),
    make(Command::PanLeft,  "PanLeft",  Category::View, CMD_TR("Pan left")).legacy("IDM_PAN_LEFT"),
    make(Command::PanRight, "PanRight", Category::View, CMD_TR("Pan right")).legacy("IDM_PAN_RIGHT"),
    make(Command::Rotate90,  "Rotate90",  Category::View, CMD_TR("Rotate +90")).legacy("IDM_ROTATE_90"),
    make(Command::Rotate270, "Rotate270", Category::View, CMD_TR("Rotate -90")).legacy("IDM_ROTATE_270"),
    make(Command::MirrorH,   "MirrorH",   Category::View, CMD_TR("Mirror horizontally")).legacy("IDM_MIRROR_H"),
    make(Command::MirrorV,   "MirrorV",   Category::View, CMD_TR("Mirror vertically")).legacy("IDM_MIRROR_V"),

    // Bildverarbeitung / Anzeige-Schalter
    make(Command::ToggleAutoCorrection,  "ToggleAutoCorrection",  Category::View, CMD_TR("Auto contrast")).legacy("IDM_AUTO_CORRECTION"),
    make(Command::KeepParameters,        "KeepParameters",        Category::View, CMD_TR("Keep parameters between images")).legacy("IDM_KEEP_PARAMETERS"),
    make(Command::ContrastCorrectionInc, "ContrastCorrectionInc", Category::View, CMD_TR("Increase contrast correction")).legacy("IDM_CONTRAST_CORRECTION_INC"),
    make(Command::ContrastCorrectionDec, "ContrastCorrectionDec", Category::View, CMD_TR("Decrease contrast correction")).legacy("IDM_CONTRAST_CORRECTION_DEC"),
    make(Command::ToggleBWMode,          "ToggleBWMode",          Category::View, CMD_TR("Toggle black and white mode")),
    make(Command::ToggleTransparency,    "ToggleTransparency",    Category::View, CMD_TR("Toggle transparency mode")).legacy("IDM_TOGGLE_TRANSPARENCY"),
    make(Command::ToggleEmbeddedColorProfiles, "ToggleEmbeddedColorProfiles", Category::View, CMD_TR("Toggle embedded color profiles")).legacy("IDM_TOGGLE_USE_EMBEDDED_COLOR_PROFILES"),
    make(Command::ToggleResamplingQuality,     "ToggleResamplingQuality",     Category::View, CMD_TR("Toggle resampling quality")).legacy("IDM_TOGGLE_RESAMPLING_QUALITY"),
    make(Command::ToggleInfoOverlay,     "ToggleInfoOverlay",     Category::View, CMD_TR("Toggle info overlay")).legacy("IDM_TOGGLE_INFO_OVERLAY"),

    // Slideshow / Animation
    // StartSlideshow ersetzt IDM_SLIDESHOW_1..20, PlayMovie ersetzt IDM_MOVIE_5_FPS..100_FPS.
    make(Command::StartSlideshow,  "StartSlideshow",  Category::Slideshow, CMD_TR("Start slideshow (seconds per image)")).intArg(1, 3600, 5),
    make(Command::ResumeSlideshow, "ResumeSlideshow", Category::Slideshow, CMD_TR("Resume slideshow")).legacy("IDM_SLIDESHOW_RESUME"),
    make(Command::StopMovie,       "StopMovie",       Category::Slideshow, CMD_TR("Pause slideshow/animation")).legacy("IDM_STOP_MOVIE").menuOnly(),
    make(Command::PlayMovie,       "PlayMovie",       Category::Slideshow, CMD_TR("Play folder as animation (frames per second)")).intArg(1, 200, 25),
    make(Command::SetTransitionEffect, "SetTransitionEffect", Category::Slideshow, CMD_TR("Transition effect")).choiceArg(kTransitionEffectChoices, V(TransitionEffectArg::None)).menuOnly(),
    make(Command::SetTransitionSpeed,  "SetTransitionSpeed",  Category::Slideshow, CMD_TR("Transition speed")).choiceArg(kTransitionSpeedChoices, V(TransitionSpeedArg::Normal)).menuOnly(),

    // Ordner / Sortierung
    make(Command::SetLoopMode,  "SetLoopMode",  Category::Sorting, CMD_TR("Folder mode")).choiceArg(kLoopModeChoices, V(LoopModeArg::Folder)),
    make(Command::SetSortMode,  "SetSortMode",  Category::Sorting, CMD_TR("Sort order")).choiceArg(kSortModeChoices, V(SortModeArg::Name)),
    make(Command::SetSortOrder, "SetSortOrder", Category::Sorting, CMD_TR("Sort direction")).choiceArg(kSortOrderChoices, V(SortOrderArg::Ascending)),
    make(Command::ToggleSortRandom,    "ToggleSortRandom",    Category::Sorting, CMD_TR("Toggle between random and previous sort order")).legacy("IDM_TOGGLE_SORT_RANDOM"),
    make(Command::ChangeFolderLanguage,"ChangeFolderLanguage",Category::Sorting, CMD_TR("Jump to folder of another language")).legacy("IDM_CHANGE_FOLDER_LANGUAGE"),

    // Fenster / Anwendung
    make(Command::FullScreenMode, "FullScreenMode", Category::Window, CMD_TR("Full screen mode")).legacy("IDM_FULL_SCREEN_MODE"),
    make(Command::Minimize,       "Minimize",       Category::Window, CMD_TR("Minimize window")).legacy("IDM_MINIMIZE"),
    make(Command::ContextMenu,    "ContextMenu",    Category::Window, CMD_TR("Show context menu")).legacy("IDM_CONTEXT_MENU"),
    make(Command::SettingsDialog, "SettingsDialog", Category::Window, CMD_TR("Settings")).legacy("IDM_SETTINGS_DIALOG"),
    make(Command::Help,           "Help",           Category::Window, CMD_TR("Help")).legacy("IDM_HELP"),
    make(Command::DefaultEscape,  "DefaultEscape",  Category::Window, CMD_TR("Escape (stop slideshow, close overlay, leave full screen, exit)")).legacy("IDM_DEFAULT_ESC"),
    make(Command::Exit,           "Exit",           Category::Window, CMD_TR("Exit")).legacy("IDM_EXIT"),
}};

// ---------------------------------------------------------------------------------------------
// Alte Namen parametrisierter Befehle. Damit funktioniert eine vorhandene keymap.txt unveraendert
// weiter, ohne dass symbols.km oder die alten IDs noch gebraucht werden.
// ---------------------------------------------------------------------------------------------
struct LegacyAlias {
    std::string_view name;
    Command cmd;
    int arg;
};

constexpr LegacyAlias kLegacyAliases[] = {
    { "IDM_SLIDESHOW_1",  Command::StartSlideshow, 1 },
    { "IDM_SLIDESHOW_2",  Command::StartSlideshow, 2 },
    { "IDM_SLIDESHOW_3",  Command::StartSlideshow, 3 },
    { "IDM_SLIDESHOW_4",  Command::StartSlideshow, 4 },
    { "IDM_SLIDESHOW_5",  Command::StartSlideshow, 5 },
    { "IDM_SLIDESHOW_6",  Command::StartSlideshow, 6 },
    { "IDM_SLIDESHOW_7",  Command::StartSlideshow, 7 },
    { "IDM_SLIDESHOW_8",  Command::StartSlideshow, 8 },
    { "IDM_SLIDESHOW_9",  Command::StartSlideshow, 9 },
    { "IDM_SLIDESHOW_10", Command::StartSlideshow, 10 },
    { "IDM_SLIDESHOW_20", Command::StartSlideshow, 20 },

    { "IDM_MOVIE_5_FPS",   Command::PlayMovie, 5 },
    { "IDM_MOVIE_10_FPS",  Command::PlayMovie, 10 },
    { "IDM_MOVIE_15_FPS",  Command::PlayMovie, 15 },
    { "IDM_MOVIE_20_FPS",  Command::PlayMovie, 20 },
    { "IDM_MOVIE_25_FPS",  Command::PlayMovie, 25 },
    { "IDM_MOVIE_30_FPS",  Command::PlayMovie, 30 },
    { "IDM_MOVIE_50_FPS",  Command::PlayMovie, 50 },
    { "IDM_MOVIE_60_FPS",  Command::PlayMovie, 60 },
    { "IDM_MOVIE_100_FPS", Command::PlayMovie, 100 },

    { "IDM_ZOOM_400", Command::ZoomTo, 400 },
    { "IDM_ZOOM_200", Command::ZoomTo, 200 },
    { "IDM_ZOOM_100", Command::ZoomTo, 100 },
    { "IDM_ZOOM_50",  Command::ZoomTo, 50 },
    { "IDM_ZOOM_25",  Command::ZoomTo, 25 },

    { "IDM_AUTO_ZOOM_NONE",           Command::SetAutoZoom, V(AutoZoomArg::None) },
    { "IDM_AUTO_ZOOM_FIT_NO_ZOOM",    Command::SetAutoZoom, V(AutoZoomArg::FitNoZoom) },
    { "IDM_AUTO_ZOOM_FILL_NO_ZOOM",   Command::SetAutoZoom, V(AutoZoomArg::FillNoZoom) },
    { "IDM_AUTO_ZOOM_FIT",            Command::SetAutoZoom, V(AutoZoomArg::Fit) },
    { "IDM_AUTO_ZOOM_FILL",           Command::SetAutoZoom, V(AutoZoomArg::Fill) },
    { "IDM_AUTO_ZOOM_BOOK_MODE",      Command::SetAutoZoom, V(AutoZoomArg::BookMode) },

    { "IDM_LOOP_FOLDER",      Command::SetLoopMode, V(LoopModeArg::Folder) },
    { "IDM_LOOP_RECURSIVELY", Command::SetLoopMode, V(LoopModeArg::Recursively) },
    { "IDM_LOOP_SIBLINGS",    Command::SetLoopMode, V(LoopModeArg::Siblings) },

    { "IDM_SORT_NAME",     Command::SetSortMode, V(SortModeArg::Name) },
    { "IDM_SORT_MOD_DATE", Command::SetSortMode, V(SortModeArg::ModificationDate) },
    { "IDM_SORT_SIZE",     Command::SetSortMode, V(SortModeArg::Size) },
    { "IDM_SORT_RANDOM",   Command::SetSortMode, V(SortModeArg::Random) },

    { "IDM_SORT_ASCENDING",  Command::SetSortOrder, V(SortOrderArg::Ascending) },
    { "IDM_SORT_DESCENDING", Command::SetSortOrder, V(SortOrderArg::Descending) },
};

// ---------------------------------------------------------------------------------------------
// Standardbelegung (== bisherige keymap.txt, ohne die zwei toten Zeilen IDM_MARK_FOR_TOGGLE und
// IDM_TOGGLE). Zusaetzlich Esc und F1, die bisher fest im Code verdrahtet
// waren (AddDefaultEscapeHandling bzw. keyPressEvent).
// ---------------------------------------------------------------------------------------------
constexpr DefaultBinding kDefaultBindings[] = {
    { "PgUp",  Command::PrevFile, 0 },
    { "PgDn",  Command::NextFile, 0 },

    { "Up",    Command::PanUp,    0 },
    { "Down",  Command::PanDown,  0 },
    { "Left",  Command::PanLeft,  0 },
    { "Right", Command::PanRight, 0 },

    { "Back",  Command::PrevImage, 0 },
    { "Space", Command::NextImage, 0 },

    { "Home",  Command::FirstImage, 0 },
    { "End",   Command::LastImage,  0 },

    { "Plus",      Command::ZoomIn,     0 },
    { "Minus",     Command::ZoomOut,    0 },
    { "Del",       Command::DeleteFile, 0 },
    { "Shift+Del", Command::DeleteFile, 0 },
    { "F2",        Command::RenameFile, 0 },

    { "F1",     Command::Help,                    0 },
    { "F3",     Command::ToggleResamplingQuality, 0 },
    { "F4",     Command::ToggleAutoCorrection,    0 },
    { "F5",     Command::ReloadFull,              0 },
    { "F6",     Command::ToggleBWMode,            0 },
    { "F11",    Command::FullScreenMode,          0 },
    { "Return", Command::FullScreenMode,          0 },
    { "Esc",    Command::DefaultEscape,           0 },

    { "Ctrl+C",       Command::CopyFull,          0 },
    { "Ctrl+Shift+C", Command::CopyPath,          0 },
    { "Ctrl+E",       Command::OpenDefaultEditor, 0 },
    { "Ctrl+O",       Command::SettingsDialog,    0 },
    { "Ctrl+L",       Command::ExploreFile,       0 },
    { "Ctrl+M",       Command::ContextMenu,       0 },
    { "Ctrl+X",       Command::CutFile,           0 },

    { "1", Command::ZoomTo, 100 },
    { "2", Command::ZoomTo, 50 },
    { "3", Command::ZoomTo, 25 },

    { "A", Command::ToggleTransparency,          0 },
    { "E", Command::ToggleEmbeddedColorProfiles, 0 },
    { "F", Command::ToggleAutoZoomFit,           0 },
    { "H", Command::MirrorH,                     0 },
    { "I", Command::ToggleInfoOverlay,           0 },
    { "L", Command::Rotate270,                   0 },
    { "M", Command::Minimize,                    0 },
    { "R", Command::Rotate90,                    0 },
    { "S", Command::ResumeSlideshow,             0 },
    { "V", Command::MirrorV,                     0 },
    { "Z", Command::ToggleSortRandom,            0 },

    { "MouseL",      Command::PrevImage,      0 },
    { "MouseDblClk", Command::PrevImage,      0 },
    { "MouseR",      Command::NextImage,      0 },
    { "MouseM",      Command::FullScreenMode, 0 },
    { "MouseX1",     Command::ContextMenu,    0 },

    { "Numpad0", Command::PlayPause, 0 },
    { "Numpad1", Command::PlayMovie, 5 },
    { "Numpad2", Command::PlayMovie, 10 },
    { "Numpad3", Command::PlayMovie, 15 },
    { "Numpad4", Command::PlayMovie, 20 },
    { "Numpad5", Command::PlayMovie, 25 },
    { "Numpad6", Command::PlayMovie, 30 },
    { "Numpad7", Command::PlayMovie, 50 },
    { "Numpad8", Command::PlayMovie, 60 },
    { "Numpad9", Command::PlayMovie, 100 },
};

// ---------------------------------------------------------------------------------------------
// Compile-Zeit-Pruefungen
// ---------------------------------------------------------------------------------------------
constexpr bool argValid(const CommandInfo& ci, int arg) {
    switch (ci.argKind) {
    case ArgKind::None:
        return arg == 0;
    case ArgKind::Int:
        return arg >= ci.argMin && arg <= ci.argMax;
    case ArgKind::Choice:
        for (std::size_t i = 0; i < ci.choiceCount; ++i) {
            if (ci.choices[i].value == arg) return true;
        }
        return false;
    }
    return false;
}

// Tabellenposition == Enum-Wert. Das ist die Eigenschaft, die info(Command) O(1) macht.
constexpr bool indexMatchesEnum() {
    for (std::size_t i = 0; i < kTable.size(); ++i) {
        if (static_cast<std::size_t>(kTable[i].id) != i) return false;
    }
    return true;
}

constexpr std::size_t nameOccurrences(std::string_view n) {
    std::size_t count = 0;
    for (const auto& ci : kTable) {
        if (equalsIgnoreCase(ci.name, n)) ++count;
        if (!ci.legacyName.empty() && equalsIgnoreCase(ci.legacyName, n)) ++count;
    }
    for (const auto& a : kLegacyAliases) {
        if (equalsIgnoreCase(a.name, n)) ++count;
    }
    return count;
}

// Jeder Name (kanonisch, alt, Alias) darf genau einmal vorkommen, ohne Beachtung der Gross-/Kleinschreibung.
constexpr bool namesAreUnique() {
    for (const auto& ci : kTable) {
        if (ci.name.empty()) return false;
        if (nameOccurrences(ci.name) != 1) return false;
        if (!ci.legacyName.empty() && nameOccurrences(ci.legacyName) != 1) return false;
    }
    for (const auto& a : kLegacyAliases) {
        if (nameOccurrences(a.name) != 1) return false;
    }
    return true;
}

constexpr bool argSpecsAreValid() {
    for (const auto& ci : kTable) {
        switch (ci.argKind) {
        case ArgKind::None:
            if (ci.argDefault != 0 || ci.choices != nullptr) return false;
            break;
        case ArgKind::Int:
            if (ci.argMin > ci.argMax || ci.argDefault < ci.argMin || ci.argDefault > ci.argMax) return false;
            break;
        case ArgKind::Choice:
            if (ci.choices == nullptr || ci.choiceCount == 0 || !argValid(ci, ci.argDefault)) return false;
            for (std::size_t i = 0; i < ci.choiceCount; ++i) {
                if (ci.choices[i].name.empty()) return false;
                for (std::size_t j = 0; j < i; ++j) {
                    if (equalsIgnoreCase(ci.choices[i].name, ci.choices[j].name) || ci.choices[i].value == ci.choices[j].value) return false;
                }
            }
            break;
        }
    }
    return true;
}

constexpr bool aliasesAreValid() {
    for (const auto& a : kLegacyAliases) {
        if (static_cast<std::size_t>(a.cmd) >= kCommandCount) return false;
        if (!argValid(kTable[static_cast<std::size_t>(a.cmd)], a.arg)) return false;
    }
    return true;
}

constexpr bool defaultBindingsAreValid() {
    for (const auto& b : kDefaultBindings) {
        if (b.keys.empty()) return false;
        if (static_cast<std::size_t>(b.cmd) >= kCommandCount) return false;
        const CommandInfo& ci = kTable[static_cast<std::size_t>(b.cmd)];
        if (!ci.bindable) return false;                 // Standardbelegung nur fuer per Hotkey belegbare Befehle
        if (!argValid(ci, b.arg)) return false;
    }
    return true;
}

static_assert(kCommandCount > 0, "Command::Count must be the last enumerator");
static_assert(indexMatchesEnum(),
              "kTable is out of sync with enum Command: same order, one entry per enumerator");
static_assert(namesAreUnique(),
              "duplicate or empty command name in kTable / kLegacyAliases (names are case-insensitive)");
static_assert(argSpecsAreValid(),
              "invalid argument spec in kTable (default outside range / not in choices / duplicate choice)");
static_assert(aliasesAreValid(),
              "kLegacyAliases contains an argument outside the range of its command");
static_assert(defaultBindingsAreValid(),
              "kDefaultBindings references a non-bindable command or an invalid argument");

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Zugriff
// ---------------------------------------------------------------------------------------------
const std::array<CommandInfo, kCommandCount>& allCommands() {
    return kTable;
}

const CommandInfo& info(Command id) {
    const std::size_t i = static_cast<std::size_t>(id);
    assert(i < kCommandCount);
    return kTable[i];
}

ArrayView<DefaultBinding> defaultBindings() {
    return { kDefaultBindings, sizeof(kDefaultBindings) / sizeof(kDefaultBindings[0]) };
}

bool isValidArg(const CommandInfo& ci, int arg) {
    return argValid(ci, arg);
}

// Linear ueber ~70 Eintraege. Wird nur beim Laden der keymap benutzt; eine Hash-Map waere hier
// unnoetige Komplexitaet.
const CommandInfo* findByName(std::string_view name) {
    for (const auto& ci : kTable) {
        if (equalsIgnoreCase(ci.name, name)) return &ci;
        if (!ci.legacyName.empty() && equalsIgnoreCase(ci.legacyName, name)) return &ci;
    }
    return nullptr;
}

std::optional<CommandInvocation> parseInvocation(std::string_view text, std::string* error) {
    auto fail = [error](std::string msg) -> std::optional<CommandInvocation> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };

    text = trim(text);
    if (text.empty()) return fail("empty command");

    // Zerlegen in Name und Argument: "PlayMovie 25", "PlayMovie(25)", "PlayMovie (25)"
    const std::size_t nameEnd = text.find_first_of(" \t(");
    const std::string_view name = text.substr(0, nameEnd);
    std::string_view rest = (nameEnd == std::string_view::npos) ? std::string_view() : trim(text.substr(nameEnd));
    if (!rest.empty() && rest.front() == '(') {
        if (rest.size() < 2 || rest.back() != ')') return fail("missing ')' after argument");
        rest = trim(rest.substr(1, rest.size() - 2));
    }

    // 1. Alte Namen parametrisierter Befehle
    for (const auto& a : kLegacyAliases) {
        if (equalsIgnoreCase(a.name, name)) {
            if (!rest.empty()) return fail("legacy name '" + std::string(name) + "' takes no argument");
            return CommandInvocation{ a.cmd, a.arg };
        }
    }

    // 2. Kanonischer oder alter Name
    const CommandInfo* ci = findByName(name);
    if (!ci) return fail("unknown command '" + std::string(name) + "'");

    CommandInvocation inv{ ci->id, ci->argDefault };

    switch (ci->argKind) {
    case ArgKind::None:
        if (!rest.empty()) return fail("command '" + std::string(ci->name) + "' takes no argument");
        break;

    case ArgKind::Int:
        if (rest.empty()) break;   // Standardwert
        {
            int value = 0;
            const auto res = std::from_chars(rest.data(), rest.data() + rest.size(), value);
            if (res.ec != std::errc() || res.ptr != rest.data() + rest.size()) {
                return fail("argument '" + std::string(rest) + "' of '" + std::string(ci->name) + "' is not a number");
            }
            if (!argValid(*ci, value)) {
                return fail("argument " + std::to_string(value) + " of '" + std::string(ci->name) + "' is out of range ("
                            + std::to_string(ci->argMin) + ".." + std::to_string(ci->argMax) + ")");
            }
            inv.arg = value;
        }
        break;

    case ArgKind::Choice:
        if (rest.empty()) break;   // Standardwert
        {
            bool found = false;
            for (std::size_t i = 0; i < ci->choiceCount; ++i) {
                if (equalsIgnoreCase(ci->choices[i].name, rest)) {
                    inv.arg = ci->choices[i].value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::string valid;
                for (std::size_t i = 0; i < ci->choiceCount; ++i) {
                    if (i) valid += ", ";
                    valid += std::string(ci->choices[i].name);
                }
                return fail("unknown argument '" + std::string(rest) + "' for '" + std::string(ci->name) + "' (valid: " + valid + ")");
            }
        }
        break;
    }

    return inv;
}

std::string defaultKeyMapFileText() {
    static constexpr const char* kCategoryNames[] = {
        "Navigation", "File", "Zoom", "View", "Slideshow", "Sorting", "Window"
    };
    static_assert(sizeof(kCategoryNames) / sizeof(kCategoryNames[0]) == 7, "update kCategoryNames when adding a Category");

    std::string out;
    out +=
        "// Key mapping for mkImageViewer\n"
        "//\n"
        "// One binding per line:   <keys>   <command> [argument]\n"
        "//   <keys>      Key names joined with '+', e.g.  Ctrl+Shift+C   F5   Space   Plus\n"
        "//               Modifiers: Alt, Ctrl, Shift          (use 'Plus' for the + key)\n"
        "//               Numeric keypad: Numpad0 .. Numpad9\n"
        "//               Mouse: MouseL, MouseR, MouseM, MouseX1, MouseX2, MouseDblClk\n"
        "//   <command>   Name from the list at the end of this file, optionally with an argument:\n"
        "//                 PlayMovie 25      SetSortMode Size      StartSlideshow 5\n"
        "//               Old names (IDM_NEXT, IDM_MOVIE_25_FPS, ...) are still understood.\n"
        "//\n"
        "// This file replaces the built-in defaults completely. To switch off a key, map it to DoNothing.\n"
        "// Lines that cannot be understood are skipped and reported in the log.\n"
        "// Delete this file to get the defaults back (it is written again on the next start).\n";

    for (std::size_t c = 0; c < sizeof(kCategoryNames) / sizeof(kCategoryNames[0]); ++c) {
        bool headerWritten = false;
        for (const auto& b : kDefaultBindings) {
            if (static_cast<std::size_t>(info(b.cmd).category) != c) continue;
            if (!headerWritten) {
                out += "\n// ";
                out += kCategoryNames[c];
                out += '\n';
                headerWritten = true;
            }
            std::string line(b.keys);
            if (line.size() < 24) line.append(24 - line.size(), ' '); else line += "  ";
            line += formatInvocation({ b.cmd, b.arg });
            out += line;
            out += '\n';
        }
    }

    out += "\n// Available commands (argument in <...>, default value first where it matters)\n";
    for (const auto& ci : kTable) {
        if (!ci.bindable) continue;
        std::string name(ci.name);
        switch (ci.argKind) {
        case ArgKind::None:
            break;
        case ArgKind::Int:
            name += " <" + std::to_string(ci.argMin) + ".." + std::to_string(ci.argMax) + ">";
            break;
        case ArgKind::Choice: {
            name += " <";
            for (std::size_t i = 0; i < ci.choiceCount; ++i) {
                if (i) name += '|';
                name += std::string(ci.choices[i].name);
            }
            name += '>';
            break;
        }
        }
        std::string line = "//   " + name;
        if (line.size() < 56) line.append(56 - line.size(), ' '); else line += "  ";
        line += ci.displayText;
        out += line;
        out += '\n';
    }
    return out;
}

std::string formatInvocation(const CommandInvocation& inv) {
    const CommandInfo& ci = info(inv.id);
    std::string out(ci.name);

    switch (ci.argKind) {
    case ArgKind::None:
        break;
    case ArgKind::Int:
        out += ' ';
        out += std::to_string(inv.arg);
        break;
    case ArgKind::Choice:
        out += ' ';
        for (std::size_t i = 0; i < ci.choiceCount; ++i) {
            if (ci.choices[i].value == inv.arg) {
                out += std::string(ci.choices[i].name);
                return out;
            }
        }
        out += std::to_string(inv.arg);   // unbekannter Wert: Zahl ausgeben statt zu verlieren
        break;
    }
    return out;
}

} // namespace Cmd

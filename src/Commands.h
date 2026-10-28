#ifndef COMMANDS_H
#define COMMANDS_H

// ---------------------------------------------------------------------------------------------
// Commands.h - zentrale Beschreibung aller Befehle der Anwendung
//
// Ersetzt:  die IDM_*-Defines in resource.h, die Datei symbols.km und die Arithmetik auf
//           Command-IDs in MainDlg.cpp (nCommand - IDM_SLIDESHOW_START, ... * 10 + OFFSET).
//
// Idee: EINE Quelle der Wahrheit.
//   - enum class Command            -> Identitaet, typsicher, im switch verwendbar
//   - CommandInfo-Tabelle (.cpp)    -> Config-Name, Anzeigetext, Kategorie, Argument-Typ
//   - DefaultBinding-Tabelle (.cpp) -> Standard-Tastenbelegung (ersetzt die mitgelieferte keymap.txt)
//   - static_asserts                -> Tabelle und Enum koennen nicht mehr auseinanderlaufen
//
// Header und Logik sind bewusst Qt-frei (nur std). Einzige Qt-Abhaengigkeit ist <QtGlobal> in
// Commands.cpp fuer QT_TRANSLATE_NOOP. Dadurch ist die Tabelle constexpr und ausserhalb von Qt
// testbar. Die Uebersetzung selbst passiert erst bei der Anzeige:
//     QCoreApplication::translate("Command", info.displayText)
//
// Der Code ist C++17-kompatibel.
// ---------------------------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Cmd {

// ---------------------------------------------------------------------------------------------
// Befehle
//
// Parametrisierte Befehle (StartSlideshow, PlayMovie, ZoomTo, SetAutoZoom, ...) ersetzen ganze
// Familien von IDs. Statt 11 IDs fuer Slideshow-Zeiten gibt es EINEN Befehl mit Argument.
// Die Reihenfolge im Enum ist beliebig, MUSS aber mit der Reihenfolge der Tabelle in Commands.cpp
// uebereinstimmen (wird per static_assert geprueft). Count bleibt immer der letzte Eintrag.
// ---------------------------------------------------------------------------------------------
enum class Command : std::uint16_t {
    DoNothing = 0,            // "Taste belegt, tut aber nichts" (ersetzt IDM_DONOTHING, siehe KeyMap)

    // Navigation
    NextImage,
    PrevImage,
    FirstImage,
    LastImage,
    NextFile,
    PrevFile,
    NextFrame,
    PlayPause,
    PlayPauseNext,

    // Datei
    Reload,
    ReloadFull,
    CopyScreen,
    CopyFull,
    CopyPath,
    CutFile,
    DeleteFile,
    RenameFile,               // noch ohne Funktion im Hauptfenster (kommt spaeter)
    ExploreFile,
    OpenDefaultEditor,

    // Zoom / Ansicht
    ZoomIn,
    ZoomOut,
    ZoomTo,                   // Argument: Prozent
    FitToScreen,
    FitToScreenNoEnlarge,
    FillWithCrop,
    ToggleFitTo100,
    ToggleFillWithCropTo100,
    ToggleAutoZoomFit,
    SetAutoZoom,              // Argument: AutoZoomArg
    ZoomMode,
    PanUp,
    PanDown,
    PanLeft,
    PanRight,
    Rotate90,
    Rotate270,
    MirrorH,
    MirrorV,

    // Bildverarbeitung / Anzeige-Schalter
    ToggleAutoCorrection,
    KeepParameters,
    ContrastCorrectionInc,
    ContrastCorrectionDec,
    ColorCorrectionInc,
    ColorCorrectionDec,
    ToggleTransparency,
    ToggleEmbeddedColorProfiles,
    ToggleResamplingQuality,
    ToggleInfoOverlay,

    // Slideshow / Animation
    StartSlideshow,           // Argument: Sekunden
    ResumeSlideshow,
    StopMovie,
    PlayMovie,                // Argument: Bilder pro Sekunde
    SetTransitionEffect,      // Argument: TransitionEffectArg (nur Menue)
    SetTransitionSpeed,       // Argument: TransitionSpeedArg  (nur Menue)

    // Ordner / Sortierung
    SetLoopMode,              // Argument: LoopModeArg
    SetSortMode,              // Argument: SortModeArg
    SetSortOrder,             // Argument: SortOrderArg
    ToggleSortRandom,
    ChangeFolderLanguage,

    // Fenster / Anwendung
    FullScreenMode,
    Minimize,
    ContextMenu,
    SettingsDialog,
    Help,
    DefaultEscape,            // Esc: Slideshow/Animation stoppen, Overlay schliessen, Vollbild verlassen, beenden
    Exit,

    Count                     // IMMER letzter Eintrag
};

constexpr std::size_t kCommandCount = static_cast<std::size_t>(Command::Count);

enum class Category : std::uint8_t {
    Navigation,
    File,
    Zoom,
    View,
    Slideshow,
    Sorting,
    Window
};

// ---------------------------------------------------------------------------------------------
// Argumente
//
// Die Werte dieser Enums sind NUR Namen fuer das Argument. Sie sind absichtlich von den Enums im
// restlichen Programm (Helpers::EAutoZoomMode, Helpers::ESorting, ...) entkoppelt. MainDlg
// uebersetzt sie explizit (siehe Beispiel im Begleittext), dadurch haengt Commands.h an nichts.
// ---------------------------------------------------------------------------------------------
enum class AutoZoomArg   { None, FitNoZoom, FillNoZoom, Fit, Fill, BookMode };
enum class LoopModeArg   { Folder, Recursively, Siblings };
enum class SortModeArg   { Name, ModificationDate, Size, Random };
enum class SortOrderArg  { Ascending, Descending };
enum class TransitionEffectArg {
    None, Blend,
    SlideFromRight, SlideFromLeft, SlideFromTop, SlideFromBottom,
    RollFromRight, RollFromLeft, RollFromTop, RollFromBottom,
    ScrollFromRight, ScrollFromLeft, ScrollFromTop, ScrollFromBottom
};
enum class TransitionSpeedArg { VeryFast, Fast, Normal, Slow, VerySlow };

enum class ArgKind : std::uint8_t {
    None,     // Befehl ohne Argument
    Int,      // Ganzzahl im Bereich [argMin, argMax]
    Choice    // benanntes Argument aus einer festen Liste (z.B. "Fit", "Size")
};

struct ArgChoice {
    std::string_view name;
    int value;
};

// Aufruf eines Befehls inklusive Argument. Das ist der Typ, den KeyMap, Menue und ExecuteCommand
// kuenftig austauschen (statt eines nackten int).
struct CommandInvocation {
    Command id = Command::DoNothing;
    int arg = 0;

    friend constexpr bool operator==(const CommandInvocation& a, const CommandInvocation& b) {
        return a.id == b.id && a.arg == b.arg;
    }
    friend constexpr bool operator!=(const CommandInvocation& a, const CommandInvocation& b) {
        return !(a == b);
    }
};

// ---------------------------------------------------------------------------------------------
// Tabelleneintrag
//
// Aufbau per constexpr-"Builder" (siehe Commands.cpp), damit die Tabelle lesbar bleibt und keine
// C++20-Designated-Initializers noetig sind:
//     make(Command::PlayMovie, "PlayMovie", Category::Slideshow, CMD_TR("Play movie"))
//         .intArg(1, 200, 25)
// ---------------------------------------------------------------------------------------------
struct CommandInfo {
    Command id = Command::DoNothing;
    std::string_view name;            // kanonischer Name in keymap.txt, z.B. "NextImage" (Klein/Gross egal)
    std::string_view legacyName;      // alter Name (z.B. "IDM_NEXT"), wird beim Laden weiter akzeptiert; "" = keiner
    Category category = Category::Window;
    const char* displayText = "";     // Uebersetzungs-Schluessel (englischer Text), Kontext "Command"
    bool bindable = true;             // false = nur ueber Menue/intern erreichbar, nicht per Hotkey belegbar

    ArgKind argKind = ArgKind::None;
    int argMin = 0;
    int argMax = 0;
    int argDefault = 0;
    const ArgChoice* choices = nullptr;
    std::size_t choiceCount = 0;

    // --- Builder (alle liefern eine veraenderte Kopie) ---
    constexpr CommandInfo legacy(std::string_view n) const {
        CommandInfo c = *this; c.legacyName = n; return c;
    }
    constexpr CommandInfo menuOnly() const {
        CommandInfo c = *this; c.bindable = false; return c;
    }
    constexpr CommandInfo intArg(int mn, int mx, int def) const {
        CommandInfo c = *this; c.argKind = ArgKind::Int; c.argMin = mn; c.argMax = mx; c.argDefault = def; return c;
    }
    template <std::size_t N>
    constexpr CommandInfo choiceArg(const ArgChoice (&list)[N], int def) const {
        CommandInfo c = *this; c.argKind = ArgKind::Choice; c.choices = list; c.choiceCount = N; c.argDefault = def; return c;
    }
};

// Standardbelegung. 'keys' im selben Format wie in keymap.txt ("Ctrl+Shift+C", "MouseL", "Numpad1").
struct DefaultBinding {
    std::string_view keys;
    Command cmd;
    int arg;
};

// Minimale Sicht auf ein konstantes Array (statt std::span, das erst C++20 ist).
template <class T>
struct ArrayView {
    const T* first = nullptr;
    std::size_t count = 0;
    constexpr const T* begin() const { return first; }
    constexpr const T* end() const { return first + count; }
    constexpr std::size_t size() const { return count; }
    constexpr const T& operator[](std::size_t i) const { return first[i]; }
};

// ---------------------------------------------------------------------------------------------
// Zugriff
// ---------------------------------------------------------------------------------------------

// Alle Befehle, in Enum-Reihenfolge. Index == static_cast<size_t>(Command).
const std::array<CommandInfo, kCommandCount>& allCommands();

// O(1), garantiert gueltig (static_assert in Commands.cpp).
const CommandInfo& info(Command id);

// Standardbelegung (Grundlage fuer "keymap.txt einmalig ausschreiben" und "Standard wiederherstellen").
ArrayView<DefaultBinding> defaultBindings();

// Sucht nach kanonischem Namen ODER altem IDM_-Namen (Klein/Gross egal). nullptr, wenn unbekannt.
// Kennt NICHT die alten Namen parametrisierter Befehle (IDM_MOVIE_5_FPS, ...): dafuer parseInvocation.
const CommandInfo* findByName(std::string_view name);

// Parst die rechte Spalte einer keymap.txt-Zeile. Akzeptiert:
//     "NextImage"            kanonisch, ohne Argument
//     "PlayMovie 25"         mit Argument  (auch "PlayMovie(25)")
//     "SetSortMode Size"     benanntes Argument
//     "IDM_MOVIE_25_FPS"     alter Name -> wird auf {PlayMovie, 25} abgebildet
// Bei Fehler: std::nullopt, und *error (falls != nullptr) enthaelt eine lesbare Begruendung.
std::optional<CommandInvocation> parseInvocation(std::string_view text, std::string* error = nullptr);

// Text der Standard-keymap.txt (aus defaultBindings() und der Befehlstabelle erzeugt), inklusive
// Syntax-Erlaeuterung und einer Referenzliste aller belegbaren Befehle als Kommentar.
// KeyMap schreibt diesen Text einmalig auf die Platte, wenn noch keine keymap.txt existiert.
std::string defaultKeyMapFileText();

// Gegenstueck zu parseInvocation: {PlayMovie, 25} -> "PlayMovie 25". Rundreise-sicher.
std::string formatInvocation(const CommandInvocation& inv);

// Gehoert das Argument zum Wertebereich des Befehls?
bool isValidArg(const CommandInfo& ci, int arg);

} // namespace Cmd

#endif // COMMANDS_H

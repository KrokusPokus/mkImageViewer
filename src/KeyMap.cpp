#include "KeyMap.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Kodierung eines "kombinierten Key-Codes" (Taste + Modifier in einem int)
//
// FIX: Die Modifier-Flags lagen vorher bei 0x10000/0x20000/0x40000. Das sind Bits, die bei
// einigen Qt-Tasten selbst gesetzt sind (z.B. Qt::Key_Select = 0x01010000, Qt::Key_Play =
// 0x01020005). Dort war die Kodierung mehrdeutig, und die Maskierung "& 0xFFFF" in
// _GetKeyShortcutName hat zusaetzlich alle Tasten oberhalb 0xFFFF (Esc, Pfeile, F-Tasten,
// Home, End, ... = 0x01000000 + x) zu 0 gemacht.
//
// Qt-Tastencodes belegen nur die Bits 0..24 (max. Qt::Key_unknown = 0x01FFFFFF). Die Bits
// darueber nutzt Qt selbst fuer Modifier (Qt::SHIFT = 0x02000000, ...). Ich lege meine Flags
// auf genau diese Bits. Dadurch ist die Trennung Taste/Modifier immer eindeutig und mit
// M_KEYMASK bzw. ~M_MODMASK exakt moeglich.
// ---------------------------------------------------------------------------------------------
static constexpr int M_SHIFT   = 0x02000000;
static constexpr int M_CTRL    = 0x04000000;
static constexpr int M_ALT     = 0x08000000;
static constexpr int M_KEYPAD  = 0x20000000;  // entspricht Qt::KeypadModifier
static constexpr int M_KEYMASK = 0x01FFFFFF;  // Bits, die der eigentliche Tastencode belegen darf

// Modifier, die der Benutzer in keymap.txt als eigenes Token schreibt (Alt+, Ctrl+, Shift+).
// M_KEYPAD gehoert bewusst NICHT dazu: Es wird nur ueber die Numpad-Tokens gesetzt.
static constexpr int M_USERMODS = M_SHIFT | M_CTRL | M_ALT;

struct SKey {
    QString Name;
    int KeyCode;
};

static const SKey KeyTable[] = {
    { QStringLiteral("Alt"), M_ALT },
    { QStringLiteral("Ctrl"), M_CTRL },
    { QStringLiteral("Shift"), M_SHIFT },
    { QStringLiteral("Esc"), Qt::Key_Escape },
    { QStringLiteral("Return"), Qt::Key_Return },
    // FIX: Die Enter-Taste des Nummernblocks liefert in Qt Key_Enter (nicht Key_Return).
    // Vorher war sie ueber keymap.txt nicht ansprechbar.
    { QStringLiteral("Enter"), Qt::Key_Enter },
    { QStringLiteral("Space"), Qt::Key_Space },
    { QStringLiteral("End"), Qt::Key_End },
    { QStringLiteral("Home"), Qt::Key_Home },
    { QStringLiteral("Back"), Qt::Key_Backspace },
    { QStringLiteral("Tab"), Qt::Key_Tab },
    { QStringLiteral("PgDn"), Qt::Key_PageDown },
    { QStringLiteral("PgUp"), Qt::Key_PageUp },
    { QStringLiteral("Left"), Qt::Key_Left },
    { QStringLiteral("Right"), Qt::Key_Right },
    { QStringLiteral("Up"), Qt::Key_Up },
    { QStringLiteral("Down"), Qt::Key_Down },
    { QStringLiteral("Insert"), Qt::Key_Insert },
    { QStringLiteral("Del"), Qt::Key_Delete },
    { QStringLiteral("Plus"), Qt::Key_Plus },
    { QStringLiteral("Minus"), Qt::Key_Minus },
    { QStringLiteral("Mul"), Qt::Key_Asterisk },
    { QStringLiteral("Div"), Qt::Key_Slash },
    { QStringLiteral("Comma"), Qt::Key_Comma },
    { QStringLiteral("Period"), Qt::Key_Period },
    // FIX (Numpad): Diese Eintraege hatten denselben Code wie die Ziffern der Haupttastatur
    // (Qt::Key_0..9). "1" und "Numpad1" landeten dadurch auf demselben Map-Schluessel, und die
    // spaetere Zeile in keymap.txt hat die fruehere ueberschrieben (Numpad1 hat "1" = Zoom 100 %
    // ausgehebelt). Qt unterscheidet beide nur ueber Qt::KeypadModifier - das ist hier als
    // M_KEYPAD Teil des Codes.
    { QStringLiteral("Numpad0"), Qt::Key_0 | M_KEYPAD },
    { QStringLiteral("Numpad1"), Qt::Key_1 | M_KEYPAD },
    { QStringLiteral("Numpad2"), Qt::Key_2 | M_KEYPAD },
    { QStringLiteral("Numpad3"), Qt::Key_3 | M_KEYPAD },
    { QStringLiteral("Numpad4"), Qt::Key_4 | M_KEYPAD },
    { QStringLiteral("Numpad5"), Qt::Key_5 | M_KEYPAD },
    { QStringLiteral("Numpad6"), Qt::Key_6 | M_KEYPAD },
    { QStringLiteral("Numpad7"), Qt::Key_7 | M_KEYPAD },
    { QStringLiteral("Numpad8"), Qt::Key_8 | M_KEYPAD },
    { QStringLiteral("Numpad9"), Qt::Key_9 | M_KEYPAD },
    // Maustasten mit eigenen internen Codes:
    { QStringLiteral("MouseL"), Mouse_Left },
    { QStringLiteral("MouseR"), Mouse_Right },
    { QStringLiteral("MouseM"), Mouse_Middle },
    { QStringLiteral("MouseX1"), Mouse_X1 },
    { QStringLiteral("MouseX2"), Mouse_X2 },
    { QStringLiteral("MouseDblClk"), Mouse_DblClk }
};

static bool _IsMouseCode(int nCombinedCode) {
    const int nKey = nCombinedCode & M_KEYMASK;
    return nKey >= Mouse_Left && nKey <= Mouse_DblClk;
}

// Position des ersten Whitespace-Zeichens, -1 wenn keines vorhanden.
// Die QRegularExpression wird nur einmal gebaut (statt pro Zeile neu kompiliert).
static int _IndexOfWhitespace(const QString& s) {
    static const QRegularExpression wsRx(QStringLiteral("\\s"));
    return s.indexOf(wsRx);
}

// Liefert den Code eines einzelnen Tokens (z.B. "PgUp", "Ctrl", "F5", "A") oder 0, wenn unbekannt.
static int _GetKeyCode(const QString& sKey) {
    for (const auto& keyItem : KeyTable) {
        if (sKey.compare(keyItem.Name, Qt::CaseInsensitive) == 0) {
            return keyItem.KeyCode;
        }
    }

    // Prüfen auf F-Tasten (F1 bis F35)
    // (Kommentar korrigiert: stand vorher "F1 bis F24", der Code erlaubt aber bis F35 -
    //  und das ist auch der Bereich, den Qt::Key_F1..Qt::Key_F35 abdeckt.)
    if (sKey.startsWith(QLatin1Char('F'), Qt::CaseInsensitive)) {
        bool ok = false;
        int nFNumber = QStringView(sKey).sliced(1).toInt(&ok);
        if (ok && nFNumber >= 1 && nFNumber <= 35) {
            return Qt::Key_F1 + nFNumber - 1;
        }
    }

    if (sKey.length() == 1) {
        int nChar = sKey[0].toUpper().unicode();
        if ((nChar >= '0' && nChar <= '9') || (nChar >= 'A' && nChar <= 'Z')) {
            return nChar;
        }
    }

    return 0;
}

// Parst "Ctrl+Shift+C" o.ae. zu einem kombinierten Key-Code.
// Rueckgabe 0 = Fehler; dann steht in *pError eine lesbare Begruendung.
//
// FIX: Vorher wurden die Teilcodes mit += addiert und unbekannte Tokens lieferten 0.
//  - "Ctrl+Foo" wurde dadurch stillschweigend zu "Ctrl" (nur der Modifier, kein Fehler).
//  - "Ctrl+Ctrl+C" oder "A+B" ergaben durch die Addition Codes, die zu keiner Taste gehoeren.
// Jetzt gilt: jedes Token muss bekannt sein, jeder Modifier hoechstens einmal vorkommen und
// genau eine "echte" Taste (bzw. Maustaste) angegeben sein. Sonst wird die Zeile mit Meldung
// abgelehnt, statt eine falsche Belegung anzulegen. Kombiniert wird mit |= statt +=.
static int _ParseKeys(const QString& sKeysStr, QString* pError) {
    int nModifiers = 0;
    int nKey = 0;

    const QStringList parts = sKeysStr.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString& rawPart : parts) {
        const QString part = rawPart.trimmed();
        const int nCode = _GetKeyCode(part);

        if (nCode == 0) {
            if (pError) *pError = QStringLiteral("unknown key name '%1'").arg(part);
            return 0;
        }

        if (nCode == (nCode & M_USERMODS)) {
            // reines Modifier-Token (Alt / Ctrl / Shift)
            if ((nModifiers & nCode) != 0) {
                if (pError) *pError = QStringLiteral("modifier '%1' used twice").arg(part);
                return 0;
            }
            nModifiers |= nCode;
        } else {
            if (nKey != 0) {
                if (pError) *pError = QStringLiteral("more than one key in '%1'").arg(sKeysStr);
                return 0;
            }
            nKey = nCode;
        }
    }

    if (nKey == 0) {
        // Deckt auch "Ctrl++" ab: Das zweite '+' ist ein Trenner, die Taste heisst "Plus".
        if (pError) *pError = QStringLiteral("no key given in '%1' (use 'Plus' for the + key)").arg(sKeysStr);
        return 0;
    }

    return nKey | nModifiers;
}

// Menschenlesbarer Name eines kombinierten Codes, leer wenn die Taste keinen Namen hat.
//
// FIX 1: "& 0xFFFF" hat bei allen Tasten ueber 0xFFFF (Esc, Pfeile, F-Tasten, Home, End, ...)
//        den Tastenanteil zu 0 gemacht -> nie ein Name. Jetzt werden gezielt nur die
//        Modifier-Bits entfernt, der Tastencode bleibt vollstaendig.
// FIX 2: Die Tabellen-Schleife hatte kein "break", dazu kamen danach noch der F-Tasten-Zweig und
//        der Ziffern-Zweig, die alle an denselben String anhaengten. Beispiel: Key_1|Keypad
//        -> "Numpad1" aus der Tabelle, danach zusaetzlich '1' aus dem Ziffern-Zweig = "Numpad11".
//        Jetzt: erster Treffer gewinnt, sofortiger Rueckgabewert.
// FIX 3: Unbenennbare Tasten lieferten vorher nur den Modifier-Praefix zurueck (z.B. "Ctrl+").
//        Jetzt leerer String; GetKeyStringForCommand ueberspringt solche Eintraege.
static QString _GetKeyShortcutName(int nShortcut) {
    QString sKeyDesc;
    if ((nShortcut & M_ALT) != 0) sKeyDesc += QStringLiteral("Alt+");
    if ((nShortcut & M_CTRL) != 0) sKeyDesc += QStringLiteral("Ctrl+");
    if ((nShortcut & M_SHIFT) != 0) sKeyDesc += QStringLiteral("Shift+");

    // Nur Alt/Ctrl/Shift entfernen. M_KEYPAD bleibt stehen, damit "Numpad1" und "1"
    // unterscheidbar bleiben (die Tabelle enthaelt die Numpad-Eintraege inkl. M_KEYPAD).
    const int nKeyPart = nShortcut & ~M_USERMODS;

    for (const auto& keyItem : KeyTable) {
        if (keyItem.KeyCode == nKeyPart) {
            if (keyItem.Name == QStringLiteral("Plus")) {
                sKeyDesc += QStringLiteral("+");
            } else if (keyItem.Name == QStringLiteral("Minus")) {
                sKeyDesc += QStringLiteral("-");
            } else {
                sKeyDesc += keyItem.Name;
            }
            return sKeyDesc;
        }
    }

    // F-Tasten-Erkennung mit Qt-Keycodes
    if (nKeyPart >= Qt::Key_F1 && nKeyPart <= Qt::Key_F35) {
        return sKeyDesc + QStringLiteral("F%1").arg(nKeyPart - Qt::Key_F1 + 1);
    }

    if ((nKeyPart >= '0' && nKeyPart <= '9') || (nKeyPart >= 'A' && nKeyPart <= 'Z')) {
        return sKeyDesc + QChar(nKeyPart);
    }

    return QString();
}

// Tasten, die auf gaengigen Layouts nur MIT Shift erreichbar sind. Qt liefert dann den Code
// des erzeugten Zeichens (z.B. Key_Plus) und setzt zusaetzlich ShiftModifier.
// US-Layout: '+' = Shift+'='   |   DE-Layout: '*' = Shift+'+', '/' = Shift+'7'
static bool _IsShiftedSymbolKey(int nKey) {
    return nKey == Qt::Key_Plus || nKey == Qt::Key_Asterisk || nKey == Qt::Key_Slash;
}

// Sucht die Datei nacheinander in den angegebenen Verzeichnissen. Leerer String = nicht gefunden.
static QString _FindFirstExisting(const QStringList& dirs, const QString& sFileName) {
    for (const QString& dir : dirs) {
        if (dir.isEmpty()) continue;   // QDir("").filePath() wuerde sonst relativ zum Arbeitsverzeichnis suchen
        const QString path = QDir(dir).filePath(sFileName);
        if (QFile::exists(path)) {
            return path;
        }
    }
    return QString();
}

// Laedt die Tastenbelegung. Suchreihenfolge fuer keymap.txt:
//     1. AppConfigLocation  (z.B. %AppData%\mkImageViewer bzw. ~/.config/mkImageViewer)
//     2. Verzeichnis der Exe
//     3. AppDataLocation    (z.B. /usr/share/mkImageViewer bei einer Linux-Installation)
//
// Gibt es in KEINEM dieser Verzeichnisse eine keymap.txt, wird die eingebaute Standardbelegung
// (Cmd::defaultKeyMapFileText()) einmalig als keymap.txt in die AppConfigLocation geschrieben
// und von dort wie jede andere Datei geladen. Es gibt damit nur EINEN Lade-Pfad; der Parser
// prueft so bei jedem Start auch die eingebauten Defaults.
//
// Die Datei ersetzt die Defaults vollstaendig (wie bisher). Neue Befehle in einer spaeteren
// Programmversion tauchen in einer bereits geschriebenen Datei daher nicht automatisch auf -
// das loest erst das Dialog-/Command-zentrierte Format.
//
// Entfallen ist symbols.km: Die Befehlsnamen kommen direkt aus Cmd::parseInvocation, inklusive der
// alten IDM_-Namen, eine vorhandene keymap.txt laeuft also unveraendert weiter.
CKeyMap::CKeyMap() {
    const QString exePath = QCoreApplication::applicationDirPath();
    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QStringList appDataDirs = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);

    QStringList keyMapDirs;
    keyMapDirs << settingsPath << exePath << appDataDirs;
    const QString keyMapFilePath = _FindFirstExisting(keyMapDirs, QStringLiteral("keymap.txt"));

    if (!keyMapFilePath.isEmpty()) {
        QFile kmFile(keyMapFilePath);
        if (kmFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&kmFile);
            LoadFromText(in.readAll(), keyMapFilePath);
            AddDefaultEscapeHandling();
            return;
        }
        // Datei existiert, ist aber nicht lesbar: NICHT ueberschreiben, nur mit Defaults weiterarbeiten.
        qWarning().noquote() << "KeyMap: cannot open" << keyMapFilePath << ":" << kmFile.errorString()
                             << "- using built-in defaults";
    }

    const QString defaultText = QString::fromStdString(Cmd::defaultKeyMapFileText());

    if (keyMapFilePath.isEmpty() && !settingsPath.isEmpty()) {
        // QSaveFile schreibt in eine temporaere Datei und benennt erst bei commit() um: Ein Absturz
        // mitten im Schreiben hinterlaesst keine halbe keymap.txt (die danach alle Tasten kappen wuerde).
        const QString targetPath = QDir(settingsPath).filePath(QStringLiteral("keymap.txt"));
        QSaveFile out(targetPath);
        if (QDir().mkpath(settingsPath) && out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&out);
            ts << defaultText;
            ts.flush();
            if (!out.commit()) {
                qWarning().noquote() << "KeyMap: cannot write" << targetPath << ":" << out.errorString();
            }
        } else {
            qWarning().noquote() << "KeyMap: cannot create" << targetPath << ":" << out.errorString();
        }
        // Ein Schreibfehler ist nicht fatal: Die Defaults gelten trotzdem fuer diese Sitzung.
    }

    LoadFromText(defaultText, QStringLiteral("built-in defaults"));
    AddDefaultEscapeHandling();
}

// Parst den Inhalt einer keymap-Datei. sSourceName dient nur der Meldungen (Dateipfad oder
// "built-in defaults").
void CKeyMap::LoadFromText(const QString& sText, const QString& sSourceName) {
    // Einheitliche, auffindbare Meldungen (Quelle + Zeilennummer).
    auto warn = [&sSourceName](int nLine, const QString& sMsg) {
        qWarning().noquote() << QStringLiteral("KeyMap: %1:%2: %3")
                                    .arg(QFileInfo(sSourceName).fileName()).arg(nLine).arg(sMsg);
    };

    // Hinweis zum "Unbelegen": Das Format ist tastenzentriert (Taste -> Befehl), eine Taste kann
    // also nicht einfach "weggelassen" werden, um einen Standard zu deaktivieren - die Datei
    // ersetzt die Defaults ja vollstaendig. Fuer "Taste soll nichts tun, aber nicht an Qt
    // weitergereicht werden" gibt es den Befehl DoNothing.
    const QStringList lines = sText.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const int nLine = i + 1;
        const QString line = lines[i].trimmed();   // trimmed() entfernt auch ein '\r' (CRLF-Dateien)
        if (line.isEmpty() || line.startsWith(QStringLiteral("//"))) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#define"))) {
            continue; // Veraltete Definitionen in KeyMap ignorieren
        }

        // Spalte 1 (links) sind die Tasten (z.B. "PgUp" oder "Ctrl+C")
        // Spalte 2 (rechts) ist der Befehl, ggf. mit Argument (z.B. "NextFile" oder "PlayMovie 25")
        const int spaceIdx = _IndexOfWhitespace(line);
        if (spaceIdx == -1) {
            warn(nLine, QStringLiteral("expected '<keys> <command>', line ignored: %1").arg(line));
            continue;
        }

        const QString sKeys = line.left(spaceIdx).trimmed();
        const QString sCommand = line.mid(spaceIdx).trimmed();

        std::string sCmdError;
        const std::optional<Cmd::CommandInvocation> inv = Cmd::parseInvocation(sCommand.toStdString(), &sCmdError);
        if (!inv) {
            warn(nLine, QString::fromStdString(sCmdError) + QStringLiteral(", line ignored"));
            continue;
        }
        if (!Cmd::info(inv->id).bindable) {
            warn(nLine, QStringLiteral("command '%1' cannot be bound to a key, line ignored").arg(sCommand));
            continue;
        }

        QString sKeyError;
        const int nKeyCode = _ParseKeys(sKeys, &sKeyError);
        if (nKeyCode == 0) {
            warn(nLine, sKeyError + QStringLiteral(", line ignored"));
            continue;
        }

        // Doppelte Belegung: Die letzte Zeile gewinnt (kompatibel), aber nicht mehr stillschweigend.
        // Dieselbe Taste noch einmal mit demselben Befehl ist harmlos.
        const auto existing = m_keyMap.find(nKeyCode);
        if (existing != m_keyMap.end() && existing->second != *inv) {
            warn(nLine, QStringLiteral("'%1' is already mapped to '%2', overridden by '%3'")
                            .arg(sKeys,
                                 QString::fromStdString(Cmd::formatInvocation(existing->second)),
                                 QString::fromStdString(Cmd::formatInvocation(*inv))));
        }

        m_keyMap[nKeyCode] = *inv;
    }
}

int CKeyMap::GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift, bool keypad) {
    // FIX: "|=" statt "+=". Mit Flags auf getrennten Bits sind beide gleichwertig, "|" drueckt
    // aber aus, was gemeint ist, und kann bei einem doppelt uebergebenen Flag nichts verfaelschen.
    int nKeyCode = keyCode;
    if (alt) nKeyCode |= M_ALT;
    if (control) nKeyCode |= M_CTRL;
    if (shift) nKeyCode |= M_SHIFT;
    if (keypad) nKeyCode |= M_KEYPAD;
    return nKeyCode;
}

void CKeyMap::AddDefaultEscapeHandling() {
    // Esc ist bewusst nicht entfernbar: Ohne eine Belegung kaeme man aus Vollbild/Slideshow nur
    // noch ueber den Task-Manager heraus. Eine explizite Belegung in keymap.txt hat Vorrang.
    if (m_keyMap.find(Qt::Key_Escape) == m_keyMap.end()) {
        m_keyMap[Qt::Key_Escape] = Cmd::CommandInvocation{ Cmd::Command::DefaultEscape, 0 };
    }
}

// Suchreihenfolge: genauester Treffer zuerst, danach Rueckfaelle.
//
// 1. Exakt (Taste + Alt/Ctrl/Shift + Keypad-Flag).
// 2. Nur bei Tasten vom Nummernblock: dieselbe Taste ohne Keypad-Flag.
//    Grund: Das Keypad-Flag darf nichts verschlechtern. "Plus -> ZoomIn" hat auf dem
//    Nummernblock-'+' vorher funktioniert (gleicher Qt-Code), und mit NumLock aus senden die
//    Numpad-Pfeile Key_Up usw. mit KeypadModifier. Wer eine Numpad-Taste explizit belegt
//    ("Numpad1"), bekommt weiterhin Vorrang vor der normalen Ziffer.
// 3. Nur bei Symboltasten, die ueber Shift erzeugt werden (+, *, /): ohne Shift suchen.
//    Grund: Auf dem US-Layout liefert '+' den Code Key_Plus MIT ShiftModifier; die Belegung
//    "Plus" (ohne Shift) haette sonst nie gegriffen. Eine explizite Belegung "Shift+Plus" hat
//    wegen Schritt 1 Vorrang.
//
// Rueckgabe std::nullopt = Taste nicht belegt. Ein belegtes DoNothing ist ein gueltiger Treffer
// (Taste wird verbraucht, tut aber nichts) - das war frueher "ID 1 > 0".
std::optional<Cmd::CommandInvocation> CKeyMap::GetCommandForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift, bool bKeypad) const {
    auto lookup = [this](int nCode) -> std::optional<Cmd::CommandInvocation> {
        const auto iter = m_keyMap.find(nCode);
        if (iter == m_keyMap.end()) return std::nullopt;
        return iter->second;
    };

    if (auto cmd = lookup(GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, bShift, bKeypad))) return cmd;

    if (bKeypad) {
        if (auto cmd = lookup(GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, bShift, false))) return cmd;
    }

    if (bShift && _IsShiftedSymbolKey(nVirtualKeyCode)) {
        if (auto cmd = lookup(GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, false, bKeypad))) return cmd;

        if (bKeypad) {
            if (auto cmd = lookup(GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, false, false))) return cmd;
        }
    }

    return std::nullopt;
}

// Reihenfolge der Kuerzel (m_keyMap ist eine unordered_map, die Iterationsreihenfolge ist
// unspezifiziert und darf nicht durchschlagen):
//   1. Tastaturkuerzel vor Maustasten,
//   2. weniger Modifier vor mehr Modifiern (der einfachste Kuerzel ist der "Haupt"-Kuerzel),
//   3. bei Gleichstand alphabetisch.
// Der Vergleich geht ueber den ganzen Aufruf (Befehl UND Argument): "ZoomTo 100" und
// "ZoomTo 50" sind verschiedene Eintraege mit verschiedenen Kuerzeln.
QString CKeyMap::GetKeyStringForCommand(const Cmd::CommandInvocation& inv) const {
    struct SShortcut {
        int nCode;
        QString sName;
    };
    std::vector<SShortcut> shortcuts;

    for (auto const& [key, cmd] : m_keyMap) {
        if (cmd == inv) {
            QString sShortCutName = _GetKeyShortcutName(key);
            if (!sShortCutName.isEmpty()) {
                shortcuts.push_back({ key, sShortCutName });
            }
        }
    }

    std::sort(shortcuts.begin(), shortcuts.end(), [](const SShortcut& a, const SShortcut& b) {
        const bool bMouseA = _IsMouseCode(a.nCode);
        const bool bMouseB = _IsMouseCode(b.nCode);
        if (bMouseA != bMouseB) return !bMouseA;

        const int nModsA = qPopulationCount(static_cast<quint32>(a.nCode & M_USERMODS));
        const int nModsB = qPopulationCount(static_cast<quint32>(b.nCode & M_USERMODS));
        if (nModsA != nModsB) return nModsA < nModsB;

        return a.sName < b.sName;
    });

    QString sKeyDesc;
    for (const SShortcut& sc : shortcuts) {
        if (!sKeyDesc.isEmpty()) {
            sKeyDesc += QLatin1Char('/');
        }
        sKeyDesc += sc.sName;
    }

    return sKeyDesc;
}

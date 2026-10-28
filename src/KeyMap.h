#ifndef KEYMAP_H
#define KEYMAP_H

#include "Commands.h"

#include <optional>
#include <unordered_map>
#include <QString>

enum InternalMouseCodes {
    Mouse_Left = 0x1000,
    Mouse_Right,
    Mouse_Middle,
    Mouse_X1,
    Mouse_X2,
    Mouse_DblClk
};

// KeyMap: ordnet Tasten/Maustasten (mit Modifiern) Befehlen aus Commands.h zu.
class CKeyMap
{
public:
    // Laedt keymap.txt (Suchreihenfolge siehe KeyMap.cpp). Existiert keine, wird die eingebaute
    // Standardbelegung einmalig als keymap.txt ausgeschrieben und geladen.
    CKeyMap();

    // Liefert den Befehl (inkl. Argument) fuer eine Taste samt Modifiern, std::nullopt wenn die
    // Taste nicht belegt ist.
    //
    // bKeypad: Qt meldet Nummernblock-Tasten als dieselbe Taste wie die Haupttastatur plus
    // Qt::KeypadModifier. Fuer Key-Events event->modifiers().testFlag(Qt::KeypadModifier)
    // uebergeben, fuer Maustasten false (Default).
    //
    // Lookup-Reihenfolge (siehe .cpp): exakter Treffer, dann Rueckfaelle fuer Keypad- und
    // Shift-Symboltasten, damit z.B. das Numpad-'+' weiter eine "Plus"-Belegung ausloest.
    std::optional<Cmd::CommandInvocation> GetCommandForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift, bool bKeypad = false) const;

    // Text der Kuerzel fuer einen Befehl (z.B. fuer Menues), mehrere mit '/' getrennt, leer wenn keins.
    // Die Reihenfolge ist deterministisch (Tastatur vor Maus, weniger Modifier zuerst, dann alphabetisch).
    QString GetKeyStringForCommand(const Cmd::CommandInvocation& inv) const;
    QString GetKeyStringForCommand(Cmd::Command cmd, int arg = 0) const {
        return GetKeyStringForCommand(Cmd::CommandInvocation{ cmd, arg });
    }

    // gets a combined key code
    static int GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift, bool keypad = false);

private:

    // key is the combined key code, value the command incl. argument
    std::unordered_map<int, Cmd::CommandInvocation> m_keyMap;

    void LoadFromText(const QString& sText, const QString& sSourceName);
    void AddDefaultEscapeHandling();
};

#endif // KEYMAP_H

#ifndef KEYMAP_H
#define KEYMAP_H

#include <unordered_map>
#include <QString>

#define VK_LBUTTONDBLCLK 7

// KeyMap
class CKeyMap
{
public:
    // Reads the keymap file with logic for loading from different locations, including the default
    CKeyMap();

    // Gets a command Id for a keycode and modifier keys. Returns -1 if no command available for this key.
    int GetCommandIdForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift);

    // Gets the string describing the keyboard shortcut for a given command, empty string if none
    QString GetKeyStringForCommand(int nCommandId);

    // gets a combined key code
    static int GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift);

private:

    // key is the key code, value the command ID
    std::unordered_map<int, int> m_keyMap;

    void AddDefaultEscapeHandling();
};

#endif // KEYMAP_H

#include "KeyMap.h"
#include "resource.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <list>

#define M_SHIFT 0x10000
#define M_ALT   0x20000
#define M_CTRL  0x40000

struct SKey {
    QString Name;
    int KeyCode;
};

static SKey KeyTable[] = {
    { QStringLiteral("Alt"), M_ALT },
    { QStringLiteral("Ctrl"), M_CTRL },
    { QStringLiteral("Shift"), M_SHIFT },
    { QStringLiteral("Esc"), Qt::Key_Escape },
    { QStringLiteral("Return"), Qt::Key_Return },
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
    { QStringLiteral("Numpad0"), Qt::Key_0 },
    { QStringLiteral("Numpad1"), Qt::Key_1 },
    { QStringLiteral("Numpad2"), Qt::Key_2 },
    { QStringLiteral("Numpad3"), Qt::Key_3 },
    { QStringLiteral("Numpad4"), Qt::Key_4 },
    { QStringLiteral("Numpad5"), Qt::Key_5 },
    { QStringLiteral("Numpad6"), Qt::Key_6 },
    { QStringLiteral("Numpad7"), Qt::Key_7 },
    { QStringLiteral("Numpad8"), Qt::Key_8 },
    { QStringLiteral("Numpad9"), Qt::Key_9 },
    // Maustasten mit eigenen internen Codes:
    { QStringLiteral("MouseL"), Mouse_Left },
    { QStringLiteral("MouseR"), Mouse_Right },
    { QStringLiteral("MouseM"), Mouse_Middle },
    { QStringLiteral("MouseX1"), Mouse_X1 },
    { QStringLiteral("MouseX2"), Mouse_X2 },
    { QStringLiteral("MouseDblClk"), Mouse_DblClk }
};


static int _GetKeyCode(const QString& sKey) {
    for (const auto& keyItem : KeyTable) {
        if (sKey.compare(keyItem.Name, Qt::CaseInsensitive) == 0) {
            return keyItem.KeyCode;
        }
    }

    // Prüfen auf F-Tasten (F1 bis F24)
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

static int _ParseKeys(const QString& sKeysStr) {
    int nKeyCode = 0;
    const QStringList parts = sKeysStr.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        nKeyCode += _GetKeyCode(part.trimmed());
    }
    return nKeyCode;
}

static int _FindCommandId(const std::unordered_map<QString, int>& symbolMap, const QString& sSymbolName) {
    if (sSymbolName.isEmpty()) return -1;

    auto iter = symbolMap.find(sSymbolName);
    if (iter == symbolMap.end()) {
        return -1;
    } else {
        return iter->second;
    }
}

static QString _GetKeyShortcutName(int nShortcut) {
    QString sKeyDesc;
    if ((nShortcut & M_ALT) != 0) sKeyDesc += QStringLiteral("Alt+");
    if ((nShortcut & M_CTRL) != 0) sKeyDesc += QStringLiteral("Ctrl+");
    if ((nShortcut & M_SHIFT) != 0) sKeyDesc += QStringLiteral("Shift+");

    int nRealShortcut = nShortcut & 0xFFFF; // Maskiert die Modifikatoren weg

    for (const auto& keyItem : KeyTable) {
        if (keyItem.KeyCode == nRealShortcut) {
            if (keyItem.Name == QStringLiteral("Plus")) {
                sKeyDesc += QStringLiteral("+");
            } else if (keyItem.Name == QStringLiteral("Minus")) {
                sKeyDesc += QStringLiteral("-");
            } else {
                sKeyDesc += keyItem.Name;
            }
        }
    }

    // F-Tasten-Erkennung mit Qt-Keycodes
    if (nRealShortcut >= Qt::Key_F1 && nRealShortcut <= Qt::Key_F35) {
        sKeyDesc += QString("F%1").arg(nRealShortcut - Qt::Key_F1 + 1);
    }

    if ((nRealShortcut >= '0' && nRealShortcut <= '9') || (nRealShortcut >= 'A' && nRealShortcut <= 'Z')) {
        sKeyDesc += QChar(nRealShortcut);
    }
    return sKeyDesc;
}

// Similar to SettingsProvider, attemps to load the keymap from various locations
// before settling for the default
//
// First tries to see if /keymap parameter was passed on command line
// Then tries %AppData%\mkImageViewer\KeyMap.txt
// Then tries (mkImageViewer root)\KeyMap.txt
// if none of those, then (mkImageViewer root)\KeyMap.txt.default
CKeyMap::CKeyMap() {
    const QString exePath = QCoreApplication::applicationDirPath();
    const QString settingsPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    
    QString keyMapFilePath = QDir(settingsPath).filePath(QStringLiteral("keymap.txt"));
    if (!QFile::exists(keyMapFilePath)) {
        keyMapFilePath = QDir(exePath).filePath(QStringLiteral("keymap.txt"));
    }
    if (!QFile::exists(keyMapFilePath)) {
        AddDefaultEscapeHandling();
        return;
    }

    // 2. Symbole-Datei prüfen
    QString symbolsFilePath = QDir(exePath).filePath(QStringLiteral("symbols.km"));
    if (!QFile::exists(symbolsFilePath)) {
        AddDefaultEscapeHandling();
        return;
    }

    std::unordered_map<QString, int> m_symbolMap;

    // Symbole einlesen (`symbols.km`)
    QFile symFile(symbolsFilePath);
    if (symFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&symFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QStringLiteral("//"))) {
                continue;
            }

            if (line.startsWith(QStringLiteral("#define"))) {
                QString trimmedRem = line.mid(7).trimmed();

                int spaceIdx = trimmedRem.indexOf(QRegularExpression(QStringLiteral("\\s")));
                if (spaceIdx == -1) continue;

                QString symbol = trimmedRem.left(spaceIdx).trimmed();
                QString valueStr = trimmedRem.mid(spaceIdx).trimmed();

                bool ok = false;
                int nValue = valueStr.toInt(&ok);
                if (ok && nValue != 0) {
                    m_symbolMap[symbol] = nValue;
                }
            }
        }
        symFile.close();
    }

    // KeyMap-Datei einlesen (`KeyMap.txt`)
    QFile kmFile(keyMapFilePath);
    if (kmFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&kmFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QStringLiteral("//"))) {
                continue;
            }
            if (line.startsWith(QStringLiteral("#define"))) {
                continue; // Veraltete Definitionen in KeyMap ignorieren
            }

            // Spalte 1 (links) sind die Tasten (z.B. "PgUp" oder "Ctrl+C")
            // Spalte 2 (rechts) ist die Command-ID (z.B. "IDM_PREVFILE")
            int spaceIdx = line.indexOf(QRegularExpression(QStringLiteral("\\s")));
            if (spaceIdx == -1) continue;

            QString sKeys = line.left(spaceIdx).trimmed();
            QString sCommandId = line.mid(spaceIdx).trimmed();

            int nCommandId = _FindCommandId(m_symbolMap, sCommandId);
            if (nCommandId < 0) continue;

            int nKeyCode = _ParseKeys(sKeys);
            if (nKeyCode == 0) continue;

            m_keyMap[nKeyCode] = nCommandId;
        }
        kmFile.close();
    }

    AddDefaultEscapeHandling();
}

int CKeyMap::GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift) {
    int nKeyCode = keyCode;
    if (alt) nKeyCode += M_ALT;
    if (control) nKeyCode += M_CTRL;
    if (shift) nKeyCode += M_SHIFT;
    return nKeyCode;
}

void CKeyMap::AddDefaultEscapeHandling() {
    if (m_keyMap.find(Qt::Key_Escape) == m_keyMap.end()) {
        m_keyMap[Qt::Key_Escape] = IDM_DEFAULT_ESC;
    }
}

int CKeyMap::GetCommandIdForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift) {
    int nKeyCode = GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, bShift);
    auto iter = m_keyMap.find(nKeyCode);
    if (iter == m_keyMap.end()) {
        return -1;
    } else {
        return iter->second;
    }
}

QString CKeyMap::GetKeyStringForCommand(int nCommandId) {
    std::list<QString> keys;
    for (auto const& [key, cmdId] : m_keyMap) {
        if (nCommandId == cmdId) {
            QString sShortCutName = _GetKeyShortcutName(key);
            if (!sShortCutName.isEmpty()) {
                if (sShortCutName.startsWith(QLatin1Char('P'))) {
                    keys.push_front(sShortCutName);
                } else {
                    keys.push_back(sShortCutName);
                }
            }
        }
    }

    QString sKeyDesc;
    for (const QString& keyName : keys) {
        if (!sKeyDesc.isEmpty()) {
            sKeyDesc += QLatin1Char('/');
        }
        sKeyDesc += keyName;
    }

    return sKeyDesc;
}

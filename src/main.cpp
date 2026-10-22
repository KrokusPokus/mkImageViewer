#include "resource.h"
#include "MainDlg.h"
#include "SettingsProvider.h"

#include <clocale>
#include <gdiplus.h>
#include <QString>
#include <QApplication>

static HWND _HWNDOtherInstance = nullptr;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    const int BUF_LEN = 255;
    WCHAR buff[BUF_LEN + 1];
    buff[BUF_LEN] = 0;
    ::GetWindowTextW(hwnd, buff, BUF_LEN);

    if (wcsstr(buff, L" - JPEGView") != nullptr) {
        // An already existing instance was found -> send our file path to it and wait how it reacts.
        // If it returns NULL, it doesn't want that file and we can open it ourselves.
        // If it returns KEY_MAGIC, it will handle that file and we can close again.

        COPYDATASTRUCT copyData{ 0 };
        copyData.dwData = KEY_MAGIC;
        copyData.cbData = static_cast<DWORD>((wcslen(reinterpret_cast<LPCWSTR>(lParam)) + 1) * sizeof(WCHAR));
        copyData.lpData = reinterpret_cast<LPVOID>(const_cast<WCHAR*>(reinterpret_cast<LPCWSTR>(lParam)));

        ULONG_PTR result = 0;
        PDWORD_PTR resultPtr = &result;
        LRESULT res = ::SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData), 0, 250, resultPtr);

        if (*resultPtr == static_cast<ULONG_PTR>(KEY_MAGIC)) {
            _HWNDOtherInstance = hwnd;
            return FALSE;    // stop enumerating windows
        }
    }
    return TRUE;        // continue enumerating windows
}

static QString ParseCommandLineForStartupFile(const QString& sCommandLine) {
    QString sStartupFile = sCommandLine.trimmed();
    if (sStartupFile.isEmpty()) {
        return QString();
    }
    if (sStartupFile.startsWith(QLatin1Char('/'))) { // parameter
        return QString();
    }

    // Extract the startup file or directory from the command line parameter string
    int nHyphen = sStartupFile.indexOf(QLatin1Char('"'));
    if (nHyphen == -1) {
        // not enclosed with "", use part until first space is found
        int nSpace = sStartupFile.indexOf(QLatin1Char(' '));
        if (nSpace > 0) {
            sStartupFile = sStartupFile.left(nSpace);
        } // else we use the whole command line (as there is no space character in it)
    } else {
        // enclosed in "", take first string enclosed in ""
        int nHyphenNext = sStartupFile.indexOf(QLatin1Char('"'), nHyphen + 1);
        if (nHyphenNext > nHyphen) {
            sStartupFile = sStartupFile.mid(nHyphen + 1, nHyphenNext - nHyphen - 1);
        }
    }
    // if it's a directory, add a trailing backslash
    if (!sStartupFile.isEmpty()) {
        DWORD nAttributes = ::GetFileAttributesW(reinterpret_cast<LPCWSTR>(sStartupFile.utf16()));
        if (nAttributes != INVALID_FILE_ATTRIBUTES && (nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!sStartupFile.endsWith(QLatin1Char('\\')) && !sStartupFile.endsWith(QLatin1Char('/'))) {
                sStartupFile += QLatin1Char('\\');
            }
        }
    }

    if (!sStartupFile.contains(QLatin1Char('\\')) && !sStartupFile.contains(QLatin1Char('/')))
        return QStringLiteral(".\\") + sStartupFile;

    return sStartupFile;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine, int /*nCmdShow*/)
{
    int argc = 0;
    QApplication app(argc, nullptr); // Notwendig für QClipboard und QMessageBox

    // use the locale from the operating system, however for numeric input/output always use 'C' locale
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");

    QString sStartupFile = ParseCommandLineForStartupFile(QString::fromWCharArray(lpstrCmdLine));

    // Searches for other instances and asks them if *they* want to handle our startup file in our place. If not, we do it.
    HANDLE hMutex = ::CreateMutexW(nullptr, FALSE, L"JPVMtX2869");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::EnumWindows(reinterpret_cast<WNDENUMPROC>(EnumWindowsProc), reinterpret_cast<LPARAM>(sStartupFile.utf16()));
    }

    int nRet = 0;
    // Run application
    if (_HWNDOtherInstance == nullptr) {
        // Initialize GDI+
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        ULONG_PTR gdiplusToken;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

        CMainDlg dlgMain(sStartupFile);

        try {
            dlgMain.show();          // Hauptfenster anzeigen
            nRet = app.exec();       // Startet die native Qt-Event-Schleife

            if (CSettingsProvider::This().StickyWindowSize() && !dlgMain.IsFullScreenMode()) {
                CSettingsProvider::This().SaveStickyWindowRect(dlgMain.WindowRectOnClose());
            }
            ::ShowCursor(TRUE);
        }
        catch (...) {
            ::ShowCursor(TRUE);
        }

        // Shut down GDI+
        Gdiplus::GdiplusShutdown(gdiplusToken);
    } else {
        ::SetForegroundWindow(_HWNDOtherInstance);
    }

    ::CloseHandle(hMutex);
    return nRet;
}
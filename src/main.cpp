#include "resource.h"
#include "MainDlg.h"
#include "SettingsProvider.h"

#include <clocale>
#include <gdiplus.h>

#include <QApplication>

CAppModule _Module;

static HWND _HWNDOtherInstance = NULL;

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    const int BUF_LEN = 255;
    TCHAR buff[BUF_LEN + 1];
    buff[BUF_LEN] = 0;
    ::GetWindowText(hwnd, (LPTSTR)&buff, BUF_LEN);

    if (_tcsstr(buff, _T(" - JPEGView")) != NULL) {
        // An already existing instance was found -> send our file path to it and wait how it reacts.
        // If it returns NULL, it doesn't want that file and we can open it ourselves.
        // If it returns KEY_MAGIC, it will handle that file and we can close again.

        COPYDATASTRUCT copyData{ 0 };
        copyData.dwData = KEY_MAGIC;
        copyData.cbData = (lstrlen((LPCTSTR)lParam) + 1) * sizeof(TCHAR);
        copyData.lpData = (LPVOID)(LPCTSTR)lParam;
        ULONG_PTR result = 0;
        PDWORD_PTR resultPtr = &result;
        LRESULT res = ::SendMessageTimeout(hwnd, WM_COPYDATA, 0, (LPARAM)&copyData, 0, 250, resultPtr);

        if (*resultPtr == (ULONG_PTR)KEY_MAGIC) {
            _HWNDOtherInstance = hwnd;
            return FALSE;    // stop enumerating windows
        }
    }
    return TRUE;        // continue enumerating windows
}

static CString ParseCommandLineForStartupFile(LPCTSTR sCommandLine) { 
    CString sStartupFile = sCommandLine;
    if (sStartupFile.GetLength() == 0) {
        return CString(_T(""));
    }
    if (sCommandLine[0] == _T('/')) { // parameter
        return CString(_T(""));
    }

    // Extract the startup file or directory from the command line parameter string
    int nHyphen = sStartupFile.Find(_T('"'));
    if (nHyphen == -1) {
        // not enclosed with "", use part until first space is found
        int nSpace = sStartupFile.Find(_T(' '));
        if (nSpace > 0) {
            sStartupFile = sStartupFile.Left(nSpace);
        } // else we use the whole command line (as there is no space character in it)
    } else {
        // enclosed in "", take first string enclosed in ""
        int nHyphenNext = sStartupFile.Find(_T('"'), nHyphen + 1);
        if (nHyphenNext > nHyphen) {
            sStartupFile = sStartupFile.Mid(nHyphen + 1, nHyphenNext - nHyphen - 1);
        }
    }
    // if it's a directory, add a trailing backslash
    if (sStartupFile.GetLength() > 0) {
        DWORD nAttributes = ::GetFileAttributes(sStartupFile);
        if (nAttributes != INVALID_FILE_ATTRIBUTES && (nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (sStartupFile[sStartupFile.GetLength() -1] != _T('\\')) {
                sStartupFile += _T('\\');
            }
        }
    }

    if (sStartupFile.Find(_T('\\')) == -1)
        return _T(".\\") + sStartupFile;

    return sStartupFile;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine, int /*nCmdShow*/)
{
    HRESULT hRes = ::CoInitialize(NULL);

    int argc = 0;
    QApplication app(argc, nullptr); // Notwendig für QClipboard
	
    // use the locale from the operating system, however for numeric input/output always use 'C' locale
    // with the OS locale the INI file can not be read correctly (due to the different decimal point characters)
    _tsetlocale(LC_ALL, _T(""));
    _tsetlocale(LC_NUMERIC, _T("C"));

    // this resolves ATL window thunking problem when Microsoft Layer for Unicode (MSLU) is used
    ::DefWindowProc(NULL, 0, 0, 0L);

    AtlInitCommonControls(ICC_BAR_CLASSES);    // add flags to support other controls

    hRes = _Module.Init(NULL, hInstance);
    ATLASSERT(SUCCEEDED(hRes));

    CString sStartupFile = ParseCommandLineForStartupFile(lpstrCmdLine);

    // Searches for other instances and asks them if *they* want to handle our startup file in our place. If not, we do it.
    HANDLE hMutex = ::CreateMutex(NULL, FALSE, _T("JPVMtX2869"));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::EnumWindows((WNDENUMPROC)EnumWindowsProc, (LPARAM)(LPCTSTR)sStartupFile);
    }

    int nRet = 0;
    //Run application
    if (_HWNDOtherInstance == NULL) {
        // Initialize GDI+
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        ULONG_PTR gdiplusToken;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

        CMainDlg dlgMain(sStartupFile);

        try {
            nRet = (int)dlgMain.DoModal();
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
        SetForegroundWindow(_HWNDOtherInstance);
    }

    ::CloseHandle(hMutex);

    _Module.Term();
    ::CoUninitialize();

    return nRet;
}


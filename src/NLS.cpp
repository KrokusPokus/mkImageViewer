#include "NLS.h"
#include "SettingsProvider.h"

#include <cstring>
#include <unordered_map>

// Neudefinition der statischen Member-Variablen mit std::unordered_map und std::wstring
std::unordered_map<std::wstring, std::wstring> CNLS::sm_texts;
bool CNLS::sm_bTableRead = false;


CString CNLS::GetLocalizedFileName(LPCTSTR sPath, LPCTSTR sPrefixName, LPCTSTR sExtension, LPCTSTR sLanguageCode) {
    CString sNLSFile;
    TCHAR buff[16], buff2[16];
    if (_tcscmp(sLanguageCode, _T("auto")) == 0) {
        LCID threadLocale = ::GetThreadLocale();
        ::GetLocaleInfo(threadLocale, LOCALE_SISO639LANGNAME, (LPTSTR)&buff, sizeof(buff) / sizeof(TCHAR));
        ::GetLocaleInfo(threadLocale, LOCALE_SISO3166CTRYNAME, (LPTSTR)&buff2, sizeof(buff2) / sizeof(TCHAR));
        sLanguageCode = buff;
        sNLSFile = CString(sPath) + sPrefixName + _T("_") + sLanguageCode + _T("-") + buff2 + _T(".") + sExtension;
        if (::GetFileAttributes(sNLSFile) == INVALID_FILE_ATTRIBUTES) {
            sNLSFile.Empty();
        }
    }
    if (sNLSFile.IsEmpty()) {
        sNLSFile = CString(sPath) + sPrefixName + _T("_") + sLanguageCode + _T(".") + sExtension;
    }
    return sNLSFile;
}

CString CNLS::GetStringTableFileName(LPCTSTR sLanguageCode) {
    return GetLocalizedFileName(CSettingsProvider::This().GetEXEPath(), _T("strings"), _T("txt"), sLanguageCode);
}

// Ignores newlines, ignores invalid lines
void CNLS::ReadStringTable(LPCTSTR sFileName) {
    FILE *fptr = _tfopen(sFileName, _T("r, ccs=UTF-8"));
    if (fptr == NULL) {
        return;
    }

    const int FILE_BUFF_SIZE = 32000;
    TCHAR* pLineBuff = new TCHAR[FILE_BUFF_SIZE];
    memset(pLineBuff, 0, FILE_BUFF_SIZE * sizeof(TCHAR));

    // Puffer einlesen
    size_t nReadBytes = fread(pLineBuff, sizeof(TCHAR), FILE_BUFF_SIZE, fptr);
    fclose(fptr);

    TCHAR* pRunning = pLineBuff;
    while (*pRunning != 0) {
        if (pRunning[0] == _T('/') && pRunning[1] == _T('/')) {
            // Kommentarzeile ignorieren
            while (*pRunning != 0 && *pRunning != _T('\n') && *pRunning != _T('\r')) {
                pRunning++;
            }
        } else {
            TCHAR* pStartLine = pRunning;
            while (*pRunning != _T('\t') && *pRunning != _T('\n') && *pRunning != 0) {
                pRunning++;
            }
            if (*pRunning == _T('\t')) {
                *pRunning = 0;
                pRunning++;
                TCHAR* pStartTranslated = pRunning;
                while (*pRunning != _T('\r') && *pRunning != _T('\n') && *pRunning != 0) {
                    pRunning++;
                }
                bool bIncrement = (*pRunning != 0);
                *pRunning = 0;
                if (*pStartTranslated != 0) {
                    // std::wstring übernimmt das Speichermanagement komplett.
                    // Kein manuelle 'new TCHAR[]' Speicher-Allocation mehr nötig!
                    sm_texts[std::wstring(pStartLine)] = std::wstring(pStartTranslated);
                }
                if (bIncrement) pRunning++;
            }
        }
        // Leerzeilen / Newlines überspringen
        while (*pRunning != 0 && (*pRunning == _T('\n') || *pRunning == _T('\r'))) {
            pRunning++;
        }
    }

    delete[] pLineBuff;
    sm_bTableRead = true;
}

LPCTSTR CNLS::GetString(LPCTSTR sString) {
    if (!sm_bTableRead || sString == NULL) {
        return sString; // no translation available, use English
    }

    // Suche in der std::unordered_map
    auto iter = sm_texts.find(std::wstring(sString));
    if (iter == sm_texts.end()) {
        return sString; // not found
    } else {
        // c_str() ist über den gesamten Lebenszyklus der Anwendung sicher,
        // da sm_texts als statische Map nicht geleert wird.
        return iter->second.c_str();
    }
}
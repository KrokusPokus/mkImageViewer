#ifndef NLS_H
#define NLS_H

#pragma once

#include <unordered_map>
#include <string>

class CNLS {
public:
    static CString GetLocalizedFileName(LPCTSTR sPath, LPCTSTR sPrefixName, LPCTSTR sExtension, LPCTSTR sLanguageCode);
    static CString GetStringTableFileName(LPCTSTR sLanguageCode);

    static void ReadStringTable(LPCTSTR sFileName);
    static LPCTSTR GetString(LPCTSTR sString);

private:
    // Modernes, plattformunabhängiges Mapping von std::wstring zu std::wstring
    static std::unordered_map<std::wstring, std::wstring> sm_texts;
    static bool sm_bTableRead;
};

#endif // NLS_H

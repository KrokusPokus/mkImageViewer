#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

// ============================================================================
// 1. C++ Standard Library & C++11 Typen
// ============================================================================
//#include <cstdint>
#include <cassert>
#include <vector>
#include <string>
#include <list>
#include <memory>
//#include <algorithm>

// ============================================================================
// 3. Windows-spezifische Altlasten (Nur unter Windows kompilieren)
// ============================================================================
#if defined(_WIN32) || defined(WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif

    #define WINVER        0x0601
    #define _WIN32_WINNT  0x0601

    #define _CRT_SECURE_NO_DEPRECATE

    // Nur falls WTL/ATL in einzelnen Dateien noch schrittweise abgebaut wird:
    #include <tchar.h>
    #include <assert.h>
    #include <atlbase.h>
    #include <atlapp.h>		// WTL-sf\Include
    #include <atldlgs.h>	// WTL-sf\Include: DROPFILES, CFSTR_PREFERREDDROPEFFECT
    #include <atlmisc.h>	// WTL-sf\Include: CSize, CRect, CPoint
    #include <mmsystem.h>   // timeBeginPeriod(), timeEndPeriod(), timeGetTime()

    extern CAppModule _Module;

    #define VK_PAGE_UP 0x021
    #define VK_PAGE_DOWN 0x22
    #define VK_PLUS 0x6b
    #define VK_MINUS 0x6d
#endif

//#include "WTLCompat.h"

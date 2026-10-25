#ifndef WTLCOMPAT_H
#define WTLCOMPAT_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#ifdef QT_CORE_LIB
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#endif

#// Falls keine Windows-Header geladen sind, definieren wir die benötigten VK_-Makros manuell
#ifndef _WINUSER_
#define VK_LBUTTON          0x01
#define VK_RBUTTON          0x02
#define VK_CANCEL           0x03
#define VK_MBUTTON          0x04
#define VK_XBUTTON1         0x05
#define VK_XBUTTON2         0x06
#define VK_BACK             0x08
#define VK_TAB              0x09
#define VK_RETURN           0x0D
#define VK_SHIFT            0x10
#define VK_CONTROL          0x11
#define VK_MENU             0x12
#define VK_ESCAPE           0x1B
#define VK_SPACE            0x20
#define VK_PAGE_UP          0x21
#define VK_PAGE_DOWN        0x22
#define VK_END              0x23
#define VK_HOME             0x24
#define VK_LEFT             0x25
#define VK_UP               0x26
#define VK_RIGHT            0x27
#define VK_DOWN             0x28
#define VK_INSERT           0x2D
#define VK_DELETE           0x2E
#define VK_NUMPAD0          0x60
#define VK_NUMPAD1          0x61
#define VK_NUMPAD2          0x62
#define VK_NUMPAD3          0x63
#define VK_NUMPAD4          0x64
#define VK_NUMPAD5          0x65
#define VK_NUMPAD6          0x66
#define VK_NUMPAD7          0x67
#define VK_NUMPAD8          0x68
#define VK_NUMPAD9          0x69
#define VK_MULTIPLY         0x6A
#define VK_ADD              0x6B
#define VK_SEPARATOR        0x6C
#define VK_SUBTRACT         0x6D
#define VK_DECIMAL          0x6E
#define VK_DIVIDE           0x6F
#define VK_LBUTTONDBLCLK    0x0201 // Fallback-Definition für Doppelklick falls nicht vorhanden
#endif

// Spezielle Symbole (Plus/Minus/Comma/Period) sind im Win32-Standard oft 
// tastaturlayoutabhängig oder als OEM definiert. Falls sie fehlen, mappen wir sie sauber:
#ifndef VK_PLUS
#define VK_PLUS             0x6B
#endif
#ifndef VK_MINUS
#define VK_MINUS            0x6D
#endif
#ifndef VK_OEM_COMMA
#define VK_OEM_COMMA        0xBC
#endif
#ifndef VK_OEM_PERIOD
#define VK_OEM_PERIOD       0xBE
#endif

#ifndef VK_F1
#define VK_F1               0x70
#define VK_F2               0x71
#define VK_F3               0x72
#define VK_F4               0x73
#define VK_F5               0x74
#define VK_F6               0x75
#define VK_F7               0x76
#define VK_F8               0x77
#define VK_F9               0x78
#define VK_F10              0x79
#define VK_F11              0x7A
#define VK_F12              0x7B
#define VK_F13              0x7C
#define VK_F14              0x7D
#define VK_F15              0x7E
#define VK_F16              0x7F
#define VK_F17              0x80
#define VK_F18              0x81
#define VK_F19              0x82
#define VK_F20              0x83
#define VK_F21              0x84
#define VK_F22              0x85
#define VK_F23              0x86
#define VK_F24              0x87
#endif

//-----------------------------------------------------------------
/*
struct POINT {
    long x;
    long y;
};

struct SIZE {
    long cx;
    long cy;
};

struct RECT {
    long left;
    long top;
    long right;
    long bottom;
};

using LPCRECT = const RECT*;
using LPRECT = RECT*;
using LPPOINT = POINT*;
using LPSIZE = SIZE*;
*/
//-----------------------------------------------------------------

struct DSize {
    int cx{0};
    int cy{0};

    // Konstruktoren für bequemes Initialisieren
    constexpr DSize() noexcept = default;
    constexpr DSize(int width, int height) noexcept : cx(width), cy(height) {}

    // Hilfsmethoden
    constexpr bool IsEmpty() const noexcept { return cx <= 0 || cy <= 0; }

    // C++20 Standard-Gleichheitsoperator
    constexpr bool operator==(const DSize&) const = default;
    
#ifdef QT_CORE_LIB
    // Nahtlose Konvertierung von/zu QSize (falls Qt6 eingebunden ist)
    constexpr DSize(const QSize& qsize) noexcept : cx(qsize.width()), cy(qsize.height()) {}
    constexpr operator QSize() const noexcept { return QSize(cx, cy); }
#endif

};

//-----------------------------------------------------------------

struct DPoint {
    int x{0};
    int y{0};

    // --- Konstruktoren ---
    constexpr DPoint() noexcept = default;
    constexpr DPoint(int xPos, int yPos) noexcept : x(xPos), y(yPos) {}


    // --- C++20 Vergleichsoperatoren ---
    constexpr bool operator==(const DPoint&) const noexcept = default;

    // --- Grundlegende Arithmetik (WTL/Win32-Kompatibilität) ---
    constexpr DPoint& operator+=(const DPoint& rhs) noexcept {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }

    constexpr DPoint& operator-=(const DPoint& rhs) noexcept {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    constexpr DPoint operator+(const DPoint& rhs) const noexcept {
        return DPoint(x + rhs.x, y + rhs.y);
    }

    constexpr DPoint operator-(const DPoint& rhs) const noexcept {
        return DPoint(x - rhs.x, y - rhs.y);
    }

    constexpr DPoint operator-() const noexcept {
        return DPoint(-x, -y);
    }

    // --- Qt6 Interoperabilität ---
#ifdef QT_CORE_LIB
    constexpr DPoint(const QPoint& qpoint) noexcept : x(qpoint.x()), y(qpoint.y()) {}
    constexpr operator QPoint() const noexcept { return QPoint(x, y); }
#endif
};

//-----------------------------------------------------------------

struct DRect {
    int left{0};
    int top{0};
    int right{0};
    int bottom{0};

    // --- Konstruktoren ---
    constexpr DRect() noexcept = default;
    constexpr DRect(int l, int t, int r, int b) noexcept
        : left(l), top(t), right(r), bottom(b) {}

    constexpr DRect(const DPoint& point, const DSize& size) noexcept
        : left(point.x), top(point.y), right(point.x + size.cx), bottom(point.y + size.cy) {}

#if defined(_WINDEF_)
    // Konvertierungskonstruktor für RECT / tagRECT
    constexpr DRect(const RECT& rc) noexcept
        : left(rc.left), top(rc.top), right(rc.right), bottom(rc.bottom) {}

    // Konvertierungskonstruktor für Zeiger (RECT* / LPCRECT)
    constexpr DRect(LPCRECT lprc) noexcept {
        if (lprc) {
            left = lprc->left; top = lprc->top;
            right = lprc->right; bottom = lprc->bottom;
        } else {
            SetRectEmpty();
        }
    }

    constexpr DRect(const POINT& point, const SIZE& size) noexcept
        : left(point.x), top(point.y), right(point.x + size.cx), bottom(point.y + size.cy) {}

    // Konvertierung nach RECT (Value)
    constexpr operator RECT() const noexcept {
        return RECT{ left, top, right, bottom };
    }
#endif

    // --- C++20 Vergleichsoperatoren ---
    constexpr bool operator==(const DRect&) const noexcept = default;

    // --- Abfragen & Eigenschaften ---
    constexpr int Width() const noexcept { return right - left; }
    constexpr int Height() const noexcept { return bottom - top; }
    constexpr DSize Size() const noexcept { return DSize(Width(), Height()); }
    constexpr DPoint TopLeft() const noexcept { return DPoint(left, top); }
    constexpr DPoint BottomRight() const noexcept { return DPoint(right, bottom); }

    // WTL / Win32 Kompatibilität
    constexpr bool IsEmpty() const noexcept { return right <= left || bottom <= top; }
    constexpr bool IsRectEmpty() const noexcept { return IsEmpty(); } // Exakte WTL-Schnittstelle
    constexpr bool IsRectNull() const noexcept { return left == 0 && top == 0 && right == 0 && bottom == 0; }

    constexpr bool PtInRect(const DPoint& pt) const noexcept {
        return (pt.x >= left && pt.x < right && pt.y >= top && pt.y < bottom);
    }

    constexpr bool PtInRect(int x, int y) const noexcept {
        return PtInRect(DPoint(x, y));
    }

    // --- Transformationen & Modifikatoren ---
    constexpr void SetRect(int l, int t, int r, int b) noexcept {
        left = l; top = t; right = r; bottom = b;
    }

    constexpr void SetRectEmpty() noexcept {
        left = top = right = bottom = 0;
    }

    constexpr void OffsetRect(int dx, int dy) noexcept {
        left += dx; right += dx;
        top += dy;  bottom += dy;
    }

    constexpr void OffsetRect(const DPoint& delta) noexcept {
        OffsetRect(delta.x, delta.y);
    }

    constexpr void InflateRect(int dx, int dy) noexcept {
        left -= dx; right += dx;
        top -= dy;  bottom += dy;
    }

    constexpr void DeflateRect(int dx, int dy) noexcept {
        InflateRect(-dx, -dy);
    }

    // --- Intersection / Union ---
    constexpr bool IntersectRect(const DRect& r1, const DRect& r2) noexcept {
        left   = std::max(r1.left, r2.left);
        top    = std::max(r1.top, r2.top);
        right  = std::min(r1.right, r2.right);
        bottom = std::min(r1.bottom, r2.bottom);

        if (IsEmpty()) {
            SetRectEmpty(); // Auf (0,0,0,0) zurücksetzen wenn keine Überlappung
            return false;
        }
        return true;
    }

};

//-----------------------------------------------------------------
using LPCTSTR = const wchar_t*;
using LPTSTR = wchar_t*;

using CSize = DSize;
using CPoint = DPoint;
using CRect = DRect;

#endif // WTLCOMPAT_H

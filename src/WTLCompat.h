#ifndef WTLCOMPAT_H
#define WTLCOMPAT_H

#include <algorithm>

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

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

using CSize = DSize;
using CPoint = DPoint;
using CRect = DRect;

#endif // WTLCOMPAT_H

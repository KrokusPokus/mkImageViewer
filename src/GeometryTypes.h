#ifndef GEOMETRYTYPES_H
#define GEOMETRYTYPES_H

#ifdef QT_CORE_LIB
#include <QPoint>
#include <QRect>
#include <QSize>
#endif

#include <algorithm>

struct DSize {
    int cx{0};
    int cy{0};

    // Konstruktoren für bequemes Initialisieren
    constexpr DSize() noexcept = default;
    constexpr DSize(int width, int height) noexcept : cx(width), cy(height) {}

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

    // --- Abfragen & Eigenschaften ---
    constexpr int Width() const noexcept { return right - left; }
    constexpr int Height() const noexcept { return bottom - top; }
    constexpr DSize Size() const noexcept { return DSize(Width(), Height()); }
    constexpr DPoint TopLeft() const noexcept { return DPoint(left, top); }

    constexpr bool IsEmpty() const noexcept { return right <= left || bottom <= top; }

    // --- Transformationen & Modifikatoren ---
    constexpr void SetRectEmpty() noexcept {
        left = top = right = bottom = 0;
    }

    constexpr void OffsetRect(int dx, int dy) noexcept {
        left += dx; right += dx;
        top += dy;  bottom += dy;
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

#endif // GEOMETRYTYPES_H

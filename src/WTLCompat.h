#ifndef WTLCOMPAT_H
#define WTLCOMPAT_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#ifdef QT_CORE_LIB
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#endif

//-----------------------------------------------------------------

struct DFileTime {
    std::uint64_t ticks{0}; // Millisekunden seit Unix-Epoche (1970)

    constexpr DFileTime() noexcept = default;
    constexpr explicit DFileTime(std::uint64_t ms) noexcept : ticks(ms) {}

    // Vergleiche
    constexpr bool operator==(const DFileTime&) const noexcept = default;
    constexpr auto operator<=>(const DFileTime&) const noexcept = default;

#if defined(_WIN32) || defined(FILETIME)
    // Automatische Konvertierung von / zu Win32 FILETIME
    operator FILETIME() const noexcept {
        // Umrechnung von Unix-Epoche (1970) auf Windows FILETIME (1601) in 100-Nanoticks
        std::uint64_t ft = (ticks * 10000ULL) + 116444736000000000ULL;
        FILETIME result;
        result.dwLowDateTime = static_cast<unsigned long>(ft & 0xFFFFFFFF);
        result.dwHighDateTime = static_cast<unsigned long>(ft >> 32);
        return result;
    }
#endif
};

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

#if defined(_WIN32) || defined(POINT)
    constexpr DPoint(const POINT& pt) noexcept : x(pt.x), y(pt.y) {}
    constexpr operator POINT() const noexcept { return POINT{x, y}; }
#endif

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

#if defined(_WIN32) || defined(POINT)
    constexpr DRect(const POINT& point, const SIZE& size) noexcept
        : left(point.x), top(point.y), right(point.x + size.cx), bottom(point.y + size.cy) {}
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

    // --- Qt6 Interoperabilität ---
#ifdef QT_CORE_LIB
    constexpr DRect(const QRect& qrect) noexcept
        : left(qrect.left()),
        top(qrect.top()),
        right(qrect.isNull() ? 0 : qrect.right() + 1),
        bottom(qrect.isNull() ? 0 : qrect.bottom() + 1) {}

    constexpr operator QRect() const noexcept {
        if (IsEmpty()) return QRect();
        return QRect(left, top, Width(), Height());
    }
#endif
};

//-----------------------------------------------------------------
using LPCTSTR = const wchar_t*;
using LPTSTR = wchar_t*;

class DString {
private:
    std::wstring m_data;

public:
    // --- 1. Konstruktoren ---
    DString() = default;
    DString(const wchar_t* str) : m_data(str ? str : L"") {}
    DString(const std::wstring& str) : m_data(str) {}
    DString(std::wstring&& str) noexcept : m_data(std::move(str)) {}
    DString(wchar_t ch, size_t repeat = 1) : m_data(repeat, ch) {}


    // --- Erweiterungen für CString(pString, nMaxChars) ---
    // 1. Breitzeichen (wchar_t*) mit expliziter Längenangabe (int & size_t)
    DString(const wchar_t* str, int length)
        : m_data((str && length > 0) ? std::wstring(str, static_cast<size_t>(length)) : L"") {}

    DString(const wchar_t* str, size_t length)
        : m_data((str && length > 0) ? std::wstring(str, length) : L"") {}

    // 2. Schmalzeichen (char* / ANSI) mit expliziter Längenangabe (falls pString ein char* ist)
    DString(const char* str, int length) {
        if (str && length > 0) {
            std::string s(str, static_cast<size_t>(length));
            m_data = std::wstring(s.begin(), s.end()); // einfache ANSI/ASCII-Konvertierung
        }
    }

    DString(const char* str, size_t length) {
        if (str && length > 0) {
            std::string s(str, length);
            m_data = std::wstring(s.begin(), s.end());
        }
    }

    // 3. Schmalzeichen (char*) ohne Längenangabe
    DString(const char* str) {
        if (str) {
            std::string s(str);
            m_data = std::wstring(s.begin(), s.end());
        }
    }

    // --- 2. WTL-Konvertierungsoperatoren (Das Herzstück) ---
    // Implizite Konvertierung zu LPCTSTR (const wchar_t*) für WTL-APIs und legacy Aufrufe
    operator LPCTSTR() const noexcept { return m_data.c_str(); }

    // Zugriff auf den zugrundeliegenden std::wstring & C-String
    const std::wstring& ToStdWString() const noexcept { return m_data; }
    const wchar_t* GetBuffer() const noexcept { return m_data.c_str(); }
    const wchar_t* getString() const noexcept { return m_data.c_str(); }

    // --- 3. WTL-Schnittstellen (Größe & Status) ---
    int GetLength() const noexcept { return static_cast<int>(m_data.length()); }
    bool IsEmpty() const noexcept { return m_data.empty(); }
    void Empty() noexcept { m_data.clear(); }

    // --- 4. Modifikatoren & Operationen ---
    void Append(const DString& str) { m_data += str.m_data; }
    void Append(LPCTSTR str) { if (str) m_data += str; }

    void Format(const wchar_t* format, ...) {
        va_list args;
        va_start(args, format);

        // Puffergröße ermitteln
        int size = std::vswprintf(nullptr, 0, format, args);
        if (size > 0) {
            std::vector<wchar_t> buf(size + 1);
            std::vswprintf(buf.data(), buf.size(), format, args);
            m_data = buf.data();
        }
        va_end(args);
    }
    int Replace(wchar_t chOld, wchar_t chNew) {
        int count = 0;
        for (auto& ch : m_data) {
            if (ch == chOld) { ch = chNew; count++; }
        }
        return count;
    }

    int ReverseFind(wchar_t ch) const noexcept {
        auto pos = m_data.rfind(ch);
        return (pos == std::wstring::npos) ? -1 : static_cast<int>(pos);
    }

    void MakeLower() {
        std::transform(m_data.begin(), m_data.end(), m_data.begin(), ::towlower);
    }
    void MakeUpper() {
        std::transform(m_data.begin(), m_data.end(), m_data.begin(), ::towupper);
    }

    wchar_t* GetBuffer(size_t minLength = 0) {
        if (minLength > m_data.size()) {
            m_data.resize(minLength);
        }
        return m_data.data(); // C++17 ermöglicht schreibbaren Zugriff über .data()
    }

    void ReleaseBuffer(int newLength = -1) {
        if (newLength < 0) {
            m_data.resize(std::wcslen(m_data.c_str()));
        } else {
            m_data.resize(static_cast<size_t>(newLength));
        }
    }

    DString& operator=(const wchar_t* str) {
        m_data = str ? str : L"";
        return *this;
    }
    DString& operator=(const std::wstring& str) {
        m_data = str;
        return *this;
    }
    DString& operator+=(const DString& rhs) {
        m_data += rhs.m_data;
        return *this;
    }
    DString& operator+=(wchar_t ch) {
        m_data += ch;
        return *this;
    }

    wchar_t operator[](int index) const { return m_data[index]; }
    wchar_t& operator[](int index) { return m_data[index]; }

    // --- 5. WTL-Stringoperationen (Left, Right, Mid, CompareNoCase) ---
    DString Left(int count) const {
        if (count <= 0) return DString();
        if (count >= GetLength()) return *this;
        return DString(m_data.substr(0, count));
    }

    DString Right(int count) const {
        if (count <= 0) return DString();
        if (count >= GetLength()) return *this;
        return DString(m_data.substr(m_data.length() - count));
    }

    DString Mid(int start, int count = -1) const {
        if (start < 0 || start >= GetLength()) return DString();
        if (count < 0 || (start + count) > GetLength()) {
            return DString(m_data.substr(start));
        }
        return DString(m_data.substr(start, count));
    }

    int CompareNoCase(LPCTSTR str) const noexcept {
        if (!str) return IsEmpty() ? 0 : 1;

        std::wstring s1 = m_data;
        std::wstring s2 = str;
        std::transform(s1.begin(), s1.end(), s1.begin(), ::towlower);
        std::transform(s2.begin(), s2.end(), s2.begin(), ::towlower);
        return s1.compare(s2);
    }

    int Find(wchar_t ch, int start = 0) const noexcept {
        auto pos = m_data.find(ch, start);
        return (pos == std::wstring::npos) ? -1 : static_cast<int>(pos);
    }

    int Find(LPCTSTR str, int start = 0) const noexcept {
        if (!str) return -1;
        auto pos = m_data.find(str, start);
        return (pos == std::wstring::npos) ? -1 : static_cast<int>(pos);
    }

    // --- 6. C++20 Vergleichsoperatoren ---
    bool operator==(const DString& rhs) const noexcept { return m_data == rhs.m_data; }
    bool operator==(LPCTSTR rhs) const noexcept { return rhs ? m_data == rhs : m_data.empty(); }
    auto operator<=>(const DString& rhs) const noexcept { return m_data <=> rhs.m_data; }

    // --- 7. Qt6 Interoperabilität ---
#ifdef QT_CORE_LIB
    DString(const QString& qstr) : m_data(qstr.toStdWString()) {}
    operator QString() const { return QString::fromStdWString(m_data); }
#endif
};

// Globaler Operator+ für Verknüpfungen
inline DString operator+(const DString& lhs, const DString& rhs) {
    DString res = lhs;
    res += rhs;
    return res;
}

inline DString operator+(const DString& lhs, LPCTSTR rhs) {
    DString res = lhs;
    res += rhs;
    return res;
}

//-----------------------------------------------------------------

namespace fs = std::filesystem;

class DFindFile {
private:
    std::vector<fs::directory_entry> m_matches;
    size_t m_currentIndex{0};
    fs::directory_entry m_currentEntry;
    bool m_bFound{false};

    // Hilfsmethode für Wildcard-Matching (*.png, *.txt, etc.)
    static bool MatchPattern(const std::wstring& filename, const std::wstring& pattern) {
        if (pattern == L"*" || pattern == L"*.*" || pattern.empty()) {
            return true;
        }

        // Einfacher Endungs-Match (*.ext)
        if (pattern.front() == L'*') {
            std::wstring ext = pattern.substr(1); // z.B. ".png"
            if (filename.size() >= ext.size()) {
                return filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0;
            }
            return false;
        }

        return filename == pattern;
    }

public:
    DFindFile() = default;
    ~DFindFile() { Close(); }

    // --- 1. Suchen starten & durchsteigen ---
    bool FindFile(DString pstrName = L"*.*") {
        Close();

        fs::path searchPath(pstrName.ToStdWString());
        fs::path dir = searchPath.parent_path();
        std::wstring pattern = searchPath.filename().wstring();

        // Wenn kein Ordner angegeben wurde, im aktuellen Verzeichnis suchen
        if (dir.empty()) {
            dir = fs::current_path();
        }

        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return false;
        }

        // Alle passenden Dateien im Ordner einsammeln
        try {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (MatchPattern(entry.path().filename().wstring(), pattern)) {
                    m_matches.push_back(entry);
                }
            }
        } catch (...) {
            return false;
        }

        if (m_matches.empty()) {
            return false;
        }

        m_currentIndex = 0;
        m_currentEntry = m_matches[0];
        m_bFound = true;
        return true;
    }

    bool FindNextFile() {
        if (!m_bFound) {
            return false;
        }

        m_currentIndex++;
        if (m_currentIndex < m_matches.size()) {
            m_currentEntry = m_matches[m_currentIndex];
            return true;
        }

        // Ende der Liste erreicht
        m_bFound = false;
        return false;
    }

    void Close() {
        m_matches.clear();
        m_currentIndex = 0;
        m_currentEntry = fs::directory_entry();
        m_bFound = false;
    }

    // --- 2. Eigenschaften abfragen (WTL CFindFile Interface) ---
    DString GetFilePath() const {
        return DString(m_currentEntry.path().wstring());
    }

    DString GetFileName() const {
        return DString(m_currentEntry.path().filename().wstring());
    }

    DString GetFileTitle() const {
        return DString(m_currentEntry.path().stem().wstring());
    }

    DString GetFileURL() const {
        return DString(L"file://" + m_currentEntry.path().wstring());
    }

    DString GetRoot() const {
        return DString(m_currentEntry.path().parent_path().wstring());
    }

    // --- 3. Attribute & Status ---
    bool IsDots() const {
        std::wstring name = m_currentEntry.path().filename().wstring();
        return name == L"." || name == L"..";
    }

    bool IsDirectory() const {
        return m_currentEntry.is_directory();
    }

    bool IsSystem() const {
        return false; // Auf nicht-Windows Systemen typischerweise unrelevant
    }

    bool IsHidden() const {
        // Unter Linux/macOS beginnen versteckte Dateien mit einem Punkt
        std::wstring name = m_currentEntry.path().filename().wstring();
        return !name.empty() && name.front() == L'.';
    }

    bool IsReadOnly() const {
        auto perms = m_currentEntry.status().permissions();
        return (perms & fs::perms::owner_write) == fs::perms::none;
    }

    uint64_t GetLength() const {
        if (m_currentEntry.is_regular_file()) {
            return m_currentEntry.file_size();
        }
        return 0;
    }
    uint64_t GetFileSize() const {
        return GetLength();
    }

    // --- 1. Parameterlose Variante (C++17 Style) ---
    DFileTime GetLastWriteTime() const {
        if (m_bFound && m_currentEntry.exists()) {
            try {
                auto ftime = m_currentEntry.last_write_time();
                auto s_tp = std::chrono::time_point_cast<std::chrono::milliseconds>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                return DFileTime{ static_cast<uint64_t>(s_tp.time_since_epoch().count()) };
            } catch (...) {}
        }
        return DFileTime{0};
    }

    // --- 2. WTL / MFC Kompatibilitäts-Überladungen mit 1 Argument ---

    // A) Wenn ein Zeiger übergeben wird: finder.GetLastWriteTime(&fileTime)
    template <typename T>
    bool GetLastWriteTime(T* pTimeStamp) const {
        if (!pTimeStamp) return false;
        *pTimeStamp = static_cast<T>(GetLastWriteTime());
        return m_bFound;
    }

    // B) Wenn eine Referenz übergeben wird: finder.GetLastWriteTime(fileTime)
    template <typename T>
    bool GetLastWriteTime(T& refTimeStamp) const {
        refTimeStamp = static_cast<T>(GetLastWriteTime());
        return m_bFound;
    }

    // --- 3. Das Gleiche für GetCreationTime ---
    DFileTime GetCreationTime() const {
        return GetLastWriteTime();
    }

    template <typename T>
    bool GetCreationTime(T* pTimeStamp) const {
        return GetLastWriteTime(pTimeStamp);
    }

    template <typename T>
    bool GetCreationTime(T& refTimeStamp) const {
        return GetLastWriteTime(refTimeStamp);
    }
};

//-----------------------------------------------------------------

using FILETIME_COMPAT = DFileTime;
using CSize = DSize;
using CPoint = DPoint;
using CRect = DRect;
using CString = DString;
using CStringW = DString;
using CFindFile = DFindFile;

#endif // WTLCOMPAT_H

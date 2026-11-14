#include "Helpers.h"

#include "SettingsProvider.h"

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringDecoder>
#include <QTextStream>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>

namespace {

    constexpr int kMaxInputLength = 8192;

    // Entfernt unsichtbare/steuernde Unicode-Zeichen, die präparierte Webseiten in
    // kopierten Text einschleusen können (Zero-Width-Chars, Bidi-Override wie bei
    // "Trojan Source"-Angriffen, BOM). Echte RTL-Schrift (Arabisch/Hebräisch) ist
    // davon NICHT betroffen - das sind normale Buchstaben, keine Steuerzeichen.
    QString stripInvisibleAndControlChars(const QString &input) {
        QString out;
        out.reserve(input.size());
        for (QChar c : input) {
            const ushort u = c.unicode();
            if (c == '\n' || c == '\r' || c == '\t') { out += c; continue; }
            if (u < 0x20 || (u >= 0x7F && u <= 0x9F)) continue;
            if (u == 0x200B || u == 0x200C || u == 0x200D || u == 0xFEFF ||
                (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069))
                continue;
            out += c;
        }
        return out;
    }

    // Ein einziger Durchlauf über 'input': ersetzte Werte werden NICHT erneut gescannt,
    // daher ist eine Verkettung ($FOO expandiert zu einem String, der wieder ${...} enthält
    // und erneut aufgelöst wird) strukturell unmöglich, nicht nur begrenzt.
    QString expandVariablesOnce(const QString &input,
                                const QRegularExpression &regex,
                                const std::function<QString(const QRegularExpressionMatch&)> &resolve) {

        auto it = regex.globalMatch(input);

        if (!it.hasNext()) {
            return input;
        }

        QString result;
        result.reserve(input.size());
        qsizetype lastEnd = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            result += input.sliced(lastEnd, m.capturedStart() - lastEnd);
            result += resolve(m);
            lastEnd = m.capturedEnd();
        }
        result += input.sliced(lastEnd);
        return result;
    }

    // Expandiert eine einzelne Zeile rein im Speicher
    QString expandSingleLine(QString line) {
        line = line.trimmed();
        if (line.isEmpty()) return QString();

        // 1. Anführungszeichen entfernen
        if (line.length() >= 2 &&
            ((line.startsWith('"') && line.endsWith('"')) ||
             (line.startsWith('\'') && line.endsWith('\''))))
        {
            line = line.mid(1, line.length() - 2).trimmed();
            if (line.isEmpty()) return QString();
        }

        // 2. file:// URLs in lokale Pfade umwandeln
        QUrl url(line);
        if (url.isLocalFile()) {
            line = url.toLocalFile();
        }

        if (line == "~") {
            line = QDir::homePath();
        } else if (line.startsWith("~/")) {
            line = QDir::homePath() + line.mid(1);
        }

        static const QRegularExpression posixEnvRegex(R"(\$\{([A-Za-z0-9_]+)\}|\$([A-Za-z0-9_]+))");
        line = expandVariablesOnce(line, posixEnvRegex, [](const QRegularExpressionMatch &m) {
            const QString varName = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            if (varName == "HOME") return QDir::homePath();
            const QByteArray name = varName.toUtf8();
            if (!qEnvironmentVariableIsSet(name.constData())) return m.captured(0);
            return qEnvironmentVariable(name.constData());
        });

        return QDir::cleanPath(line);
    }


} // namespace

namespace {
// ---------------------------------------------------------------------------------------------
// Exec-Zeile nach der Desktop Entry Specification zerlegen und Feldcodes ersetzen
// https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
// ---------------------------------------------------------------------------------------------

struct ExecToken {
    QString text;
    bool quoted = false; // Argument stand (ganz oder teilweise) in "..."
};

// Stufe 1: String-Escapes, die für jeden Wert vom Typ "string" gelten (\s \n \t \r \\)
QString unescapeDesktopString(const QString &in) {
    QString out;
    out.reserve(in.size());
    for (qsizetype i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == '\\' && i + 1 < in.size()) {
            const QChar n = in.at(i + 1);
            if (n == 's')  { out += ' ';  ++i; continue; }
            if (n == 'n')  { out += '\n'; ++i; continue; }
            if (n == 't')  { out += '\t'; ++i; continue; }
            if (n == 'r')  { out += '\r'; ++i; continue; }
            if (n == '\\') { out += '\\'; ++i; continue; }
        }
        out += c;
    }
    return out;
}

// Stufe 2: In Argumente zerlegen. Trenner sind Leerzeichen; in "..." sind \" \` \$ \\ Escapes.
// Unbekannte Escapes bleiben aus Kulanz wörtlich stehen.
QList<ExecToken> splitDesktopExec(const QString &exec) {
    QList<ExecToken> tokens;
    ExecToken current;
    bool inQuotes = false;
    bool hasToken = false;

    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (inQuotes) {
            if (c == '\\' && i + 1 < exec.size() && QStringLiteral("\"`$\\").contains(exec.at(i + 1))) {
                current.text += exec.at(++i);
            } else if (c == '"') {
                inQuotes = false;
            } else {
                current.text += c;
            }
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (hasToken) {
                tokens << current;
                current = ExecToken();
                hasToken = false;
            }
        } else if (c == '"') {
            inQuotes = true;
            current.quoted = true;
            hasToken = true;
        } else {
            current.text += c;
            hasToken = true;
        }
    }
    if (hasToken) tokens << current;
    return tokens;
}

// Enthält das Argument einen der Feldcodes aus 'codes' (z.B. "fu")? %% wird korrekt übersprungen.
bool containsFieldCode(const QString &token, const QString &codes) {
    for (qsizetype i = 0; i + 1 < token.size(); ++i) {
        if (token.at(i) != '%') continue;
        if (codes.contains(token.at(i + 1))) return true;
        ++i; // "%%" oder anderer Code -> nächstes Zeichen überspringen
    }
    return false;
}

// Ersetzt die Feldcodes innerhalb eines Arguments. Rückgabe false = Argument entfällt
// (%f/%u ohne Datei, sowie %F/%U/%i, die nur als eigenständiges Argument gültig sind).
bool expandFieldCodes(const QString &token, const QString *file, const DesktopEntry &entry, QString &out) {
    out.clear();
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token.at(i);
        if (c != '%' || i + 1 >= token.size()) {
            out += c;
            continue;
        }
        const QChar code = token.at(++i);
        switch (code.unicode()) {
        case '%': out += '%'; break;
        case 'f':
        case 'u':
            if (!file) return false;
            out += *file;
            break;
        case 'c': out += entry.name; break;
        case 'k': out += QFileInfo(entry.path).absoluteFilePath(); break;
        case 'd': case 'D': case 'n': case 'N': case 'v': case 'm':
            break; // veraltet -> entfernen
        case 'F': case 'U': case 'i':
            return false;
        default:
            out += '%';
            out += code;
            break;
        }
    }
    return true;
}
} // namespace

namespace Helpers {

    namespace {
        // Reads the first number of a small text file (e.g. a cgroup value), -1 if missing or not a number ("max")
        qint64 ReadNumberFile(const QString& sPath) {
            QFile f(sPath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return -1;
            bool bOk = false;
            const qint64 nValue = f.readLine().trimmed().toLongLong(&bOk);
            return bOk ? nValue : -1;
        }

        // Value of a line of /proc/meminfo (e.g. "MemAvailable:") in bytes, -1 if missing.
        // Files in /proc report the size 0, so QFile::atEnd() is true right away: read everything, then split.
        qint64 ReadMeminfoBytes(const char* pKey) {
            QFile meminfo(QStringLiteral("/proc/meminfo"));
            if (!meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) return -1;
            const int nKeyLength = static_cast<int>(strlen(pKey));
            for (const QByteArray& line : meminfo.readAll().split('\n')) {
                if (line.startsWith(pKey)) {
                    return line.mid(nKeyLength).trimmed().split(' ').first().toLongLong() * 1024;  // given in kB
                }
            }
            return -1;
        }

        // cgroup v2: the line "0::/path" of /proc/self/cgroup. Every level up to the root can have a memory limit
        // (memory.max, "max" = none). Calls fn(nMax, nCurrent) for each level that has one.
        template <class Fn>
        void ForEachCgroupMemoryLimit(Fn fn) {
            QFile cgroup(QStringLiteral("/proc/self/cgroup"));
            if (!cgroup.open(QIODevice::ReadOnly | QIODevice::Text)) return;
            for (QByteArray line : cgroup.readAll().split('\n')) {
                line = line.trimmed();
                if (!line.startsWith("0::")) continue;
                QString sPath = QString::fromUtf8(line.mid(3));
                while (!sPath.isEmpty()) {
                    const QString sDir = QStringLiteral("/sys/fs/cgroup") + sPath;
                    const qint64 nMax = ReadNumberFile(sDir + QStringLiteral("/memory.max"));
                    const qint64 nCurrent = ReadNumberFile(sDir + QStringLiteral("/memory.current"));
                    if (nMax >= 0 && nCurrent >= 0) {
                        fn(nMax, nCurrent);
                    }
                    if (sPath == QStringLiteral("/")) break;
                    sPath = sPath.left(sPath.lastIndexOf(QLatin1Char('/')));
                    if (sPath.isEmpty()) sPath = QStringLiteral("/");
                }
                break;
            }
        }
    }

    qint64 AvailableMemoryBytes() {
        qint64 nAvailable = ReadMeminfoBytes("MemAvailable:");
        if (nAvailable < 0) {
            return -1;
        }
        // the smallest remaining amount of all cgroup limits (limit - current usage) counts
        ForEachCgroupMemoryLimit([&nAvailable](qint64 nMax, qint64 nCurrent) {
            nAvailable = std::min(nAvailable, std::max<qint64>(0, nMax - nCurrent));
        });
        return nAvailable;
    }

    qint64 TotalMemoryBytes() {
        // Read once (on the first call, i.e. at the first read ahead): the total does not change while the app runs.
        // The initialization of a local static is thread safe.
        static const qint64 nTotal = [] {
            qint64 n = ReadMeminfoBytes("MemTotal:");
            if (n < 0) {
                return qint64(-1);
            }
            ForEachCgroupMemoryLimit([&n](qint64 nMax, qint64) {
                n = std::min(n, nMax);
            });
            return n;
        }();
        return nTotal;
    }

    bool StillEnoughFree() {
        const qint64 nAvailable = AvailableMemoryBytes();
        const qint64 nTotal = TotalMemoryBytes();
        if (nAvailable < 0 || nTotal <= 0) {
            return true;    // not known (no Linux): no limit, as before
        }
        return nAvailable * 100 >= nTotal * READ_AHEAD_MIN_FREE_PERCENT;
    }
    
    int ScaleWithDPI(int value)
    {
        return (int)(value * DPIScalingFactor + 0.5f);
    }

    QSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, 
                        bool bAllowZoomIn, bool bFillCrop, bool bLimitAR, EAutoZoomMode eAutoZoomMode, double & dZoom) {
        CSettingsProvider& sp = CSettingsProvider::This();
        int nBookModePageHeight = sp.BookModePageHeight();

        if (eAutoZoomMode == ZM_BookMode) {
            double dImageAR = (double)nWidth/nHeight;
            if (dImageAR >= 1.0) {                        // If it's wider than high, assume a double page
                dZoom = (double)nScreenWidth/nWidth;    //    ... and Fit to screen width
            } else {                                    // else, assume a single page
                double dAR1 = ((double)(nHeight*100) / (nBookModePageHeight * nScreenHeight));    // Zoom to user specified height
                double dAR2 = (double)nWidth/nScreenWidth;                                        //    ...but limit maximum image width to window width
                double dAR = std::max(dAR1, dAR2);
                dZoom = 1.0/dAR;
            }

        } else if (eAutoZoomMode == ZM_None) {
            dZoom = 1.0;
        } else {
            double dAR1 = (double)nWidth/nScreenWidth;
            double dAR2 = (double)nHeight/nScreenHeight;
            double dARMin = std::min(dAR1, dAR2);
            double dARMax = std::max(dAR1, dAR2);
            double dAR = bFillCrop ? dARMin : dARMax;

            if (dAR <= 1.0 && !bAllowZoomIn) {
                if (bFillCrop) dAR = dARMax;
                if (dAR <= 1.0) {
                    dZoom = 1.0;
                    return QSize(nWidth, nHeight);
                }
            }
            if (bFillCrop && bLimitAR) {
                if (((nWidth/dAR * nHeight/dAR) / (nScreenWidth * nScreenHeight)) > 1.5) {
                    dAR = dARMax; // use fit to screen
                }
            }
            dZoom = 1.0/dAR;
            dZoom = std::min(ZoomMax, dZoom);
        }

        return QSize((int)(nWidth * dZoom + 0.5), (int)(nHeight * dZoom + 0.5));
    }

    QSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, EAutoZoomMode eAutoZoomMode, double & dZoom) {

        // Do not comment this out! newSize is unused, but GetImageRect() is needed to calculate dZoom!
        GetImageRect(nWidth, nHeight, nScreenWidth, nScreenHeight,
            eAutoZoomMode == ZM_FitToScreen || eAutoZoomMode == ZM_FillScreen,
            eAutoZoomMode == ZM_FillScreenNoZoom || eAutoZoomMode == ZM_FillScreen,
            eAutoZoomMode == ZM_FillScreenNoZoom, eAutoZoomMode, dZoom);
            
        return QSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
    }

    const void* FindJPEGMarker(const void* pJPEGStream, size_t nStreamLength, unsigned char nMarker) {
        const uint8_t* pStream = static_cast<const uint8_t*>(pJPEGStream);
        if (pStream == nullptr || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
            return nullptr; // not a JPEG
        }
        size_t nIndex = 2;
        do {
            if (pStream[nIndex] == 0xFF) {
                // block header found, skip padding bytes
                while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
                if (nIndex >= nStreamLength) break;

                if (pStream[nIndex] == 0 || pStream[nIndex] == nMarker) {
                    break; // 0xFF 0x00 is part of pixel block, break
                } else {
                    // it's a block marker, read length of block and skip the block
                    nIndex++;
                    if (nIndex + 1 < nStreamLength) {
                        nIndex += pStream[nIndex] * 256 + pStream[nIndex + 1];
                    } else {
                        nIndex = nStreamLength;
                    }
                }
            } else {
                break; // block with pixel data found
            }
        } while (nIndex < nStreamLength);

        if (nIndex < nStreamLength && (nMarker == 0 || (pStream[nIndex] == nMarker && pStream[nIndex - 1] == 0xFF))) {
            return &(pStream[nIndex - 1]); // place on marker start
        } else {
            return nullptr;
        }
    }

    void* FindEXIFBlock(void* pJPEGStream, size_t nStreamLength) {
        uint8_t* pEXIFBlock = (uint8_t*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xE1);
        if (pEXIFBlock == nullptr) {
            return nullptr;
        }
        // The segment (marker, length, "Exif\0\0", data) must be completely inside of the stream: the length comes from
        // the file, and CBaseImage copies as many bytes as it says
        // FindJPEGMarker() returns a pointer into the stream, so the offset is >= 0 and < nStreamLength
        const size_t nAvailable = nStreamLength - static_cast<size_t>(pEXIFBlock - static_cast<uint8_t*>(pJPEGStream));
        if (nAvailable < 10) {
            return nullptr;
        }
        const size_t nSegmentSize = pEXIFBlock[2] * 256u + pEXIFBlock[3] + 2u;
        if (nSegmentSize < 10 || nSegmentSize > nAvailable || memcmp(pEXIFBlock + 4, "Exif", 4) != 0) {
            return nullptr;
        }
        return pEXIFBlock;
    }

// Versucht streng als validen UTF-8-String zu dekodieren (entspricht TryConvertFromUTF8)
QString TryConvertFromUTF8(const uint8_t* pComment, int nLengthInBytes) {
    auto utf8Decoder = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    
    // Dekodiert den Puffer als UTF-8
    QString result = utf8Decoder(QByteArrayView(reinterpret_cast<const char*>(pComment), nLengthInBytes));

    // Falls ungültige Byte-Sequenzen enthalten waren, schlägt die Validierung fehl
    if (utf8Decoder.hasError()) {
        return {};
    }

    return result;
}

QString GetJPEGComment(const void* pJPEGStream, size_t nStreamLength) {
    const uint8_t* pCommentSeg = static_cast<const uint8_t*>(FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE));
    if (!pCommentSeg) {
        return {};
    }
    
    // marker (2), length (2), comment: the comment must be completely inside of the stream (the length comes from the file)
    // FindJPEGMarker() returns a pointer into the stream, so the offset is >= 0 and < nStreamLength
    const size_t nAvailable = nStreamLength - static_cast<size_t>(pCommentSeg - static_cast<const uint8_t*>(pJPEGStream));
    if (nAvailable < 4) {
        return {};
    }
    pCommentSeg += 2;
    const int nCommentLen = pCommentSeg[0] * 256 + pCommentSeg[1] - 2;
    if (nCommentLen <= 0 || static_cast<size_t>(nCommentLen) > nAvailable - 4) {
        return {};
    }

    const uint8_t* pComment = &(pCommentSeg[2]);

    if (nCommentLen > 2 && (nCommentLen & 1) == 0) {
        // UTF-16 Little Endian (BOM 0xFF 0xFE)
        if (pComment[0] == 0xFF && pComment[1] == 0xFE) {
            return QString::fromUtf16(reinterpret_cast<const char16_t*>(&(pComment[2])), (nCommentLen - 2) / 2);
        } 
        // UTF-16 Big Endian (BOM 0xFE 0xFF)
        else if (pComment[0] == 0xFE && pComment[1] == 0xFF) {
            // Optional: Wenn Big-Endian doch unterstützt werden soll, dank Qt6 sehr einfach:
            // return QString::fromUcs2(reinterpret_cast<const char16_t*>(&(pComment[2])), (nCommentLen - 2) / 2, QSysInfo::BigEndian);
            return {};
        }
    }

    // 1. Versuch: UTF-8 Dekodierung
    const QString sConvertedFromUTF8 = TryConvertFromUTF8(pComment, nCommentLen);
    if (!sConvertedFromUTF8.isEmpty()) {
        return sConvertedFromUTF8.contains(QStringLiteral("Intel(R) JPEG Library")) ? QString() : sConvertedFromUTF8;
    }

    // 2. Fallback: ANSI / ASCII Plausibilitätsprüfung
    if (nCommentLen < 7) {
        return {};
    }

    int nGoodChars = 0;
    for (int i = 0; i < nCommentLen; ++i) {
        const uint8_t ch = pComment[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || 
            (ch >= '0' && ch <= '9') || ch == ' ' || ch == ',' || ch == '.') {
            nGoodChars++;
        }
    }

    // Filter für Intel JPEG Library
    const QByteArrayView commentView(reinterpret_cast<const char*>(pComment), nCommentLen);
    if (nCommentLen > 20 && commentView.contains("Intel(R) JPEG Library")) {
        return {};
    }

    // Latin-1 / ANSI Fallback, falls mehr als 80% der Zeichen lesbar sind
    return (nGoodChars > nCommentLen * 8 / 10) 
        ? QString::fromLatin1(reinterpret_cast<const char*>(pComment), nCommentLen) 
        : QString();
}

    // Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
    int GetFrameIndex(const CImagePosition& position, bool bImageLoaded, int iOffset, bool bSwitchFrame, bool & switchImage) {
        const int nNumberOfFrames = position.FrameCount;
        bool isMultiFrame = nNumberOfFrames > 1;
        bool isAnimation = position.IsAnimation;
        bool bInArchive = position.InArchive;
        int nFrameIndex = 0;
        switchImage = true;
        if (isMultiFrame) {
            switchImage = false;
            nFrameIndex = position.FrameIndex + iOffset;
            if (isAnimation) {
                if (bSwitchFrame) {
                    if (nFrameIndex < 0) {
                        nFrameIndex = nFrameIndex + nNumberOfFrames;
                    } else if (nFrameIndex > nNumberOfFrames - 1) {
                        nFrameIndex = nFrameIndex - nNumberOfFrames;
                    }
                } else {
                    switchImage = true;
                    nFrameIndex = 0;
                }
            } else if (bInArchive) {
                if (nFrameIndex < 0) {
                    nFrameIndex = nFrameIndex + nNumberOfFrames;
                } else if (nFrameIndex > nNumberOfFrames - 1) {
                    nFrameIndex = nFrameIndex - nNumberOfFrames;
                }
            } else {
                if (nFrameIndex < 0 || nFrameIndex >= nNumberOfFrames) {
                    nFrameIndex = 0;
                    switchImage = true;
                }
            }
        }
        if (bSwitchFrame && !bImageLoaded) {
            switchImage = false; // never switch image when error during animation playing
        }

        return nFrameIndex;
    }

// Based on: https://github.com/sdneon/jpegview.git
    void BlendAlpha(uint32_t* pImage32, int nWidth, int nHeight, Helpers::ETransparencyMode nTransparencyMode) {
        QColor cT = CSettingsProvider::This().ColorTransparency();
        QRgb nTransparency = qRgb(cT.red(), cT.green(), cT.blue());

        const size_t totalPixels = static_cast<size_t>(nWidth) * nHeight;

        if (nTransparencyMode != Helpers::TP_CHECKERBOARD) {
            if (nTransparencyMode == Helpers::TP_BLEND_INVERSE) {
                nTransparency = ~(nTransparency & 0x00FFFFFF);
            }

            for (size_t i = 0; i < totalPixels; ++i) {
                *pImage32 = AlphaBlendBackground(*pImage32, nTransparency);
                pImage32++;
            }
        } else {
            for (int y = 0; y < nHeight; ++y) {
                for (int x = 0; x < nWidth; ++x) {
                    *pImage32 = AlphaBlendBackground(*pImage32, nTransparency, true, x, y);
                    pImage32++;
                }
            }
        }
    }

    uint64_t GetTimeMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }


    DesktopEntry getDesktopEntryById(const QString &id) {
        static const QStringList appDirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
        for (const QString &dirPath : appDirs) {
            QFileInfo fileInfo(QDir(dirPath).filePath(id));
            DesktopEntry entry = getDesktopEntry(fileInfo);
            if (entry.isValid) {
                return entry;
            }
        }
        return DesktopEntry();
    }

    DesktopEntry getDesktopEntry(const QFileInfo &fileInfo) {

        static const QString localeName = QLocale::system().name().left(2); // "de", "fr", ...
        static const QString localKey = QString("Name[%1]=").arg(localeName);

        DesktopEntry currentEntry;

        QFile file(fileInfo.filePath());
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            in.setEncoding(QStringConverter::Utf8);

            QString iniName, iniNameLocalised;
            bool bInMainSection = false;

            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();

                if (line.startsWith('[') && line.endsWith(']')) {
                    if (bInMainSection) break;
                    bInMainSection = (line == "[Desktop Entry]");
                    continue;
                }

                if (!bInMainSection || line.startsWith('#')) continue;

                if (line.startsWith("Name=")) iniName = line.mid(5);
                else if (line.startsWith(localKey)) iniNameLocalised = line.mid(localKey.size());
                else if (line.startsWith("Type=")) currentEntry.type = line.mid(5);
                // URL (fängt sowohl "URL=" als auch "URL[$e]=" ab)
                else if (line.startsWith("URL=") || line.startsWith("URL[")) {
                    int idx = line.indexOf('=');
                    if (idx != -1) currentEntry.url = line.mid(idx + 1).trimmed();
                }
                else if (line.startsWith("Icon=")) currentEntry.icon = line.mid(5);
                else if (line.startsWith("Exec=")) currentEntry.exec = line.mid(5);
                else if (line.startsWith("Path=")) currentEntry.workDir = line.mid(5);
                else if (line.startsWith("Terminal=true")) currentEntry.useTerminal = true;
                else if (line.startsWith("Hidden=true")) {
                    return DesktopEntry();
                }
            }

            currentEntry.name = iniNameLocalised.isEmpty() ? iniName : iniNameLocalised;
            currentEntry.isValid = !currentEntry.name.isEmpty();
            currentEntry.id = fileInfo.fileName();
            currentEntry.path = fileInfo.filePath();

            if (currentEntry.isValid) {
                return currentEntry;
            }
        }

        return DesktopEntry();
    }

    // Opens a file or URL using the system's default application.
    // We deliberately avoid using QDesktopServices directly: in that case, Qt—even outside of
    //  a sandbox—uses xdg-desktop-portal, which ignores the user's configured default for local files
    // (instead presenting a selection dialog, followed by remembering the chosen application).
    // In contrast, xdg-open queries the desktop environment (e.g., using kde-open on Plasma).
    // Returns true if the launch process was initiated.
    // This does not guarantee that xdg-open was actually able to open the file.
    bool openWithSystemDefault(const QUrl &url) {
        if (!url.isValid()) return false;

        const QString xdgOpen = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
        if (!xdgOpen.isEmpty()) {
            const QString target = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
            if (QProcess::startDetached(xdgOpen, { target })) return true;
        }
        return QDesktopServices::openUrl(url);
    }

    void openFileListWithHandler(const QString &handlerApp, const QStringList &fileList) {
        if (handlerApp.endsWith(".desktop", Qt::CaseInsensitive)) {
            launchDesktopFile(getDesktopEntryById(handlerApp), fileList);
            return;
        }

        if (!handlerApp.contains('/')) {
            QString localAppPath = QDir(QCoreApplication::applicationDirPath()).filePath(handlerApp);
            if (QFile::exists(localAppPath)) {
                QProcess::startDetached(localAppPath, fileList);
                return;
            }

            QString absolutePath = QStandardPaths::findExecutable(handlerApp);
            if (!absolutePath.isEmpty()) {
                // bool QProcess::startDetached(const QString &program, const QStringList &arguments = {}, const QString &workingDirectory = QString(), qint64 *pid = nullptr)
                QProcess::startDetached(absolutePath, fileList);
                return;
            }
        } else if (QFile::exists(handlerApp)) {
            QProcess::startDetached(handlerApp, fileList);
            return;
        }
    }

    // Wandelt die Exec-Zeile in eine Liste von Aufrufen (Programm + Argumente) um.
    // Mehrere Aufrufe gibt es nur bei %f/%u mit mehreren Dateien (laut Spezifikation einmal pro Datei).
    QList<QStringList> expandDesktopExec(const DesktopEntry &entry, const QStringList &files) {
        const QList<ExecToken> tokens = splitDesktopExec(unescapeDesktopString(entry.exec));
        if (tokens.isEmpty()) return {};

        bool hasListCode = false;   // %F / %U
        bool hasSingleCode = false; // %f / %u
        for (qsizetype t = 1; t < tokens.size(); ++t) {
            const ExecToken &tok = tokens.at(t);
            if (!tok.quoted && (tok.text == "%F" || tok.text == "%U")) {
                hasListCode = true;
            } else if (containsFieldCode(tok.text, QStringLiteral("fu"))) {
                hasSingleCode = true;
            }
        }

        auto buildInvocation = [&](const QString *file) {
            QStringList args;
            args << tokens.first().text; // Programm: keine Feldcodes
            for (qsizetype t = 1; t < tokens.size(); ++t) {
                const ExecToken &tok = tokens.at(t);
                if (!tok.quoted && (tok.text == "%F" || tok.text == "%U")) {
                    args << files;
                    continue;
                }
                if (!tok.quoted && tok.text == "%i") {
                    if (!entry.icon.isEmpty()) args << QStringLiteral("--icon") << entry.icon;
                    continue;
                }
                QString expanded;
                if (!expandFieldCodes(tok.text, file, entry, expanded)) continue;
                if (expanded.isEmpty() && !tok.text.isEmpty() && !tok.quoted) continue; // z.B. nur "%d"
                args << expanded;
            }
            return args;
        };

        QList<QStringList> invocations;
        if (hasListCode) {
            invocations << buildInvocation(files.isEmpty() ? nullptr : &files.first());
        } else if (hasSingleCode) {
            if (files.isEmpty()) {
                invocations << buildInvocation(nullptr);
            } else {
                for (const QString &file : files) invocations << buildInvocation(&file);
            }
        } else {
            // Kein Datei-Feldcode: Dateien anhängen (bisheriges Verhalten, ähnlich wie KDE)
            QStringList args = buildInvocation(nullptr);
            args << files;
            invocations << args;
        }
        return invocations;
    }

    void launchDesktopFile(const DesktopEntry &info, const QStringList &fileList) {
        if (!info.isValid) return;

        // ==========================================
        // 1. Handhabung von Type=Link oder verknüpften URLs
        // ==========================================
        if (info.type.compare(QLatin1String("Link"), Qt::CaseInsensitive) == 0 || !info.url.isEmpty()) {
            QString rawUrl = info.url.trimmed();
            if (rawUrl.isEmpty()) return;

            // 1. Direkt testen, ob es bereits eine valide Netzwerk-URL mit Schema ist (http, https, ftp, etc.)
            QUrl url(rawUrl);
            if (!url.scheme().isEmpty() && url.isValid()) {
                openWithSystemDefault(url);
                return;
            }

            // 2. Erst wenn kein Schema vorliegt, Pfade auflösen (~/, $HOME, relative Pfade)
            QString urlStr = Helpers::expandPath(rawUrl);

            QFileInfo fileInfo(urlStr);
            if (fileInfo.isRelative()) {
                QDir desktopFileDir = QFileInfo(info.path).absoluteDir();
                urlStr = desktopFileDir.absoluteFilePath(urlStr);
            }

            // Lokale Pfade explizit als File-URL konvertieren
            url = QUrl::fromLocalFile(urlStr);
            if (!url.isValid()) {
                url = QUrl::fromUserInput(urlStr);
            }

            if (url.isValid()) {
                openWithSystemDefault(url);
            } else {
                qWarning() << "launchDesktopFile(): Ungültige URL:" << urlStr;
            }
            return;
        }

        // ==========================================
        // 2. Handhabung von Type=Application (Normaler Exec-Start)
        // ==========================================
        if (info.exec.isEmpty()) return;

        for (QStringList args : expandDesktopExec(info, fileList)) {
            if (args.isEmpty()) continue;
            const QStringList invocation = args;
            QString program = args.takeFirst();

            program = Helpers::expandPath(program);
            if (!QFileInfo(program).isAbsolute()) {
                program = QStandardPaths::findExecutable(program);
                if (program.isEmpty()) {
                    qWarning() << "launchDesktopFile(): Programm nicht gefunden:" << invocation.first();
                    continue;
                }
            }

            // ==========================================
            // Terminal-Handling
            // ==========================================
            if (info.useTerminal) {
                QString terminal;
                // Priorisierte Liste bekannter Linux-Terminals (Konsole zuerst wegen KDE)
                const QStringList terminalCandidates = {
                    "konsole", "gnome-terminal", "xfce4-terminal", "alacritty", "kitty", "xterm"
                };

                for (const QString &candidate : terminalCandidates) {
                    terminal = QStandardPaths::findExecutable(candidate);
                    if (!terminal.isEmpty()) {
                        break;
                    }
                }

                if (!terminal.isEmpty()) {
                    QStringList terminalArgs;

                    // Ältere/Schlanke Terminals wie xterm/alacritty brauchen meist '-e'
                    // Moderne Desktoptop-Terminals (konsole, gnome-terminal) bevorzugen '--'
                    if (terminal.endsWith("gnome-terminal")) {
                        terminalArgs << "--" << program << args;
                    } else {
                        terminalArgs << "-e" << program << args;
                    }

                    // Das Terminal wird zum Hauptprogramm, die App wandert in die Argumente
                    program = terminal;
                    args = terminalArgs;
                } else {
                    // Fallback: Wenn absolut kein Terminal gefunden wurde,
                    // loggen wir eine Warnung und starten es normal im Hintergrund.
                    qWarning("Konnte keinen Terminal-Emulator finden. Starte App normal.");
                }
            }
            // ==========================================

            QString workDir = Helpers::expandPath(info.workDir.trimmed());
            if (workDir.startsWith('"') && workDir.endsWith('"'))
                workDir = workDir.mid(1, workDir.length() - 2);
            if (workDir.isEmpty() || !QDir(workDir).exists())
                workDir = QFileInfo(program).absolutePath();
            qDebug() << "launchDesktopFile() program:" << program << "args:" << args;
            QProcess::startDetached(program, args, workDir);
        }
    }

    QString expandPath(QString path) {
        if (path.size() > kMaxInputLength) {
            path.truncate(kMaxInputLength);
        }
        path = stripInvisibleAndControlChars(path);
        return expandSingleLine(path);
    }

} // namespace Helpers

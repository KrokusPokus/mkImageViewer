#include "ImageLoadThread.h"
#include "Helpers.h"
#include "BaseImage.h"
#include "MaxImageDef.h"
#include "MainDlg.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "SettingsProvider.h"
#include "wrapperAVIF.h"
#include "wrapperHEIF.h"
#include "wrapperJXL.h"
#include "wrapperPNG.h"
#include "wrapperQOI.h"
#include "wrapperTJPEG.h"
#include "wrapperWEBP.h"
#include "zip/zip.h"
#include "libjpeg-turbo/include/turbojpeg.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QString>

#include "libbit7z/include/bitarchivereader.hpp"
using namespace bit7z;

// static initializers
std::atomic<int> CImageLoadThread::m_curHandle(0);

// The DIBs handed to CBaseImage are BGRA. QImage::Format_ARGB32 / Format_RGB32 have exactly this layout on little endian.
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "The 32bpp DIB layout (B,G,R,A) assumes a little endian platform");

/////////////////////////////////////////////////////////////////////////////////////////////
// static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

namespace {

using namespace std::string_view_literals;

// Upper bound for a single archive entry we are willing to inflate into memory.
// The size stored in an archive is attacker controlled, so it must be checked before allocating.
// The wrappers take 'int' sizes, hence the INT_MAX cap.
const uint64_t kMaxZipEntrySize = std::min<uint64_t>(
    std::max({ static_cast<uint64_t>(MAX_JPEG_FILE_SIZE), static_cast<uint64_t>(MAX_WEBP_FILE_SIZE),
              static_cast<uint64_t>(MAX_PNG_FILE_SIZE), static_cast<uint64_t>(MAX_JXL_FILE_SIZE),
              static_cast<uint64_t>(MAX_HEIF_FILE_SIZE), static_cast<uint64_t>(MAX_BMP_FILE_SIZE) }),
    static_cast<uint64_t>(INT_MAX));

// Flags for the decoder caches that must survive when switching to another format
enum CacheKeep : unsigned {
    Keep_None = 0,
    Keep_Qt   = 1u << 0,
    Keep_Webp = 1u << 1,
    Keep_Png  = 1u << 2,
    Keep_Jxl  = 1u << 3,
    Keep_Avif = 1u << 4,
    Keep_Zip  = 1u << 5
};

// The decoder cache that must survive while a file of this format is read (an animation is continued with it)
unsigned CacheKeepFor(EImageFormat eFormat) {
    switch (eFormat) {
    case IF_WEBP: return Keep_Webp;
    case IF_PNG:  return Keep_Png;
    case IF_JXL:  return Keep_Jxl;
    case IF_AVIF: return Keep_Avif;
    default:      return Keep_None;
    }
}

// Runs a callable when the scope is left (normally or by exception)
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : m_f(std::move(f)) {}
    ~ScopeExit() { m_f(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    F m_f;
};

// Suppresses the "missing DLL" system dialogs while a delay-loaded codec DLL is touched.
// Restores the previous error mode on every exit path. No-op on non-Windows platforms.
class CErrorModeGuard {
public:
    CErrorModeGuard() {}
    ~CErrorModeGuard() {}
    CErrorModeGuard(const CErrorModeGuard&) = delete;
    CErrorModeGuard& operator=(const CErrorModeGuard&) = delete;
};

// Reads a whole file into a freshly allocated buffer. Returns nullptr on failure, eError tells why.
std::unique_ptr<char[]> ReadWholeFile(const QString& sFileName, qint64 nMaxSize, qint64& nFileSize, ELoadError& eError) {
    nFileSize = 0;
    eError = ELoadError::None;

    QFile file(sFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        eError = ELoadError::FileNotReadable;
        return nullptr;
    }
    const qint64 nSize = file.size();
    if (nSize <= 0) {
        eError = ELoadError::NotDecodable; // empty file
        return nullptr;
    }
    if (nSize > std::min<qint64>(nMaxSize, INT_MAX)) {
        eError = ELoadError::TooLarge;
        return nullptr;
    }
    std::unique_ptr<char[]> pBuffer(new (std::nothrow) char[static_cast<size_t>(nSize)]);
    if (!pBuffer) {
        eError = ELoadError::OutOfMemory;
        return nullptr;
    }
    if (file.read(pBuffer.get(), nSize) != nSize) {
        eError = ELoadError::FileNotReadable;
        return nullptr;
    }
    nFileSize = nSize;
    return pBuffer;
}

EImageFormat GetImageFormatFromName(const QString& sFileName) {
    static const QHash<QString, EImageFormat> kFormatBySuffix = {
        { QStringLiteral("jpg"),  IF_JPEG },
        { QStringLiteral("jpeg"), IF_JPEG },
        { QStringLiteral("jfif"), IF_JPEG },
        { QStringLiteral("bmp"),  IF_WindowsBMP },
        { QStringLiteral("png"),  IF_PNG },
        { QStringLiteral("tif"),  IF_TIFF },
        { QStringLiteral("tiff"), IF_TIFF },
        { QStringLiteral("webp"), IF_WEBP },
        { QStringLiteral("jxl"),  IF_JXL },
        { QStringLiteral("avif"), IF_AVIF },
        { QStringLiteral("heif"), IF_HEIF },
        { QStringLiteral("heic"), IF_HEIF },
        { QStringLiteral("hif"),  IF_HEIF },
        { QStringLiteral("tga"),  IF_TGA },
        { QStringLiteral("zip"),  IF_ZIP },
        { QStringLiteral("cbz"),  IF_ZIP },
        { QStringLiteral("7z"),   IF_ZIP },
        { QStringLiteral("cb7"),  IF_ZIP },
        { QStringLiteral("qoi"),  IF_QOI }
    };
    return kFormatBySuffix.value(QFileInfo(sFileName).suffix().toLower(), IF_Unknown);
}

// The header is always exactly 16 bytes (zero padded); the fixed extent makes the bounds explicit.
using Header16 = std::span<const unsigned char, 16>;

bool HasSig(const Header16& h, size_t nOffset, std::string_view sig) {
    Q_ASSERT(nOffset + sig.size() <= h.size());
    return std::memcmp(h.data() + nOffset, sig.data(), sig.size()) == 0;
}

// Finds the image format from an in-memory 16-byte header. sFileName is only used as a fallback for the
// extension-based check and for the magic-byte checks that must run after it. It does not need to point
// to an actual file on disk - only its name/extension is used here.
EImageFormat GetImageFormatInMemory(Header16 h, const QString& sFileName) {
    if (HasSig(h, 0, "BM"sv)) {
        return IF_WindowsBMP;
    }
    if (HasSig(h, 0, "\xff\xd8"sv)) {
        return IF_JPEG;
    }
    if (HasSig(h, 0, "\x89PNG\x0d\x0a\x1a\x0a"sv)) {
        return IF_PNG;
    }
    if (HasSig(h, 0, "GIF8"sv) && (h[4] == '7' || h[4] == '9') && h[5] == 'a') {
        return IF_GIF;
    }
    if (HasSig(h, 0, "RIFF"sv) && HasSig(h, 8, "WEBP"sv)) {
        return IF_WEBP;
    }
    if ((h[0] == 0xff && h[1] == 0x0a) ||
        HasSig(h, 0, "\x00\x00\x00\x0c" "JXL " "\x0d\x0a\x87\x0a"sv)) {
        return IF_JXL;
    }

    if (HasSig(h, 4, "ftyp"sv)) {
        // https://github.com/strukturag/libheif/issues/83
        // https://github.com/strukturag/libheif/blob/ce1e4586b6222588c5afcd60c7ba9caa86bcc58c/libheif/heif.h#L602-L805

        // AV1: avif, avis
        if (HasSig(h, 8, "avi"sv)) {
            return IF_AVIF;
        }
        // H265: heic, heix, hevc, hevx, heim, heis, hevm, hevs
        if (HasSig(h, 8, "hei"sv) || HasSig(h, 8, "hev"sv)) {
            return IF_HEIF;
        }
    } else if (HasSig(h, 0, "qoif"sv)) {
        return IF_QOI;
    } else if (HasSig(h, 0, "PK\x03\x04"sv)) {
        return IF_ZIP;
    }

    // default fallback if no matches based on magic bytes
    const EImageFormat eByName = GetImageFormatFromName(sFileName);
    if (eByName != IF_Unknown) {
        return eByName;
    }
    if (HasSig(h, 4, "ftyp"sv)) {
        // Unspecified encoding (possibly AVIF or HEIF): mif1, mif2, msf1, miaf, 1pic
        return IF_AVIF;
    }
    if (HasSig(h, 0, "II*\0"sv) || HasSig(h, 0, "MM\0*"sv)) {
        // Must be checked after file extension to avoid classifying RAW as TIFF
        // A few RAW image formats use TIFF as the container
        // ex: CR2 - http://lclevy.free.fr/cr2/#key_info
        // ex: DNG - https://www.adobe.com/creativecloud/file-types/image/raw/dng-file.html#dng
        return IF_TIFF;
    }
    if (h[0] == 0x00 && h[1] == 0x00 && h[2] == 0x01 && h[3] == 0x00) {
        return IF_ICO;
    }
    return IF_Unknown;
}

// opens the file, reads up to 16 header bytes, and determines the image format
EImageFormat GetImageFormat(const QString& sFileName) {
    QFile file(sFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return IF_Unknown;
    }

    unsigned char header[16] = { 0 };
    const qint64 nSize = file.read(reinterpret_cast<char*>(header), sizeof(header));
    file.close();

    if (nSize < 2) {
        return IF_Unknown;
    }

    return GetImageFormatInMemory(header, sFileName);
}

// Maximum accepted size of a (decompressed) archive entry for the given format
uint64_t MaxFileSizeForFormat(EImageFormat eFormat) {
    switch (eFormat) {
    case IF_WindowsBMP:
    case IF_TGA:  return MAX_BMP_FILE_SIZE;
    case IF_JPEG: return MAX_JPEG_FILE_SIZE;
    case IF_WEBP: return MAX_WEBP_FILE_SIZE;
    case IF_PNG:  return MAX_PNG_FILE_SIZE;
    case IF_QOI:  return MAX_PNG_FILE_SIZE;
    case IF_JXL:  return MAX_JXL_FILE_SIZE;
    case IF_AVIF:
    case IF_HEIF: return MAX_HEIF_FILE_SIZE;
    default:      return kMaxZipEntrySize;
    }
}

std::string ToNativeNarrowPath(const QString& sFileName) {
    return QFile::encodeName(sFileName).toStdString();
}

// Location of 7z.so differs between distributions / packages (7zip, p7zip)
const std::string& SevenZipLibraryPath() {
    static const std::string kPath = [] {
        for (const char* p : { "/usr/lib/7zip/7z.so", "/usr/lib/p7zip/7z.so",
                              "/usr/libexec/p7zip/7z.so", "/usr/lib64/p7zip/7z.so" }) {
            if (QFile::exists(QString::fromUtf8(p))) {
                return std::string(p);
            }
        }
        return std::string("7z.so"); // let dlopen() search
    }();
    return kPath;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Qt image decoding (QImageReader / QImage / QColorSpace)
/////////////////////////////////////////////////////////////////////////////////////////////

// Format and frame information that is handed over to the CBaseImage constructor
struct CImageFrameInfo {
    EImageFormat eFormat = IF_Unknown;
    bool bIsAnimation = false;
    int nFrameIndex = 0;
    int nFrameCount = 1;
    int nFrameTimeMs = 100;
    bool bInArchive = false;
};

// Maps the format name reported by QImageReader::format() to the image format used by CBaseImage.
EImageFormat MapQtImageFormat(const QByteArray& sFormat) {
    if (sFormat == "avif") {
        return IF_AVIF;
    }
    if (sFormat == "bmp") {
        return IF_WindowsBMP;
    }
    if (sFormat == "gif") {
        return IF_GIF;
    }
    if (sFormat == "heif" || sFormat == "heic" || sFormat == "hif") {
        return IF_HEIF;
    }
    if (sFormat == "ico") {
        return IF_ICO;
    }
    if (sFormat == "jpg" || sFormat == "jpeg" || sFormat == "jfif") {
        return IF_JPEG;
    }
    if (sFormat == "jxl") {
        return IF_JXL;
    }
    if (sFormat == "png") {
        return IF_PNG;
    }
    if (sFormat == "qoi") {
        return IF_QOI;
    }
    if (sFormat == "tga") {
        return IF_TGA;
    }
    if (sFormat == "tif" || sFormat == "tiff") {
        return IF_TIFF;
    }
    if (sFormat == "webp") {
        return IF_WEBP;
    }

    return IF_Unknown;
}

const QColorSpace& SrgbColorSpace() {
    static const QColorSpace kSrgb(QColorSpace::SRgb);
    return kSrgb;
}

// bApplyEXIFOrientation: let Qt rotate/mirror the image according to its EXIF orientation (if enabled in the settings).
// Pass false if the caller hands the EXIF data to the CBaseImage, which then does the rotation itself (otherwise it would be done twice).
std::unique_ptr<QImageReader> CreateQtImageReader(QIODevice* pDevice, bool bApplyEXIFOrientation) {
    auto pReader = std::make_unique<QImageReader>(pDevice);
    pReader->setDecideFormatFromContent(true); // do not trust the file extension
    pReader->setAutoTransform(bApplyEXIFOrientation && CSettingsProvider::This().AutoRotateEXIF());
    return pReader;
}

// Checks the image size reported by the decoder *before* the pixels are decoded.
// Returns false if the image must not be decoded. bOutOfMemory is set if it is rejected because of its pixel count.
bool IsImageSizeAcceptable(const QSize& size, bool& bOutOfMemory) {
    if (!size.isValid()) {
        return true; // not known before decoding, ConvertQImageToJPEGImage() checks again afterwards
    }
    if (size.width() > MAX_IMAGE_DIMENSION || size.height() > MAX_IMAGE_DIMENSION) {
        return false;
    }
    if (static_cast<double>(size.width()) * size.height() > MAX_IMAGE_PIXELS) {
        bOutOfMemory = true;
        return false;
    }
    return true;
}

// Converts a decoded QImage into a CBaseImage (32bpp BGRA DIB).
// - If enabled in the settings, the embedded ICC profile is applied (conversion to sRGB). Images without a profile,
//   or with a profile Qt cannot use, have an invalid colorSpace() and are left untouched.
// - Images with an alpha channel are blended onto the background according to nTransparencyMode.
// pEXIFData is copied by CBaseImage, the caller keeps ownership.
CBaseImage* ConvertQImageToJPEGImage(QImage image, void* pEXIFData, const CImageFrameInfo& frame,
                                     bool& isOutOfMemory, Helpers::ETransparencyMode nTransparencyMode) {
    isOutOfMemory = false;
    if (image.isNull()) {
        return nullptr;
    }
    if (!IsImageSizeAcceptable(image.size(), isOutOfMemory)) {
        return nullptr;
    }

    // Always work with 8 bit/channel: RGB32 for opaque images (the unused byte is 0xff), straight (not premultiplied) ARGB32 otherwise.
    // convertTo() keeps the color space of the image.
    const bool bHasAlphaChannel = image.hasAlphaChannel();
    image.convertTo(bHasAlphaChannel ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (image.isNull()) {
        isOutOfMemory = true; // the conversion could not allocate its target
        return nullptr;
    }

    if (CSettingsProvider::This().UseEmbeddedColorProfiles()) {
        const QColorSpace colorSpace = image.colorSpace();
        if (colorSpace.isValid() && colorSpace != SrgbColorSpace()) {
            image.convertToColorSpace(SrgbColorSpace());
        }
    }

    const int nWidth = image.width();
    const int nHeight = image.height();
    const size_t nRowBytes = static_cast<size_t>(nWidth) * 4;
    std::unique_ptr<uint8_t[]> pDIB(new (std::nothrow) uint8_t[nRowBytes * static_cast<size_t>(nHeight)]);
    if (!pDIB) {
        isOutOfMemory = true;
        return nullptr;
    }
    for (int y = 0; y < nHeight; ++y) {
        std::memcpy(pDIB.get() + static_cast<size_t>(y) * nRowBytes, image.constScanLine(y), nRowBytes);
    }
    image = QImage(); // release the decoded image before the CBaseImage is created

    if (bHasAlphaChannel) {
        Helpers::BlendAlpha(reinterpret_cast<uint32*>(pDIB.get()), nWidth, nHeight, nTransparencyMode);
    }
    CBaseImage* pJPEGImage = new CBaseImage(nWidth, nHeight, pDIB.get(), pEXIFData, 4, frame.eFormat,
                                            frame.bIsAnimation, frame.nFrameIndex, frame.nFrameCount, frame.nFrameTimeMs, frame.bInArchive);
    pDIB.release(); // owned by the CBaseImage now
    return pJPEGImage;
}

// Decodes the first frame of an image that is already held in memory.
// This is what QImage::fromData() does internally; a QImageReader is used directly because it reports the image
// size before the pixels are decoded, which keeps the MAX_IMAGE_DIMENSION / MAX_IMAGE_PIXELS limits effective.
// pData must stay valid for the duration of the call (it is not copied).
CBaseImage* DecodeBufferWithQt(const void* pData, int nSize, void* pEXIFData, const CImageFrameInfo& frame,
                               bool& isOutOfMemory, Helpers::ETransparencyMode nTransparencyMode) {
    isOutOfMemory = false;
    try {
        QByteArray data = QByteArray::fromRawData(static_cast<const char*>(pData), nSize);
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::ReadOnly)) {
            return nullptr;
        }
        const std::unique_ptr<QImageReader> pReader = CreateQtImageReader(&buffer, pEXIFData == nullptr); // destroyed before 'buffer'
        if (!IsImageSizeAcceptable(pReader->size(), isOutOfMemory)) {
            return nullptr;
        }
        QImage image = pReader->read();
        if (image.isNull()) {
            qDebug() << "DecodeBufferWithQt() failed:" << pReader->errorString();
            return nullptr;
        }
        return ConvertQImageToJPEGImage(std::move(image), pEXIFData, frame, isOutOfMemory, nTransparencyMode);
    } catch (const std::bad_alloc&) {
        isOutOfMemory = true;
        return nullptr;
    }
}

// Failed:       this single entry could not be read (corrupt data, CRC error, ...) -> skip it like a non-image
// ArchiveError: the archive itself could not be opened -> abort and keep the entry list intact
enum class EEntryRead { Ok, Empty, Failed, ArchiveError, TooLarge, OutOfMemory };

struct ZipCloser {
    void operator()(struct zip_t* pZip) const { zip_close(pZip); }
};
using ZipPtr = std::unique_ptr<struct zip_t, ZipCloser>;

// Reads one archive entry into pBuffer (pBuffer is released again on every failure).
// 'zip' is the (optionally already opened) handle for zip/cbz archives; it is closed again when done.
// p7zLibrary: nullptr for zip/cbz archives (read with the zip library), otherwise the loaded 7z.so
EEntryRead ReadZipEntry(const ZipEntry& z, Bit7zLibrary* p7zLibrary, const std::string& sArchivePath, ZipPtr& zip, std::unique_ptr<char[]>& pBuffer) {
    pBuffer.reset();
    if (z.size == 0) {
        return EEntryRead::Empty;
    }
    if (z.size > kMaxZipEntrySize) {
        return EEntryRead::TooLarge;
    }
    pBuffer.reset(new (std::nothrow) char[static_cast<size_t>(z.size)]);
    if (!pBuffer) {
        return EEntryRead::OutOfMemory;
    }

    if (p7zLibrary != nullptr) {
        try {
            // The library stays loaded, but the archive has to be reopened for every extraction, otherwise extract fails
            BitArchiveReader reader(*p7zLibrary, sArchivePath, BitFormat::Auto);
            try {
                reader.extractTo(reinterpret_cast<byte_t*>(pBuffer.get()), static_cast<size_t>(z.size), z.index);
                return EEntryRead::Ok;
            } catch (const BitException& ex) {
                qDebug() << "bit7z:" << ex.what();
                pBuffer.reset();
                return EEntryRead::Failed; // only this entry is broken
            }
        } catch (const BitException& ex) {
            qDebug() << "bit7z:" << ex.what();
            pBuffer.reset();
            return EEntryRead::ArchiveError; // library or archive could not be opened
        }
    }

    if (!zip) {
        zip.reset(zip_open(sArchivePath.c_str(), 0, 'r'));
    }
    if (!zip) {
        pBuffer.reset();
        return EEntryRead::ArchiveError;
    }
    EEntryRead result = EEntryRead::Failed;
    if (zip_entry_openbyindex(zip.get(), z.index) == 0) {
        const auto nRead = zip_entry_noallocread(zip.get(), pBuffer.get(), static_cast<size_t>(z.size));
        if (nRead >= 0 && static_cast<uint64_t>(nRead) == z.size) {
            result = EEntryRead::Ok;
        }
        zip_entry_close(zip.get());
    }
    zip.reset();
    if (result != EEntryRead::Ok) {
        pBuffer.reset();
    }
    return result;
}


/////////////////////////////////////////////////////////////////////////////////////////////
// Decoding from a buffer (one code path for single files and for entries of an archive)
/////////////////////////////////////////////////////////////////////////////////////////////

// default frame interval in ms for images in CB* archives: 1hr
constexpr int kMangaFrameTimeMs = 360000;

// Frees memory that the wrappers allocated with malloc() (the EXIF segments)
struct CFreeDeleter {
    void operator()(void* p) const noexcept { std::free(p); }
};
using ExifPtr = std::unique_ptr<void, CFreeDeleter>;

// Pixel data is always allocated with new[]: CBaseImage releases it with delete[]
using PixelPtr = std::unique_ptr<uint8_t[]>;

// The encoded data handed to a decoder
struct CEncodedData {
    char* pData = nullptr; // nullptr: the decoder continues an animation it has cached
    size_t nSize = 0;
};

// Result of a decoder run, the same for all formats
struct CDecoded {
    CDecoded() = default;
    CDecoded(const CDecoded&) = delete;
    CDecoded& operator=(const CDecoded&) = delete;
    ~CDecoded() { std::free(pExif); }

    PixelPtr pixels;
    void* pExif = nullptr;                   // EXIF segment, allocated by the wrapper with malloc(), released here
    int nWidth = 0;
    int nHeight = 0;
    int nChannels = 0;
    int nFrameIndex = 0;                     // frame that was decoded (formats with several frames)
    int nFrameCount = 1;
    int nFrameTimeMs = 0;
    bool bHasAnimation = false;
    bool bBlendAlpha = false;                // the pixels have an alpha channel that must be blended onto the background
    bool bOutOfMemory = false;
    TJSAMP eChromoSubSampling = TJSAMP_420;  // JPEG only
};

// Reads one image from 'in'. Returns false if that did not work; bOutOfMemory is set if the reason is the image size.
// nFrameIndex is only used by the formats that can return a specific frame (AVIF, HEIF).
using DecoderFn = bool (*)(const CEncodedData& in, int nFrameIndex, CDecoded& out);

bool DecodeJpeg(const CEncodedData& in, int, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(TurboJpeg::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.eChromoSubSampling,
                                                                 out.bOutOfMemory, in.pData, static_cast<int>(in.nSize))));
    if (out.nChannels != 3 && out.nChannels != 1) { // color and b/w JPEG is supported
        out.pixels.reset();
    }
    return out.pixels != nullptr;
}

bool DecodeBmp(const CEncodedData& in, int, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(CReaderBMP::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bOutOfMemory,
                                                                  in.pData, in.nSize)));
    return out.pixels != nullptr;
}

bool DecodeTga(const CEncodedData& in, int, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(CReaderTGA::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bOutOfMemory,
                                                                  in.pData, in.nSize)));
    if (out.pixels && out.nChannels == 4) {
        uint32_t* pImage32 = reinterpret_cast<uint32_t*>(out.pixels.get());
        if (CReaderTGA::IsAlphaChannelValid(out.nWidth, out.nHeight, pImage32)) {
            out.bBlendAlpha = true;
        } else {
            // no valid alpha channel - set all A to 255
            const size_t nPixels = static_cast<size_t>(out.nWidth) * static_cast<size_t>(out.nHeight);
            for (size_t i = 0; i < nPixels; ++i) {
                pImage32[i] |= 0xFF000000u;
            }
        }
    }
    return out.pixels != nullptr;
}

bool DecodeWebp(const CEncodedData& in, int nFrameIndex, CDecoded& out) {
    // Without data the reader continues the cached animation and leaves has_animation untouched
    out.bHasAnimation = (in.pData == nullptr);
    out.pixels.reset(static_cast<uint8_t*>(WebpReaderWriter::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bHasAnimation,
                                                                        out.nFrameCount, out.nFrameTimeMs, out.pExif, out.bOutOfMemory,
                                                                        in.pData, static_cast<int>(in.nSize))));
    if (out.nChannels != 4) {
        out.pixels.reset();
    }
    out.nFrameIndex = nFrameIndex;
    out.bBlendAlpha = true;
    return out.pixels != nullptr;
}

bool DecodePng(const CEncodedData& in, int nFrameIndex, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(PngReader::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bHasAnimation,
                                                                 out.nFrameCount, out.nFrameTimeMs, out.pExif, out.bOutOfMemory,
                                                                 in.pData, in.nSize)));
    out.nFrameIndex = nFrameIndex;
    out.bBlendAlpha = true;
    return out.pixels != nullptr;
}

bool DecodeJxl(const CEncodedData& in, int nFrameIndex, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(JxlReader::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bHasAnimation,
                                                                 out.nFrameCount, out.nFrameTimeMs, out.pExif, out.bOutOfMemory,
                                                                 in.pData, static_cast<int>(in.nSize))));
    out.nFrameIndex = nFrameIndex;
    out.bBlendAlpha = true;
    return out.pixels != nullptr;
}

bool DecodeAvif(const CEncodedData& in, int nFrameIndex, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(AvifReader::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bHasAnimation,
                                                                  nFrameIndex, out.nFrameCount, out.nFrameTimeMs, out.pExif,
                                                                  out.bOutOfMemory, in.pData, in.nSize)));
    out.nFrameIndex = nFrameIndex;
    out.bBlendAlpha = true;
    return out.pixels != nullptr;
}

bool DecodeHeif(const CEncodedData& in, int nFrameIndex, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(HeifReader::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.nFrameCount,
                                                                  out.pExif, out.bOutOfMemory, nFrameIndex,
                                                                  in.pData, static_cast<int>(in.nSize))));
    out.nFrameIndex = nFrameIndex;
    out.bBlendAlpha = true;
    return out.pixels != nullptr;
}

bool DecodeQoi(const CEncodedData& in, int, CDecoded& out) {
    out.pixels.reset(static_cast<uint8_t*>(QoiReaderWriter::ReadImage(out.nWidth, out.nHeight, out.nChannels, out.bOutOfMemory,
                                                                       in.pData, static_cast<int>(in.nSize))));
    out.bBlendAlpha = (out.nChannels == 4);
    return out.pixels != nullptr;
}

// nullptr: no dedicated wrapper (GIF, TIFF, ICO and the like are read by Qt)
DecoderFn DecoderFor(EImageFormat eFormat) {
    switch (eFormat) {
    case IF_JPEG:       return DecodeJpeg;
    case IF_WindowsBMP: return DecodeBmp;
    case IF_TGA:        return DecodeTga;
    case IF_WEBP:       return DecodeWebp;
    case IF_PNG:        return DecodePng;
    case IF_JXL:        return DecodeJxl;
    case IF_AVIF:       return DecodeAvif;
    case IF_HEIF:       return DecodeHeif;
    case IF_QOI:        return DecodeQoi;
    default:            return nullptr;
    }
}

// Frame information for an image that Qt decodes from a buffer
CImageFrameInfo MakeFrameInfo(EImageFormat eFormat, const CDecodeTarget& target) {
    CImageFrameInfo info;
    info.eFormat = eFormat;
    if (target.bInArchive) {
        info.bInArchive = true;
        info.bIsAnimation = target.bMultipleFiles;
        info.nFrameIndex = target.nEntryIndex;
        info.nFrameCount = target.nArchiveCount;
        info.nFrameTimeMs = kMangaFrameTimeMs;
    }
    return info;
}

// Builds the CBaseImage from decoded pixels. The ownership of the pixels goes to the image.
std::unique_ptr<CBaseImage> CreateImage(CDecoded& d, EImageFormat eFormat, const CEncodedData& in,
                                        const CDecodeTarget& target, Helpers::ETransparencyMode eTransparency) {
    if (d.bBlendAlpha) {
        Helpers::BlendAlpha(reinterpret_cast<uint32*>(d.pixels.get()), d.nWidth, d.nHeight, eTransparency);
    }

    // JPEG: the EXIF block is part of the file data (CBaseImage copies it)
    void* pEXIF = d.pExif;
    if (eFormat == IF_JPEG) {
        pEXIF = Helpers::FindEXIFBlock(in.pData, static_cast<int>(in.nSize));
    }

    std::unique_ptr<CBaseImage> pImage;
    if (target.bInArchive) {
        // For an entry of an archive "the frame" is the entry
        pImage.reset(new CBaseImage(d.nWidth, d.nHeight, d.pixels.get(), pEXIF, d.nChannels, eFormat,
                                    target.bMultipleFiles, target.nEntryIndex, target.nArchiveCount, kMangaFrameTimeMs, target.bInArchive));
    } else {
        pImage.reset(new CBaseImage(d.nWidth, d.nHeight, d.pixels.get(), pEXIF, d.nChannels, eFormat,
                                    d.bHasAnimation, d.nFrameIndex, d.nFrameCount, d.nFrameTimeMs, target.bInArchive));
    }
    d.pixels.release(); // owned by the CBaseImage now

    if (eFormat == IF_JPEG) {
        pImage->SetJPEGComment(Helpers::GetJPEGComment(in.pData, static_cast<int>(in.nSize)));
    }
    return pImage;
}

} // namespace


/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CImageLoadThread::CImageLoadThread(void) : CWorkThread(true) {
}

CImageLoadThread::~CImageLoadThread(void) {
    Terminate(); // *must* happen first
    DeleteCachedQtImageReader();
    DeleteCachedWebpDecoder();
    DeleteCachedPngDecoder();
    DeleteCachedJxlDecoder();
    DeleteCachedAvifDecoder();
    DeleteCachedZip();
}

int CImageLoadThread::AsyncLoad(const QString& strFileName, int nFrameIndex, const CProcessParams & processParams, bool bReadAhead,
                                std::function<void(int)> onCompleted) {
    auto pRequest = std::make_shared<CRequest>(strFileName, nFrameIndex, processParams, bReadAhead, onCompleted);
    const int nHandle = pRequest->RequestHandle; // Read the handle before handing the request over: the worker thread may process it right away
    ProcessAsync(std::move(pRequest));
    return nHandle;
}

CImageData CImageLoadThread::GetLoadedImage(int nHandle) {
    return WithRequestList([&](const auto& list) -> CImageData {
        for (const auto& r : list) {
            if (r->Type == CReleaseFileRequest::ReleaseFileRequest) continue;
            auto& rq = static_cast<CRequest&>(*r);
            if (rq.Processed && !rq.Deleted && rq.RequestHandle == nHandle) {
                rq.Deleted = true;
                CImageData d;
                d.Image = rq.Image;
                d.Error = rq.Error;
                d.Position = rq.Position;
                return d;
            }
        }
        CImageData d; // cancelled before it was processed
        d.Error = ELoadError::Exception;
        return d;
    });
}

void CImageLoadThread::ReleaseFile(const QString& strFileName) {
    try { ProcessAndWait(std::make_shared<CReleaseFileRequest>(strFileName)); } catch (...) {}
}

std::vector<int> CImageLoadThread::CancelPendingLoads(const std::vector<int>& handles) {
    const auto cancelled = CancelUnstartedRequests([&handles](const CRequestBase& r) {
        if (r.Type == CReleaseFileRequest::ReleaseFileRequest) return false;  // never drop a release
        const int nHandle = static_cast<const CRequest&>(r).RequestHandle;
        return std::find(handles.begin(), handles.end(), nHandle) != handles.end();
    });
    std::vector<int> result;
    result.reserve(cancelled.size());
    for (const auto& r : cancelled) {
        result.push_back(static_cast<const CRequest&>(*r).RequestHandle);
    }
    return result;
}

int CImageLoadThread::AbortRunningLoad(const std::vector<int>& handles) {
    const auto aborted = RequestAbortRunning([&handles](const CRequestBase& r) {
        if (r.Type == CReleaseFileRequest::ReleaseFileRequest) return false;
        const int nHandle = static_cast<const CRequest&>(r).RequestHandle;
        return std::find(handles.begin(), handles.end(), nHandle) != handles.end();
    });
    return aborted ? static_cast<const CRequest&>(*aborted).RequestHandle : -1;
}

void CImageLoadThread::OnCancelled(CRequestBase& r) {
    if (r.Type != CReleaseFileRequest::ReleaseFileRequest) {
        auto& rq = static_cast<CRequest&>(r);
        if (rq.CompletedCallback) rq.CompletedCallback(rq.RequestHandle);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Protected
/////////////////////////////////////////////////////////////////////////////////////////////

// Called on the processing thread
void CImageLoadThread::ProcessRequest(CRequestBase& request) {
    if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
        CReleaseFileRequest& rq = static_cast<CReleaseFileRequest&>(request);
        if (rq.FileName == m_sLastFileName) {
            DeleteCachedQtImageReader();
        }
        if (rq.FileName == m_sLastWebpFileName) {
            DeleteCachedWebpDecoder();
        }
        if (rq.FileName == m_sLastPngFileName) {
            DeleteCachedPngDecoder();
        }
        if (rq.FileName == m_sLastJxlFileName) {
            DeleteCachedJxlDecoder();
        }
        if (rq.FileName == m_sLastAvifFileName) {
            DeleteCachedAvifDecoder();
        }
        if (rq.FileName == m_sLastZipFileName) {
            DeleteCachedZip(); // the list of entries of the archive
        }
        return;
    }

    // Drops all cached decoders except the ones flagged in 'keep'
    auto dropCaches = [this](unsigned keep) {
        if (!(keep & Keep_Qt))   DeleteCachedQtImageReader();
        if (!(keep & Keep_Webp)) DeleteCachedWebpDecoder();
        if (!(keep & Keep_Png))  DeleteCachedPngDecoder();
        if (!(keep & Keep_Jxl))  DeleteCachedJxlDecoder();
        if (!(keep & Keep_Avif)) DeleteCachedAvifDecoder();
        if (!(keep & Keep_Zip))  DeleteCachedZip();
    };

    CRequest& rq = static_cast<CRequest&>(request);

    // Pre-caching stops while memory is short; the image is loaded anyway when the user goes to it
    if (rq.ReadAhead && !Helpers::StillEnoughFree()) {
        rq.Error = ELoadError::ReadAheadSkipped;
        return;
    }

    const uint64_t startTimeMs = Helpers::GetTimeMs();

    // Fills the position, also if the image could not be read. Outside of an archive a failed load has frame 0 of 1
    // (the count is unknown); ProcessReadZipRequest() sets the position of an archive page itself.
    ScopeExit fillPosition([&rq] {
        if (rq.Image != nullptr) {
            rq.Error = ELoadError::None;
            rq.Position.FrameIndex = rq.Image->FrameIndex();
            rq.Position.FrameCount = rq.Image->NumberOfFrames();
            rq.Position.IsAnimation = rq.Image->IsAnimation();
            rq.Position.InArchive = rq.Image->InArchive();
        } else {
            if (rq.Error == ELoadError::None) {
                rq.Error = ELoadError::NotDecodable;
            }
            if (!rq.Position.InArchive) {
                rq.Position = CImagePosition{};
            }
        }
    });

    if (!QFileInfo::exists(rq.FileName)) {
        rq.Error = ELoadError::FileNotFound;
        return;
    }

    EImageFormat eFormat = GetImageFormat(rq.FileName);
    if (eFormat != IF_ZIP && CSettingsProvider::This().ForceQImageReader()) {
        eFormat = IF_Unknown; // der default-Zweig des switch ist der Qt-Pfad
    }

    // Get image format and read the image
    switch (eFormat) {
    case IF_JPEG:
    case IF_WindowsBMP:
    case IF_TGA:
    case IF_WEBP:
    case IF_PNG:
    case IF_JXL:
    case IF_AVIF:
    case IF_HEIF:
    case IF_QOI:
        // only the cached decoder of the format that is read survives: an animation is continued with it
        dropCaches(CacheKeepFor(eFormat));
        ProcessReadFileRequest(&rq, eFormat);
        break;
    case IF_ZIP:
        // ProcessReadZipRequest decides internally which cached decoders to drop.
        // This is necessary since only *it* knows which type of file inside the ZIP it actually is going to display next.
        ProcessReadZipRequest(&rq);
        break;
    default:
        // GIF, TIFF, ICO and everything else: try with Qt (keeps the cached QImageReader for animated GIFs)
        dropCaches(Keep_Qt | Keep_Zip);
        ProcessReadQtImageRequest(&rq);
        break;
    }

    // The owner does not need the image any more (read ahead out of range): skip rotation and resampling,
    // which take the most time after decoding, and free the memory right away
    if (rq.Image != nullptr && rq.AbortRequested) {
        delete rq.Image;
        rq.Image = nullptr;
        rq.Error = ELoadError::Aborted;
        return;
    }

    // then process the image if read was successful
    if (rq.Image != nullptr) {
        rq.Image->SetFileLoadDelayMs(Helpers::GetTimeMs() - startTimeMs);
        if (!ProcessImageAfterLoad(&rq)) {
            delete rq.Image;
            rq.Image = nullptr;
            rq.Error = ELoadError::OutOfMemory;
        }
    }
}

// Called on the processing thread
void CImageLoadThread::AfterFinishProcess(CRequestBase& request) {
    if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
        return;
    }

    CRequest& rq = static_cast<CRequest&>(request);
    // Invokes the completion callback that was passed in by CImageProvider
    if (rq.CompletedCallback) {
        rq.CompletedCallback(rq.RequestHandle);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

static void LimitOffsets(CPoint& offsets, QSize clippingSize, const QSize & imageSize) {
    const int nMaxOffsetX = std::max(0, (imageSize.width() - clippingSize.width()) / 2);
    const int nMaxOffsetY = std::max(0, (imageSize.height() - clippingSize.height()) / 2);
    offsets.x = std::clamp(offsets.x, -nMaxOffsetX, +nMaxOffsetX);
    offsets.y = std::clamp(offsets.y, -nMaxOffsetY, +nMaxOffsetY);
}

void CImageLoadThread::DeleteCachedQtImageReader() {
    // Order matters: the reader uses the file as its device
    m_pLastImageReader.reset();
    m_pLastImageFile.reset();
    m_sLastFileName.clear();
    m_eLastQtFormat = IF_Unknown;
    m_bLastQtAnimation = false;
    m_nLastQtFrameIndex = -1;
    m_nLastQtFrameCount = 1;
}

void CImageLoadThread::DeleteCachedWebpDecoder() {
    WebpReaderWriter::DeleteCache();
    m_sLastWebpFileName.clear();
}

void CImageLoadThread::DeleteCachedPngDecoder() {
    PngReader::DeleteCache();
    m_sLastPngFileName.clear();
}

void CImageLoadThread::DeleteCachedJxlDecoder() {
    // Order matters: the JXL decoder reads from m_pJxlBuffer until its cache is deleted.
    JxlReader::DeleteCache();
    m_pJxlBuffer.reset();
    m_sLastJxlFileName.clear();
}

void CImageLoadThread::DeleteCachedAvifDecoder() {
    {
        // prevent crashing when libavif/dav1d fail or missing
        CErrorModeGuard errorModeGuard;
        try {
            AvifReader::DeleteCache();
        } catch (...) {}
    }
    m_sLastAvifFileName.clear();
}

QString* CImageLoadThread::CachedFileNameFor(EImageFormat eFormat) {
    switch (eFormat) {
    case IF_WEBP: return &m_sLastWebpFileName;
    case IF_PNG:  return &m_sLastPngFileName;
    case IF_JXL:  return &m_sLastJxlFileName;
    case IF_AVIF: return &m_sLastAvifFileName;
    default:      return nullptr;
    }
}

void CImageLoadThread::DeleteCachedDecoder(EImageFormat eFormat) {
    switch (eFormat) {
    case IF_WEBP: DeleteCachedWebpDecoder(); break;
    case IF_PNG:  DeleteCachedPngDecoder();  break;
    case IF_JXL:  DeleteCachedJxlDecoder();  break;
    case IF_AVIF: DeleteCachedAvifDecoder(); break;
    default:      break;
    }
}

// Reads a file that has a dedicated decoder (everything except GIF, TIFF, ICO and the like, which Qt reads from the file).
// WebP, PNG, JXL and AVIF can be animated: the decoder stays cached (m_sLast*FileName) and delivers the next frame
// when the same file is requested again. In that case the file is not read again and DecodeFromBuffer() gets no data.
void CImageLoadThread::ProcessReadFileRequest(CRequest* request, EImageFormat eFormat) {
    const QString* pCachedName = CachedFileNameFor(eFormat);
    const bool bUseCachedDecoder = (pCachedName != nullptr && *pCachedName == request->FileName);
    if (!bUseCachedDecoder) {
        DeleteCachedDecoder(eFormat);
    }

    std::unique_ptr<char[]> pBuffer;
    qint64 nFileSize = 0;
    if (!bUseCachedDecoder) {
        pBuffer = ReadWholeFile(request->FileName, static_cast<qint64>(MaxFileSizeForFormat(eFormat)), nFileSize, request->Error);
        if (!pBuffer) {
            return;
        }
    }
    DecodeFromBuffer(request, eFormat, pBuffer, static_cast<size_t>(nFileSize), CDecodeTarget{});
}

// Decodes the encoded image in pBuffer and stores the result in request->Image. This is the only place that knows
// how to turn the data of a format into a CBaseImage, for single files and for entries of an archive alike.
// On failure request->Error tells why.
//
// Buffer ownership (pBuffer is empty if a cached animation is continued):
//  - WebpReaderWriter, PngReader and AvifReader copy what they need for an animation into their own cache,
//    HeifReader and the other readers only read during the call. The buffer can be released after the call.
//  - JxlReader does NOT copy its input: the decoder keeps reading from the buffer for as long as an animation is
//    cached. In that case the buffer is moved into m_pJxlBuffer and released in DeleteCachedJxlDecoder(),
//    strictly after JxlReader::DeleteCache().
//
// Differences between a file and an archive entry (CDecodeTarget::bInArchive):
//  - Archive: only the first frame is shown, no decoder is kept (the caller drops them before the buffer is released),
//    AVIF/HEIF always read frame 0, and the CBaseImage is created as an entry of the container IF_ZIP.
EDecodeStatus CImageLoadThread::DecodeFromBuffer(CRequest* request, EImageFormat eFormat, std::unique_ptr<char[]>& pBuffer,
                                                 size_t nSize, const CDecodeTarget& target) {
    CErrorModeGuard errorModeGuard;
    const Helpers::ETransparencyMode eTransparency = request->ProcessParams.TransparencyMode;
    const CEncodedData in{ pBuffer.get(), nSize };
    const bool bHaveData = (in.pData != nullptr);

    try {
        // Qt decodes the (first frame of the) buffer. pEXIFData is only used for the rotation done by the CBaseImage.
        auto decodeWithQt = [&](void* pEXIFData) {
            bool bQtOutOfMemory = false;
            request->Image = DecodeBufferWithQt(in.pData, static_cast<int>(in.nSize), pEXIFData, MakeFrameInfo(eFormat, target),
                                                bQtOutOfMemory, eTransparency);
            if (request->Image != nullptr) {
                return EDecodeStatus::Ok;
            }
            if (bQtOutOfMemory) {
                request->Error = ELoadError::OutOfMemory;
                return EDecodeStatus::OutOfMemory;
            }
            return EDecodeStatus::NotDecodable;
        };

        const DecoderFn decode = DecoderFor(eFormat);
        if (decode == nullptr) {
            // No dedicated wrapper (GIF, TIFF, ICO in an archive)
            const bool bQtFormat = (eFormat == IF_GIF || eFormat == IF_TIFF || eFormat == IF_ICO);
            return (bHaveData && bQtFormat) ? decodeWithQt(nullptr) : EDecodeStatus::NotDecodable;
        }

        CDecoded d;
        bool bDecoded = false;
        bool bException = false;
        try {
            bDecoded = decode(in, target.bInArchive ? 0 : request->FrameIndex, d);
        } catch (const heif::Error&) {
            // libheif could not read the data: the same as undecodable data, not an error of the program
        } catch (const std::bad_alloc&) {
            d.bOutOfMemory = true;
        } catch (...) {
            bException = true;
        }

        if (bDecoded) {
            if (!target.bInArchive && d.bHasAnimation) {
                // The decoder stays cached to deliver the next frame
                if (QString* pCachedName = CachedFileNameFor(eFormat)) {
                    *pCachedName = request->FileName;
                }
                if (eFormat == IF_JXL && pBuffer) {
                    // Only the first call has a buffer: with a cached decoder pBuffer is empty, and releasing
                    // the buffer the decoder is reading from would be a use after free.
                    m_pJxlBuffer = std::move(pBuffer);
                }
            }
            request->Image = CreateImage(d, eFormat, in, target, eTransparency).release();
            return EDecodeStatus::Ok;
        }

        // The state of a decoder is unknown after a failure
        DeleteCachedDecoder(eFormat);
        if (d.bOutOfMemory) {
            request->Error = ELoadError::OutOfMemory;
            return EDecodeStatus::OutOfMemory;
        }
        if (bException && eFormat != IF_AVIF) {
            request->Error = ELoadError::Exception;
            return EDecodeStatus::Exception;
        }

        // Fallbacks (only possible if the data is at hand)
        EDecodeStatus status = EDecodeStatus::NotDecodable;
        if (bHaveData) {
            switch (eFormat) {
            case IF_AVIF:
                // libavif failed or is not available (also by exception): libheif reads AVIF as well, the buffer is reused
                status = DecodeFromBuffer(request, IF_HEIF, pBuffer, nSize, target);
                break;
            case IF_JPEG:
            case IF_WindowsBMP:
            case IF_TGA:
                status = decodeWithQt(nullptr);
                break;
            case IF_PNG:
            {
                // libpng failed: Qt gets a chance. The EXIF data is taken from the PNG, the CBaseImage does the rotation.
                const ExifPtr pQtEXIF(PngReader::GetEXIFBlock(in.pData, in.nSize));
                status = decodeWithQt(pQtEXIF.get());
            }
            break;
            default:
                break; // WebP, JXL, QOI, HEIF: no fallback
            }
        }
        if (bException && status == EDecodeStatus::NotDecodable) {
            request->Error = ELoadError::Exception;
            return EDecodeStatus::Exception;
        }
        return status;
    } catch (const std::bad_alloc&) {
        request->Error = ELoadError::OutOfMemory;
        DeleteCachedDecoder(eFormat);
        return EDecodeStatus::OutOfMemory;
    } catch (...) {
        request->Error = ELoadError::Exception;
        DeleteCachedDecoder(eFormat);
        return EDecodeStatus::Exception;
    }
}

// Loads an image with Qt (QImageReader). Used for GIF, TIFF, ICO, and as a fallback if the dedicated JPEG/BMP decoders fail.
// Multi-frame images: the frame to show is request->FrameIndex.
//  - Animations (every format whose reader reports supportsAnimation() and more than one image: GIF, WebP, and e.g. AVIF / JXL
//          if a plugin provides them): the handlers can only decode forward (jumpToImage() does not work for GIF / WebP).
//          The QImageReader and its file are therefore cached while the animation is shown: the next frame is a plain read().
//          Any other frame rewinds the file and decodes forward (frames before the requested one are decoded and thrown away).
//          The cache is dropped by ReleaseFile() or when another file is loaded.
//  - TIFF: pages are random access (jumpToImage()), nothing is cached.
void CImageLoadThread::ProcessReadQtImageRequest(CRequest * request) {
    try {
        const bool bUseCachedReader = m_pLastImageReader && request->FileName == m_sLastFileName;
        if (!bUseCachedReader) {
            DeleteCachedQtImageReader();
            auto pFile = std::make_unique<QFile>(request->FileName);
            if (!pFile->open(QIODevice::ReadOnly)) {
                request->Error = ELoadError::FileNotReadable;
                return;
            }
            m_pLastImageFile = std::move(pFile);
            m_pLastImageReader = CreateQtImageReader(m_pLastImageFile.get(), true);
            m_sLastFileName = request->FileName;
            // Determine the format once: QImageReader::format() returns an empty name again as soon as the reader has
            // read the last frame of an animation, which made the second loop look like an unknown format.
            m_eLastQtFormat = MapQtImageFormat(m_pLastImageReader->format());
        }

        const EImageFormat eFormat = m_eLastQtFormat;
        if (!bUseCachedReader) {
            // imageCount() has to be queried BEFORE size(): with size() called first the TIFF handler reports a single
            // page for multi-page files (observed with Qt 6.12).
            // A format that can not tell its number of images (0) is shown as a still image.
            const bool bSupportsAnimation = m_pLastImageReader->supportsAnimation();
            if (bSupportsAnimation || eFormat == IF_TIFF) {
                m_nLastQtFrameCount = std::max(1, m_pLastImageReader->imageCount());
            }
            m_bLastQtAnimation = bSupportsAnimation && m_nLastQtFrameCount > 1;
            // reject too large images before any pixel is decoded
            bool bTooManyPixels = false;
            if (!IsImageSizeAcceptable(m_pLastImageReader->size(), bTooManyPixels)) {
                request->Error = ELoadError::TooLarge;
                DeleteCachedQtImageReader();
                return;
            }
        }

        const int nFrameCount = m_nLastQtFrameCount;
        const int nFrameIndex = std::clamp(request->FrameIndex, 0, nFrameCount - 1);
        const bool bIsAnimation = m_bLastQtAnimation;

        QImage image;
        if (eFormat == IF_TIFF && nFrameCount > 1) {
            if (m_pLastImageReader->jumpToImage(nFrameIndex)) {
                image = m_pLastImageReader->read();
            }
        } else if (bIsAnimation) {
            if (nFrameIndex <= m_nLastQtFrameIndex) {
                // rewind: a new reader on the file at position 0 starts again with the first frame
                m_pLastImageReader.reset();
                if (!m_pLastImageFile->seek(0)) {
                    DeleteCachedQtImageReader();
                    return;
                }
                m_pLastImageReader = CreateQtImageReader(m_pLastImageFile.get(), true);
                m_nLastQtFrameIndex = -1;
            }
            // decode (and discard) the frames in front of the requested one; for the next frame there is nothing to skip
            for (int i = m_nLastQtFrameIndex + 1; i < nFrameIndex; ++i) {
                if (m_pLastImageReader->read().isNull()) {
                    break;
                }
            }
            image = m_pLastImageReader->read();
            m_nLastQtFrameIndex = nFrameIndex;
        } else {
            image = m_pLastImageReader->read();
        }

        if (image.isNull()) {
            qDebug() << "ProcessReadQtImageRequest() failed:" << request->FileName << m_pLastImageReader->errorString();
            DeleteCachedQtImageReader();
            return;
        }

        int nFrameTimeMs = 100;
        if (nFrameCount > 1) {
            // delay of the frame that was just read
            const int nDelayMs = m_pLastImageReader->nextImageDelay();
            if (nDelayMs > 0) {
                nFrameTimeMs = nDelayMs;
            }
        }

        bool bOutOfMemory = false;
        request->Image = ConvertQImageToJPEGImage(std::move(image), nullptr,
                                                  CImageFrameInfo{ eFormat, bIsAnimation, nFrameIndex, nFrameCount, nFrameTimeMs },
                                                  bOutOfMemory, request->ProcessParams.TransparencyMode);
        if (request->Image == nullptr && bOutOfMemory) {
            request->Error = ELoadError::OutOfMemory;
        }
        if (!bIsAnimation) {
            DeleteCachedQtImageReader();
        }
    } catch (const std::bad_alloc&) {
        delete request->Image;
        request->Image = nullptr;
        request->Error = ELoadError::OutOfMemory;
        DeleteCachedQtImageReader();
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->Error = ELoadError::Exception;
        DeleteCachedQtImageReader();
    }
}

bool CImageLoadThread::ProcessImageAfterLoad(CRequest * request) {
    qDebug() << "CImageLoadThread::ProcessImageAfterLoad() request->FileName:" << request->FileName << "request->FrameIndex:" << request->FrameIndex;

    // initial parameters of the image from the request (rotation completed by the EXIF orientation)
    request->Image->SetInitialProcessParams(&(request->ProcessParams));

    // First do rotation, this maybe modifies the width and height
    if (!request->Image->VerifyRotation(CRotationParams(request->ProcessParams.RotationParams, request->ProcessParams.RotationParams.Rotation + request->ProcessParams.UserRotation))) {
        return false;
    }

    // Do nothing (except rotation) if processing after load turned off
    if (GetProcessingFlag(request->ProcessParams.ProcFlags, PFLAG_NoProcessingAfterLoad)) {
        return true;
    }

    const int nWidth = request->Image->OrigWidth();
    const int nHeight = request->Image->OrigHeight();

    double dZoom = request->ProcessParams.Zoom;

    qDebug() << "CImageLoadThread::ProcessImageAfterLoad() OrigWidth:" << nWidth << "OrigHeight:" << nHeight << "dZoom:" << dZoom << "TargetWidth:" << request->ProcessParams.TargetWidth << "TargetHeight:" << request->ProcessParams.TargetHeight;

    QSize newSize;
    if (dZoom < 0.0) {
        newSize = Helpers::GetImageRect(nWidth,
                                        nHeight,
                                        request->ProcessParams.TargetWidth,
                                        request->ProcessParams.TargetHeight,
                                        request->ProcessParams.AutoZoomMode,
                                        dZoom);
    } else {
        newSize = QSize(qRound(nWidth * dZoom), qRound(nHeight * dZoom));
    }

    newSize.setWidth(std::clamp(newSize.width(), 1, 65535));
    newSize.setHeight(std::clamp(newSize.height(), 1, 65535));

    // clip to target rectangle
    const QSize clippedSize(std::min<int>(request->ProcessParams.TargetWidth, newSize.width()),
                            std::min<int>(request->ProcessParams.TargetHeight, newSize.height()));

    LimitOffsets(request->ProcessParams.Offsets, QSize(request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight), newSize);

    // this will process the image and cache the processed DIB in the CBaseImage instance
    const CPoint offsetInImage = request->Image->ConvertOffset(newSize, clippedSize, request->ProcessParams.Offsets);
    return nullptr != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
                                             request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags);
}


// #################################################

namespace {

// CP437 (original IBM PC code page) to Unicode for the bytes 0x80 to 0xFF, 0x00 to 0x7F are ASCII
const char16_t kCP437High[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// Name of the current entry of a ZIP archive (zip_entry_openbyindex). Per ZIP specification the name is UTF-8 if
// bit 11 of the general purpose flags is set, otherwise CP437. Archivers like 7-Zip or Windows store names that are
// representable in CP437 that way and add an Info-ZIP Unicode Path extra field with the UTF-8 name, which takes
// precedence (it is only returned if its CRC matches the stored name).
QString ZipEntryName(struct zip_t* pZip) {
    const char* pchName = zip_entry_name(pZip);
    if (pchName == nullptr) {
        return QString();
    }
    if (zip_entry_isutf8(pZip) == 1) {
        return QString::fromUtf8(pchName);
    }
    size_t nUnicodeLen = 0;
    if (const char* pchUnicode = zip_entry_unicodepath(pZip, &nUnicodeLen)) {
        return QString::fromUtf8(pchUnicode, static_cast<qsizetype>(nUnicodeLen));
    }
    QString sName;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pchName); *p != 0; ++p) {
        sName += (*p < 0x80) ? QChar(*p) : QChar(kCP437High[*p - 0x80]);
    }
    return sName;
}

// Only entries with these extensions are listed (formats DecodeFromBuffer() can read from an archive). The list is
// made once when the archive is opened and never changed afterwards: the index of an entry is its page number.
bool IsArchiveImageEntry(const QString& sEntryName) {
    static const QSet<QString> kExtensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("bmp"), QStringLiteral("png"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("jxl"),
        QStringLiteral("avif"), QStringLiteral("heif"), QStringLiteral("heic"), QStringLiteral("hif"), QStringLiteral("tga"),
        QStringLiteral("qoi"), QStringLiteral("ico")
    };
    return kExtensions.contains(QFileInfo(sEntryName).suffix().toLower());
}

} // namespace

ZipEntry::ZipEntry(const QString& a_name, unsigned int a_index, unsigned long long a_size) :
    name(a_name),
    index(a_index),
    size(a_size)
{
}

ZipEntry::ZipEntry(const bit7z::tstring& a_name, unsigned int a_index, unsigned long long a_size) :
#if defined(BIT7Z_STRING_IS_U16) || defined(_UNICODE)
    name(QString::fromUtf16(reinterpret_cast<const char16_t*>(a_name.c_str()))),
#else
    name(QString::fromLocal8Bit(a_name.c_str())),
#endif
    index(a_index),
    size(a_size)
{
}

/*
 * Notes on archive mode:
 *
 * - request->FrameIndex is the index of the file in the archive to display.
 *
 * - These readers take FrameIndex as an argument and are able to return specific frames without stepping through them one by one:
 *        AvifReader::ReadImage()
 *        HeifReader::ReadImage()
 *   In archive mode DecodeFromBuffer() passes frame index 0 to them, so no animations are available within archives.
 *
 * - These readers deliver consecutive frames of an animation when called repeatedly for the same image:
 *        PngReader::ReadImage()
 *        WebpReaderWriter::ReadImage()
 *        JxlReader::ReadImage()
 *   In archive mode only the first frame is shown. Their caches are dropped after every entry, so a decoder
 *   never outlives the entry buffer (JXL reads directly from it) and never leaks state into the next entry.
 *
 * - GIF, TIFF and ICO entries have no dedicated wrapper: they are decoded from the entry buffer by Qt
 *   (DecodeBufferWithQt), again only the first frame.
 */
void CImageLoadThread::ProcessReadZipRequest(CRequest* request) {
    bool bIsSameArchive = false;
    int nArchiveItemIndex = request->FrameIndex;

    if (request->FileName == m_sLastZipFileName) {
        if (m_nZipCount <= 0) {
            request->Error = ELoadError::ArchiveEmpty;
            return;
        }
        bIsSameArchive = true;
    } else {
        // New ZIP. Delete all cached decoders.
        DeleteCachedQtImageReader();
        DeleteCachedWebpDecoder();
        DeleteCachedPngDecoder();
        DeleteCachedJxlDecoder();
        DeleteCachedAvifDecoder();
        DeleteCachedZip();
    }

    const QString ext = QFileInfo(request->FileName).suffix().toLower();
    const bool use7z = !(ext == QStringLiteral("cbz") || ext == QStringLiteral("zip"));
    const std::string sArchivePath = ToNativeNarrowPath(request->FileName);

    ZipPtr zip; // closes itself on every exit path

    if (!bIsSameArchive) {
        if (use7z) {
            try {
                BitArchiveReader reader(SevenZipLibrary(), sArchivePath, BitFormat::Auto);
                for (const auto& item : reader) {
                    if (!item.isDir()) {
                        // path within the archive (including folders), as for ZIP entries
                        ZipEntry entry(item.path(), item.index(), item.size());
                        if (IsArchiveImageEntry(entry.name)) {
                            zipEntries.push_back(std::move(entry));
                        }
                    }
                }
                // The reader is destroyed here; extraction reopens the archive (extract fails otherwise).
            } catch (const BitException& ex) {
                qDebug() << "bit7z:" << ex.what();
                DeleteCachedZip();
                request->Error = ELoadError::ArchiveError;
                return;
            }
        } else {
            zip.reset(zip_open(sArchivePath.c_str(), 0, 'r'));
            if (!zip) {
                request->Error = ELoadError::ArchiveError;
                return;
            }
            const auto nTotal = zip_entries_total(zip.get());
            for (decltype(zip_entries_total(zip.get())) i = 0; i < nTotal; ++i) {
                if (zip_entry_openbyindex(zip.get(), static_cast<int>(i)) != 0) {
                    continue;
                }
                const char* pchName = zip_entry_name(zip.get());
                if (pchName != nullptr && !zip_entry_isdir(zip.get())) {
                    const QString sName = ZipEntryName(zip.get());
                    if (IsArchiveImageEntry(sName)) {
                        zipEntries.push_back(ZipEntry(sName, static_cast<unsigned int>(i), zip_entry_size(zip.get())));
                    }
                }
                zip_entry_close(zip.get());
            }
        }

        // The list is kept for this archive (also if it has no image at all), entries are never removed from it
        m_nZipCount = static_cast<int>(zipEntries.size());
        m_sLastZipFileName = request->FileName;
        if (m_nZipCount <= 0) {
            request->Error = ELoadError::ArchiveEmpty;
            return;
        }
    }

    nArchiveItemIndex = std::clamp(nArchiveItemIndex, 0, m_nZipCount - 1);
    request->FrameIndex = nArchiveItemIndex;
    // The position in the archive is known from here on, also if this page can not be read
    request->Position.InArchive = true;
    request->Position.FrameIndex = nArchiveItemIndex;
    request->Position.FrameCount = m_nZipCount;

    // One attempt: an entry that can not be read just yields no image, it keeps its index (page number)
    const ZipEntry z = zipEntries[static_cast<size_t>(nArchiveItemIndex)];
    request->Position.EntryName = z.name;
    request->Position.EntrySize = z.size;

    std::unique_ptr<char[]> pBuffer;
    // Declared after pBuffer, so it runs before the buffer is released: drops every decoder that might
    // have cached something for this entry (JXL would otherwise point into a freed buffer).
    ScopeExit dropDecoders([this] {
        DeleteCachedWebpDecoder();
        DeleteCachedPngDecoder();
        DeleteCachedJxlDecoder();
        DeleteCachedAvifDecoder();
    });
    CErrorModeGuard errorModeGuard;

    try {
        const EEntryRead eRead = ReadZipEntry(z, use7z ? m_p7zLibrary.get() : nullptr, sArchivePath, zip, pBuffer);
        if (eRead == EEntryRead::OutOfMemory) {
            request->Error = ELoadError::OutOfMemory;
        } else if (eRead == EEntryRead::TooLarge) {
            request->Error = ELoadError::TooLarge;
        } else if (eRead == EEntryRead::ArchiveError) {
            DeleteCachedZip(); // the archive could not be opened (changed or removed): list it again on the next request
            request->Error = ELoadError::ArchiveError;
        } else if (eRead == EEntryRead::Failed) {
            request->Error = ELoadError::FileNotReadable; // this entry could not be extracted (e.g. CRC error)
        } else if (eRead == EEntryRead::Empty) {
            request->Error = ELoadError::NotDecodable;
        } else if (eRead == EEntryRead::Ok) {
            unsigned char header[16] = { 0 };
            memcpy(header, pBuffer.get(), static_cast<size_t>(std::min<uint64_t>(sizeof(header), z.size)));
            const EImageFormat eFormat = GetImageFormatInMemory(header, z.name);
            if (z.size > MaxFileSizeForFormat(eFormat)) {
                request->Error = ELoadError::TooLarge;
            } else {
                const CDecodeTarget target{ .bInArchive = true, .bMultipleFiles = m_nZipCount > 1,
                                           .nArchiveCount = m_nZipCount, .nEntryIndex = nArchiveItemIndex };
                // The reason of a failure is in request->Error (none set: not decodable, see ProcessRequest())
                DecodeFromBuffer(request, eFormat, pBuffer, static_cast<size_t>(z.size), target);
            }
        }
    }
    catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->Error = ELoadError::Exception;
    }

    if (request->Image == nullptr) {
        qDebug() << "ProcessReadZipRequest() Failed to read item index" << nArchiveItemIndex << " name:" << z.name << " bytes:" << z.size;
    }
}

void CImageLoadThread::DeleteCachedZip()
{
    m_sLastZipFileName.clear();
    m_nZipCount = 0;
    zipEntries.clear();
}

// Throws BitException if 7z.so can not be loaded; the next call tries again
Bit7zLibrary& CImageLoadThread::SevenZipLibrary() {
    if (!m_p7zLibrary) {
        m_p7zLibrary = std::make_unique<Bit7zLibrary>(SevenZipLibraryPath());
    }
    return *m_p7zLibrary;
}

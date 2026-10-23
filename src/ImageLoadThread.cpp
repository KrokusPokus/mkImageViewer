#include "ImageLoadThread.h"
#include "Helpers.h"
#include "JPEGImage.h"
#include "MaxImageDef.h"
#include "MainDlg.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "SettingsProvider.h"
#include "wrapperAVIF.h"
#include "wrapperHEIF.h"
#include "wrapperJXL.h"
#include "wrapperPNG.h"
#include "wrapperPSD.h"
#include "wrapperQOI.h"
#include "wrapperTJPEG.h"
#include "wrapperWEBP.h"
#include "zip/zip.h"
#include "libjpeg-turbo/include/turbojpeg.h"

#include <algorithm>
#include <climits>
#include <cstdint>
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
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QString>

#include "bit7z/include/bitarchivereader.hpp"
using namespace bit7z;

// static initializers
std::atomic<int> CImageLoadThread::m_curHandle(0);

// The DIBs handed to CJPEGImage are BGRA. QImage::Format_ARGB32 / Format_RGB32 have exactly this layout on little endian.
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "The 32bpp DIB layout (B,G,R,A) assumes a little endian platform");

/////////////////////////////////////////////////////////////////////////////////////////////
// static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

namespace {

using namespace std::string_view_literals;

#ifdef Q_OS_WIN
constexpr const char* k7zLibraryName = "7z.dll";
#else
constexpr const char* k7zLibraryName = "7z.so";
#endif

// Upper bound for a single archive entry we are willing to inflate into memory.
// The size stored in an archive is attacker controlled, so it must be checked before allocating.
// The wrappers take 'int' sizes, hence the INT_MAX cap.
const uint64_t kMaxZipEntrySize = std::min<uint64_t>(
    std::max({ static_cast<uint64_t>(MAX_JPEG_FILE_SIZE), static_cast<uint64_t>(MAX_WEBP_FILE_SIZE),
              static_cast<uint64_t>(MAX_PNG_FILE_SIZE), static_cast<uint64_t>(MAX_JXL_FILE_SIZE),
              static_cast<uint64_t>(MAX_HEIF_FILE_SIZE) }),
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
#ifdef Q_OS_WIN
    CErrorModeGuard() : m_nPrevErrorMode(::SetErrorMode(SEM_FAILCRITICALERRORS)) {}
    ~CErrorModeGuard() { ::SetErrorMode(m_nPrevErrorMode); }
private:
    UINT m_nPrevErrorMode;
#else
    CErrorModeGuard() {}
    ~CErrorModeGuard() {}
#endif
public:
    CErrorModeGuard(const CErrorModeGuard&) = delete;
    CErrorModeGuard& operator=(const CErrorModeGuard&) = delete;
};

// Reads a whole file into a freshly allocated buffer.
// Returns nullptr on failure. bOutOfMemory is set only if the file is too large or the allocation failed.
std::unique_ptr<char[]> ReadWholeFile(const QString& sFileName, qint64 nMaxSize, qint64& nFileSize, bool& bOutOfMemory) {
    nFileSize = 0;
    bOutOfMemory = false;

    QFile file(sFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    const qint64 nSize = file.size();
    if (nSize <= 0) {
        return nullptr;
    }
    if (nSize > std::min<qint64>(nMaxSize, INT_MAX)) {
        bOutOfMemory = true;
        return nullptr;
    }
    std::unique_ptr<char[]> pBuffer(new (std::nothrow) char[static_cast<size_t>(nSize)]);
    if (!pBuffer) {
        bOutOfMemory = true;
        return nullptr;
    }
    if (file.read(pBuffer.get(), nSize) != nSize) {
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
                                                                  { QStringLiteral("qoi"),  IF_QOI },
                                                                  { QStringLiteral("psd"),  IF_PSD },
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
    } else if (HasSig(h, 0, "8BPS"sv)) {
        return IF_PSD;
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
#ifdef Q_OS_WIN
    // TODO: non-ASCII paths: zip_open() uses fopen() on Windows
    return sFileName.toUtf8().toStdString();
#else
    return QFile::encodeName(sFileName).toStdString();
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Qt image decoding (QImageReader / QImage / QColorSpace)
/////////////////////////////////////////////////////////////////////////////////////////////

// Format and frame information that is handed over to the CJPEGImage constructor
struct CImageFrameInfo {
    EImageFormat eFormat = IF_Unknown;
    bool bIsAnimation = false;
    int nFrameIndex = 0;
    int nFrameCount = 1;
    int nFrameTimeMs = 100;
};

// Maps the format name reported by QImageReader::format() to the image format used by CJPEGImage.
// ICO deliberately stays IF_Unknown
EImageFormat MapQtImageFormat(const QByteArray& sFormat) {
    if (sFormat == "gif") {
        return IF_GIF;
    }
    if (sFormat == "tif" || sFormat == "tiff") {
        return IF_TIFF;
    }
    if (sFormat == "png") {
        return IF_PNG;
    }
    if (sFormat == "jpg" || sFormat == "jpeg") {
        return IF_JPEG;
    }
    if (sFormat == "bmp") {
        return IF_WindowsBMP;
    }
    return IF_Unknown;
}

const QColorSpace& SrgbColorSpace() {
    static const QColorSpace kSrgb(QColorSpace::SRgb);
    return kSrgb;
}

std::unique_ptr<QImageReader> CreateQtImageReader(QIODevice* pDevice) {
    auto pReader = std::make_unique<QImageReader>(pDevice);
    pReader->setDecideFormatFromContent(true); // do not trust the file extension
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

// Converts a decoded QImage into a CJPEGImage (32bpp BGRA DIB).
// - If enabled in the settings, the embedded ICC profile is applied (conversion to sRGB). Images without a profile,
//   or with a profile Qt cannot use, have an invalid colorSpace() and are left untouched.
// - Images with an alpha channel are blended onto the background according to nTransparencyMode.
// pEXIFData is copied by CJPEGImage, the caller keeps ownership.
CJPEGImage* ConvertQImageToJPEGImage(QImage image, void* pEXIFData, const CImageFrameInfo& frame,
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
    image = QImage(); // release the decoded image before the CJPEGImage is created

    if (bHasAlphaChannel) {
        Helpers::BlendAlpha(reinterpret_cast<uint32*>(pDIB.get()), nWidth, nHeight, nTransparencyMode);
    }
    CJPEGImage* pJPEGImage = new CJPEGImage(nWidth, nHeight, pDIB.get(), pEXIFData, 4, 0, frame.eFormat,
                                            frame.bIsAnimation, frame.nFrameIndex, frame.nFrameCount, frame.nFrameTimeMs);
    pDIB.release(); // owned by the CJPEGImage now
    return pJPEGImage;
}

// Decodes the first frame of an image that is already held in memory.
// This is what QImage::fromData() does internally; a QImageReader is used directly because it reports the image
// size before the pixels are decoded, which keeps the MAX_IMAGE_DIMENSION / MAX_IMAGE_PIXELS limits effective.
// pData must stay valid for the duration of the call (it is not copied).
CJPEGImage* DecodeBufferWithQt(const void* pData, int nSize, void* pEXIFData, const CImageFrameInfo& frame,
                               bool& isOutOfMemory, Helpers::ETransparencyMode nTransparencyMode) {
    isOutOfMemory = false;
    try {
        QByteArray data = QByteArray::fromRawData(static_cast<const char*>(pData), nSize);
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::ReadOnly)) {
            return nullptr;
        }
        const std::unique_ptr<QImageReader> pReader = CreateQtImageReader(&buffer); // destroyed before 'buffer'
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
enum class EEntryRead { Ok, Empty, Failed, ArchiveError, OutOfMemory };

struct ZipCloser {
    void operator()(struct zip_t* pZip) const { zip_close(pZip); }
};
using ZipPtr = std::unique_ptr<struct zip_t, ZipCloser>;

// Reads one archive entry into pBuffer (pBuffer is released again on every failure).
// 'zip' is the (optionally already opened) handle for zip/cbz archives; it is closed again when done.
EEntryRead ReadZipEntry(const ZipEntry& z, bool use7z, const std::string& sArchivePath, ZipPtr& zip, std::unique_ptr<char[]>& pBuffer) {
    pBuffer.reset();
    if (z.size == 0) {
        return EEntryRead::Empty;
    }
    if (z.size > kMaxZipEntrySize) {
        return EEntryRead::OutOfMemory;
    }
    pBuffer.reset(new (std::nothrow) char[static_cast<size_t>(z.size)]);
    if (!pBuffer) {
        return EEntryRead::OutOfMemory;
    }

    if (use7z) {
        try {
            // The archive has to be reopened for every extraction, otherwise extract fails
            Bit7zLibrary lib{ k7zLibraryName };
            BitArchiveReader reader(lib, sArchivePath, BitFormat::Auto);
            try {
                reader.extractTo(reinterpret_cast<byte_t*>(pBuffer.get()), static_cast<size_t>(z.size), z.index);
                return EEntryRead::Ok;
            } catch (const BitException&) {
                pBuffer.reset();
                return EEntryRead::Failed; // only this entry is broken
            }
        } catch (const BitException&) {
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

} // namespace


/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CImageLoadThread::CImageLoadThread(void) : CWorkThread(true) {
}

CImageLoadThread::~CImageLoadThread(void) {
    DeleteCachedQtImageReader();
    DeleteCachedWebpDecoder();
    DeleteCachedPngDecoder();
    DeleteCachedJxlDecoder();
    DeleteCachedAvifDecoder();
    DeleteCachedZip();
}

int CImageLoadThread::AsyncLoad(const QString& strFileName, int nFrameIndex, const CProcessParams & processParams, std::function<void(int)> onCompleted) {
    auto pRequest = std::make_shared<CRequest>(strFileName, nFrameIndex, processParams, onCompleted);
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
                return CImageData(rq.Image, rq.OutOfMemory, rq.ExceptionError);
            }
        }
        return CImageData(nullptr, false, false);
    });
}

void CImageLoadThread::ReleaseFile(const QString& strFileName) {
    try { ProcessAndWait(std::make_shared<CReleaseFileRequest>(strFileName)); } catch (...) {}
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
    const UINT startTimeMs = Helpers::GetTimeMs();
    // Get image format and read the image
    switch (GetImageFormat(rq.FileName)) {
    case IF_JPEG:
        dropCaches(Keep_None);
        ProcessReadJPEGRequest(&rq);
        break;
    case IF_WindowsBMP:
        dropCaches(Keep_None);
        ProcessReadBMPRequest(&rq);
        break;
    case IF_TGA:
        dropCaches(Keep_None);
        ProcessReadTGARequest(&rq);
        break;
    case IF_WEBP:
        dropCaches(Keep_Webp);
        ProcessReadWEBPRequest(&rq);
        break;
    case IF_PNG:
        dropCaches(Keep_Png);
        ProcessReadPNGRequest(&rq);
        break;
    case IF_JXL:
        dropCaches(Keep_Jxl);
        ProcessReadJXLRequest(&rq);
        break;
    case IF_AVIF:
        dropCaches(Keep_Avif);
        ProcessReadAVIFRequest(&rq);
        break;
    case IF_HEIF:
        dropCaches(Keep_None);
        ProcessReadHEIFRequest(&rq);
        break;
    case IF_PSD:
        dropCaches(Keep_None);
        ProcessReadPSDRequest(&rq);
        break;
    case IF_QOI:
        dropCaches(Keep_None);
        ProcessReadQOIRequest(&rq);
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
    // then process the image if read was successful
    if (rq.Image != nullptr) {
        rq.Image->SetFileLoadDelayMs(Helpers::GetTimeMs() - startTimeMs);
        if (!ProcessImageAfterLoad(&rq)) {
            delete rq.Image;
            rq.Image = nullptr;
            rq.OutOfMemory = true;
        }
    }
}

// Called on the processing thread
void CImageLoadThread::AfterFinishProcess(CRequestBase& request) {
    if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
        return;
    }

    CRequest& rq = static_cast<CRequest&>(request);
    // Invokes the completion callback that was passed in by CJPEGProvider
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
    offsets.x = std::clamp<LONG>(offsets.x, -nMaxOffsetX, +nMaxOffsetX);
    offsets.y = std::clamp<LONG>(offsets.y, -nMaxOffsetY, +nMaxOffsetY);
}

void CImageLoadThread::DeleteCachedQtImageReader() {
    // Order matters: the reader uses the file as its device
    m_pLastImageReader.reset();
    m_pLastImageFile.reset();
    m_sLastFileName.clear();
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

void CImageLoadThread::ProcessReadJPEGRequest(CRequest * request) {
    bool bOutOfMemory = false;
    qint64 nFileSize = 0;
    std::unique_ptr<char[]> pBuffer = ReadWholeFile(request->FileName, MAX_JPEG_FILE_SIZE, nFileSize, bOutOfMemory);
    if (!pBuffer) {
        request->OutOfMemory = bOutOfMemory;
        return;
    }
    const int nSize = static_cast<int>(nFileSize);

    try {
        int nWidth = 0, nHeight = 0, nBPP = 0;
        TJSAMP eChromoSubSampling = TJSAMP_420;
        bool bDecodeOutOfMemory = false;

        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
            TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bDecodeOutOfMemory, pBuffer.get(), nSize)));

        if (pPixelData && (nBPP == 3 || nBPP == 1)) {
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(),
                                            Helpers::FindEXIFBlock(pBuffer.get(), nSize), nBPP,
                                            Helpers::CalculateJPEGFileHash(pBuffer.get(), nSize), IF_JPEG, false, 0, 1, 0);
            pPixelData.release(); // owned by the CJPEGImage now
            request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer.get(), nSize));
            request->Image->SetJPEGChromoSampling(eChromoSubSampling);
        } else if (bDecodeOutOfMemory) {
            request->OutOfMemory = true;
        } else {
            pPixelData.reset();
            ProcessReadQtImageRequest(request);
        }
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
    }
}

void CImageLoadThread::ProcessReadBMPRequest(CRequest * request) {
    bool bOutOfMemory = false;
    request->Image = CReaderBMP::ReadBmpImage(reinterpret_cast<LPCWSTR>(request->FileName.utf16()), bOutOfMemory);
    if (bOutOfMemory) {
        request->OutOfMemory = true;
    } else if (request->Image == nullptr) {
        ProcessReadQtImageRequest(request);
    }
}

void CImageLoadThread::ProcessReadTGARequest(CRequest * request) {
    bool bOutOfMemory = false;
    request->Image = CReaderTGA::ReadTgaImage(reinterpret_cast<LPCWSTR>(request->FileName.utf16()), request->ProcessParams.TransparencyMode, bOutOfMemory);
    if (bOutOfMemory) {
        request->OutOfMemory = true;
    }
}

// Buffer ownership: WebpReaderWriter copies the data it needs for animations into its own cache,
// the file buffer can always be released right after the call.
void CImageLoadThread::ProcessReadWEBPRequest(CRequest * request) {
    const bool bUseCachedDecoder = (request->FileName == m_sLastWebpFileName);
    if (!bUseCachedDecoder) {
        DeleteCachedWebpDecoder();
    }

    std::unique_ptr<char[]> pBuffer;
    qint64 nFileSize = 0;
    if (!bUseCachedDecoder) {
        bool bOutOfMemory = false;
        pBuffer = ReadWholeFile(request->FileName, MAX_WEBP_FILE_SIZE, nFileSize, bOutOfMemory);
        if (!pBuffer) {
            request->OutOfMemory = bOutOfMemory;
            return;
        }
    }

    try {
        int nWidth = 0, nHeight = 0, nBPP = 0;
        bool bHasAnimation = bUseCachedDecoder;
        int nFrameCount = 1;
        int nFrameTimeMs = 0;
        void* pEXIFData = nullptr;
        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
            WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData,
                                        request->OutOfMemory, pBuffer.get(), static_cast<int>(nFileSize))));
        if (pPixelData && nBPP == 4) {
            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
            if (bHasAnimation) {
                m_sLastWebpFileName = request->FileName;
            }
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, nBPP, 0, IF_WEBP, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
            pPixelData.release();
        } else {
            pPixelData.reset();
            DeleteCachedWebpDecoder();
        }
        free(pEXIFData);
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
        DeleteCachedWebpDecoder();
    }
}

// Buffer ownership: PngReader copies the file into its own cache (without the 8 byte signature),
// the file buffer can always be released right after the call.
void CImageLoadThread::ProcessReadPNGRequest(CRequest* request) {
    const bool bUseCachedDecoder = (request->FileName == m_sLastPngFileName);
    if (!bUseCachedDecoder) {
        DeleteCachedPngDecoder();
    }

    std::unique_ptr<char[]> pBuffer;
    qint64 nFileSize = 0;
    if (!bUseCachedDecoder) {
        bool bOutOfMemory = false;
        pBuffer = ReadWholeFile(request->FileName, MAX_PNG_FILE_SIZE, nFileSize, bOutOfMemory);
        if (!pBuffer) {
            request->OutOfMemory = bOutOfMemory;
            return;
        }
    }

    try {
        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
        bool bHasAnimation = false;
        void* pEXIFData = nullptr;

        // Note: PngReader allocates the pixel data with malloc(), it is handed over to the CJPEGImage as is.
        uint8* pPixelData = static_cast<uint8*>(
            PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData,
                                 request->OutOfMemory, pBuffer.get(), static_cast<size_t>(nFileSize)));
        if (pPixelData != nullptr) {
            if (bHasAnimation) {
                m_sLastPngFileName = request->FileName;
            }
            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData), nWidth, nHeight, request->ProcessParams.TransparencyMode);
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
            free(pEXIFData);
        } else {
            free(pEXIFData);
            DeleteCachedPngDecoder();

            if (!request->OutOfMemory && pBuffer) {
                // libpng could not decode the file: give Qt a chance, the file is already in memory
                void* pQtEXIFData = PngReader::GetEXIFBlock(pBuffer.get(), static_cast<size_t>(nFileSize));
                bool bQtOutOfMemory = false;
                request->Image = DecodeBufferWithQt(pBuffer.get(), static_cast<int>(nFileSize), pQtEXIFData,
                                                    CImageFrameInfo{ .eFormat = IF_PNG }, bQtOutOfMemory,
                                                    request->ProcessParams.TransparencyMode);
                free(pQtEXIFData);
                request->OutOfMemory = request->Image == nullptr && bQtOutOfMemory;
            }
        }
    }
    catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
        DeleteCachedPngDecoder();
    }
}

// Buffer ownership: unlike all other wrappers, JxlReader does NOT copy the input data. Its decoder keeps
// reading from the buffer for as long as an animation is cached. In that case the buffer is moved into
// m_pJxlBuffer and released in DeleteCachedJxlDecoder() - strictly after JxlReader::DeleteCache().
void CImageLoadThread::ProcessReadJXLRequest(CRequest* request) {
    const bool bUseCachedDecoder = (request->FileName == m_sLastJxlFileName);
    if (!bUseCachedDecoder) {
        DeleteCachedJxlDecoder();
    }

    std::unique_ptr<char[]> pBuffer;
    qint64 nFileSize = 0;
    if (!bUseCachedDecoder) {
        bool bOutOfMemory = false;
        pBuffer = ReadWholeFile(request->FileName, MAX_JXL_FILE_SIZE, nFileSize, bOutOfMemory);
        if (!pBuffer) {
            request->OutOfMemory = bOutOfMemory;
            return;
        }
    }

    CErrorModeGuard errorModeGuard;
    try {
        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
        bool bHasAnimation = false;
        void* pEXIFData = nullptr;
        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
            JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData,
                                 request->OutOfMemory, pBuffer.get(), static_cast<int>(nFileSize))));
        if (pPixelData) {
            if (bHasAnimation) {
                // the decoder still points into this buffer
                m_pJxlBuffer = std::move(pBuffer);
                m_sLastJxlFileName = request->FileName;
            }
            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, 4, 0, IF_JXL, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
            pPixelData.release();
        } else {
            DeleteCachedJxlDecoder();
        }
        free(pEXIFData);
    }
    catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
        DeleteCachedJxlDecoder();
    }
}

// Buffer ownership: AvifReader copies the data into its own cache (malloc), the file buffer can be released right away.
void CImageLoadThread::ProcessReadAVIFRequest(CRequest* request) {
    const bool bUseCachedDecoder = (request->FileName == m_sLastAvifFileName);
    if (!bUseCachedDecoder) {
        DeleteCachedAvifDecoder();
    }

    std::unique_ptr<char[]> pBuffer;
    qint64 nFileSize = 0;
    if (!bUseCachedDecoder) {
        bool bOutOfMemory = false;
        pBuffer = ReadWholeFile(request->FileName, MAX_HEIF_FILE_SIZE, nFileSize, bOutOfMemory);
        if (!pBuffer) {
            request->OutOfMemory = bOutOfMemory;
            return;
        }
    }

    bool bSuccess = false;
    {
        CErrorModeGuard errorModeGuard;
        try {
            int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
            bool bHasAnimation = false;
            void* pEXIFData = nullptr;
            std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, request->FrameIndex,
                                      nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer.get(), static_cast<int>(nFileSize))));
            if (pPixelData) {
                if (bHasAnimation) {
                    m_sLastAvifFileName = request->FileName;
                }
                Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, 4, 0, IF_AVIF, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
                pPixelData.release();
                bSuccess = true;
            } else {
                DeleteCachedAvifDecoder();
            }
            free(pEXIFData);
        }
        catch (...) {
            delete request->Image;
            request->Image = nullptr;
            request->ExceptionError = true;
            DeleteCachedAvifDecoder();
        }
    }
    if (!bSuccess && !request->OutOfMemory) {
        ProcessReadHEIFRequest(request);
    }
}

// Buffer ownership: HeifReader reads from the buffer only during the call (no copy kept).
void CImageLoadThread::ProcessReadHEIFRequest(CRequest* request) {
    bool bOutOfMemory = false;
    qint64 nFileSize = 0;
    std::unique_ptr<char[]> pBuffer = ReadWholeFile(request->FileName, MAX_HEIF_FILE_SIZE, nFileSize, bOutOfMemory);
    if (!pBuffer) {
        request->OutOfMemory = bOutOfMemory;
        return;
    }

    CErrorModeGuard errorModeGuard;
    try {
        int nWidth = 0, nHeight = 0, nBPP = 0;
        int nFrameCount = 1;
        const int nFrameTimeMs = 0;
        void* pEXIFData = nullptr;
        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
            HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex,
                                  pBuffer.get(), static_cast<int>(nFileSize))));
        if (pPixelData) {
            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, nBPP, 0, IF_HEIF, false, request->FrameIndex, nFrameCount, nFrameTimeMs);
            pPixelData.release();
        }
        free(pEXIFData);
    } catch (const heif::Error&) {
        delete request->Image;
        request->Image = nullptr;
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
    }
}

void CImageLoadThread::ProcessReadPSDRequest(CRequest* request) {
    request->Image = PsdReader::ReadImage(reinterpret_cast<LPCWSTR>(request->FileName.utf16()), request->ProcessParams.TransparencyMode, request->OutOfMemory);
    if (request->Image == nullptr && !request->OutOfMemory) {
        request->Image = PsdReader::ReadThumb(reinterpret_cast<LPCWSTR>(request->FileName.utf16()), request->OutOfMemory);
    }
}

void CImageLoadThread::ProcessReadQOIRequest(CRequest* request) {
    bool bOutOfMemory = false;
    qint64 nFileSize = 0;
    std::unique_ptr<char[]> pBuffer = ReadWholeFile(request->FileName, MAX_PNG_FILE_SIZE, nFileSize, bOutOfMemory);
    if (!pBuffer) {
        request->OutOfMemory = bOutOfMemory;
        return;
    }

    try {
        int nWidth = 0, nHeight = 0, nBPP = 0;
        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
            QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer.get(), static_cast<int>(nFileSize))));
        if (pPixelData) {
            if (nBPP == 4) {
                Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
            }
            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), nullptr, nBPP, 0, IF_QOI, false, 0, 1, 0);
            pPixelData.release();
        }
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
    }
}

// Loads an image with Qt (QImageReader). Used for GIF, TIFF, ICO, and as a fallback if the dedicated JPEG/BMP decoders fail.
// Multi-frame images: the frame to show is request->FrameIndex.
//  - GIF:  Qt's GIF handler can only decode forward (jumpToImage() is not supported). The QImageReader and its file are
//          therefore cached while the animation is shown: the next frame is a plain read(). Any other frame rewinds the
//          file and decodes forward (frames before the requested one are decoded and thrown away).
//          The cache is dropped by ReleaseFile() or when another file is loaded.
//  - TIFF: pages are random access (jumpToImage()), nothing is cached.
void CImageLoadThread::ProcessReadQtImageRequest(CRequest * request) {
    try {
        const bool bUseCachedReader = m_pLastImageReader && request->FileName == m_sLastFileName;
        if (!bUseCachedReader) {
            DeleteCachedQtImageReader();
            auto pFile = std::make_unique<QFile>(request->FileName);
            if (!pFile->open(QIODevice::ReadOnly)) {
                return;
            }
            m_pLastImageFile = std::move(pFile);
            m_pLastImageReader = CreateQtImageReader(m_pLastImageFile.get());
            m_sLastFileName = request->FileName;
        }

        const EImageFormat eFormat = MapQtImageFormat(m_pLastImageReader->format());
        if (!bUseCachedReader) {
            // imageCount() has to be queried BEFORE size(): with size() called first the TIFF handler reports a single
            // page for multi-page files (observed with Qt 6.12).
            if (eFormat == IF_GIF || eFormat == IF_TIFF) {
                m_nLastQtFrameCount = std::max(1, m_pLastImageReader->imageCount());
            }
            // reject too large images before any pixel is decoded
            if (!IsImageSizeAcceptable(m_pLastImageReader->size(), request->OutOfMemory)) {
                DeleteCachedQtImageReader();
                return;
            }
        }

        const int nFrameCount = m_nLastQtFrameCount;
        const int nFrameIndex = std::clamp(request->FrameIndex, 0, nFrameCount - 1);
        const bool bIsAnimatedGif = (eFormat == IF_GIF && nFrameCount > 1);

        QImage image;
        if (eFormat == IF_TIFF && nFrameCount > 1) {
            if (m_pLastImageReader->jumpToImage(nFrameIndex)) {
                image = m_pLastImageReader->read();
            }
        } else if (bIsAnimatedGif) {
            if (nFrameIndex <= m_nLastQtFrameIndex) {
                // rewind: a new reader on the file at position 0 starts again with the first frame
                m_pLastImageReader.reset();
                if (!m_pLastImageFile->seek(0)) {
                    DeleteCachedQtImageReader();
                    return;
                }
                m_pLastImageReader = CreateQtImageReader(m_pLastImageFile.get());
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
                                                  CImageFrameInfo{ eFormat, bIsAnimatedGif, nFrameIndex, nFrameCount, nFrameTimeMs },
                                                  bOutOfMemory, request->ProcessParams.TransparencyMode);
        request->OutOfMemory = request->Image == nullptr && bOutOfMemory;
        if (!bIsAnimatedGif) {
            DeleteCachedQtImageReader();
        }
    } catch (const std::bad_alloc&) {
        delete request->Image;
        request->Image = nullptr;
        request->OutOfMemory = true;
        DeleteCachedQtImageReader();
    } catch (...) {
        delete request->Image;
        request->Image = nullptr;
        request->ExceptionError = true;
        DeleteCachedQtImageReader();
    }
}

bool CImageLoadThread::ProcessImageAfterLoad(CRequest * request) {
    // set process parameters depending on filename
    request->Image->SetFileDependentProcessParams(request->FileName, &(request->ProcessParams));

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

    // this will process the image and cache the processed DIB in the CJPEGImage instance
    const CPoint offsetInImage = request->Image->ConvertOffset(newSize, clippedSize, request->ProcessParams.Offsets);
    return nullptr != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
                                             request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags);
}


// #################################################

ZipEntry::ZipEntry(const char* a_pchName, unsigned int a_index, unsigned long long a_size) :
    name(QString::fromUtf8(a_pchName)),
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

//default frame interval in ms for images in CB* archives: 1hr
#define DEF_MANGA_FRAME_TIME_MS 360000
#define EXTRA_MANGA_PARAMS IF_ZIP, bMultipleFiles, request->FrameIndex, m_nZipCount, DEF_MANGA_FRAME_TIME_MS

/*
 * Notes on archive mode:
 *
 * - request->FrameIndex is the index of the file in the archive to display.
 *
 * - These readers take FrameIndex as an argument and are able to return specific frames without stepping through them one by one:
 *        AvifReader::ReadImage()
 *        HeifReader::ReadImage()
 *   In archive mode they get "nFrameIndexFixed = 0", so no animations are available within archives.
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
 *
 * - These readers take it as an argument but do nothing with it and internally just hand it over to CJPEGImage():
 *        CReaderBMP::ReadBmpImage()
 *        RawReader::ReadImage()
 */
void CImageLoadThread::ProcessReadZipRequest(CRequest* request) {
    bool bIsSameArchive = false;
    bool bMultipleFiles = false;
    int nArchiveItemIndex = request->FrameIndex;
    constexpr int nFrameIndexFixed = 0;

    if (request->FileName == m_sLastZipFileName) {
        if (m_nZipCount <= 0) {
            return; // abort as no valid image entries in archive; all marked as non-image
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
                Bit7zLibrary lib{ k7zLibraryName };
                BitArchiveReader reader(lib, sArchivePath, BitFormat::Auto);
                for (const auto& item : reader) {
                    if (!item.isDir()) {
                        zipEntries.push_back(ZipEntry(item.name(), item.index(), item.size()));
                    }
                }
                // The reader is destroyed here; extraction reopens the archive (extract fails otherwise).
            } catch (const BitException&) {
                DeleteCachedZip();
                return;
            }
        } else {
            zip.reset(zip_open(sArchivePath.c_str(), 0, 'r'));
            if (!zip) {
                return;
            }
            const auto nTotal = zip_entries_total(zip.get());
            for (decltype(zip_entries_total(zip.get())) i = 0; i < nTotal; ++i) {
                if (zip_entry_openbyindex(zip.get(), static_cast<int>(i)) != 0) {
                    continue;
                }
                const char* pchName = zip_entry_name(zip.get());
                if (pchName != nullptr && !zip_entry_isdir(zip.get())) {
                    zipEntries.push_back(ZipEntry(pchName, static_cast<unsigned int>(i), zip_entry_size(zip.get())));
                }
                zip_entry_close(zip.get());
            }
        }

        m_nZipCount = static_cast<int>(zipEntries.size()); // count of valid entries only
        if (m_nZipCount <= 0) {
            return;
        }
        if (m_nZipCount > 1) {
            m_sLastZipFileName = request->FileName;
        }
    }

    bMultipleFiles = m_nZipCount > 1;
    nArchiveItemIndex = std::clamp(nArchiveItemIndex, 0, m_nZipCount - 1);
    request->FrameIndex = nArchiveItemIndex;

    bool bSuccess = false;
    while (!bSuccess) {
        const ZipEntry z = zipEntries[static_cast<size_t>(nArchiveItemIndex)];

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

        bool bAbort = false; // stop without removing the entry from the list

        try {
            const EEntryRead eRead = ReadZipEntry(z, use7z, sArchivePath, zip, pBuffer);
            if (eRead == EEntryRead::OutOfMemory) {
                request->OutOfMemory = true;
                bAbort = true;
            } else if (eRead == EEntryRead::ArchiveError) {
                DeleteCachedZip(); // force a fresh listing on the next request
                bAbort = true;
            } else if (eRead == EEntryRead::Ok) {
                unsigned char header[16] = { 0 };
                memcpy(header, pBuffer.get(), static_cast<size_t>(std::min<uint64_t>(sizeof(header), z.size)));
                const EImageFormat eFormat = GetImageFormatInMemory(header, z.name);
                const int nSize = static_cast<int>(z.size); // z.size <= kMaxZipEntrySize <= INT_MAX

                if (z.size > MaxFileSizeForFormat(eFormat)) {
                    request->OutOfMemory = true;
                    bAbort = true;
                } else {
                    switch (eFormat) {
                    case IF_JXL:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
                        bool bHasAnimation = false;
                        void* pEXIFData = nullptr;
                        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                            JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer.get(), nSize)));
                        if (pPixelData) {
                            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, 4, 0, IF_JXL, EXTRA_MANGA_PARAMS);
                            pPixelData.release();
                            bSuccess = true;
                        }
                        free(pEXIFData);
                    }
                    break;
                    case IF_JPEG:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0;
                        TJSAMP eChromoSubSampling = TJSAMP_420;
                        bool bDecodeOutOfMemory = false;
                        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                            TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bDecodeOutOfMemory, pBuffer.get(), nSize)));
                        // Color and b/w JPEG is supported
                        if (pPixelData && (nBPP == 3 || nBPP == 1)) {
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(),
                                                            Helpers::FindEXIFBlock(pBuffer.get(), nSize), nBPP,
                                                            Helpers::CalculateJPEGFileHash(pBuffer.get(), nSize), IF_JPEG, EXTRA_MANGA_PARAMS);
                            pPixelData.release();
                            request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer.get(), nSize));
                            request->Image->SetJPEGChromoSampling(eChromoSubSampling);
                            bSuccess = true;
                        } else if (bDecodeOutOfMemory) {
                            request->OutOfMemory = true;
                        }
                    }
                    break;
                    case IF_PNG:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
                        bool bHasAnimation = false;
                        void* pEXIFData = nullptr;
                        // Note: PngReader allocates the pixel data with malloc(), it is handed over to the CJPEGImage as is.
                        uint8* pPixelData = static_cast<uint8*>(
                            PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer.get(), static_cast<size_t>(z.size)));
                        if (pPixelData != nullptr) {
                            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, EXTRA_MANGA_PARAMS);
                            bSuccess = true;
                        }
                        free(pEXIFData);
                    }
                    break;
                    case IF_WEBP:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
                        bool bHasAnimation = false;
                        void* pEXIFData = nullptr;
                        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                            WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer.get(), nSize)));
                        if (pPixelData && nBPP == 4) {
                            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, nBPP, 0, IF_WEBP, EXTRA_MANGA_PARAMS);
                            pPixelData.release();
                            bSuccess = true;
                        }
                        free(pEXIFData);
                    }
                    break;
                    case IF_QOI:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0;
                        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                            QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer.get(), nSize)));
                        if (pPixelData) {
                            if (nBPP == 4) {
                                Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                            }
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), nullptr, nBPP, 0, IF_QOI, EXTRA_MANGA_PARAMS);
                            pPixelData.release();
                            bSuccess = true;
                        }
                    }
                    break;
                    case IF_WindowsBMP:
                    {
                        bool bDecodeOutOfMemory = false;
                        request->Image = CReaderBMP::ReadBmpImage(pBuffer.get(), nSize, bDecodeOutOfMemory, EXTRA_MANGA_PARAMS);
                        if (request->Image != nullptr) {
                            bSuccess = true;
                        }
                        if (bDecodeOutOfMemory) {
                            request->OutOfMemory = true;
                        }
                    }
                    break;
                    case IF_AVIF:
                    {
                        int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
                        bool bHasAnimation = false;
                        void* pEXIFData = nullptr;
                        std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                            AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameIndexFixed, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer.get(), nSize)));
                        if (pPixelData) {
                            Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                            request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, 4, 0, IF_AVIF, EXTRA_MANGA_PARAMS);
                            pPixelData.release();
                            bSuccess = true;
                        }
                        free(pEXIFData);
                    }
                    break;
                    case IF_HEIF:
                    {
                        try {
                            int nWidth = 0, nHeight = 0, nBPP = 0, nFrameCount = 1, nFrameTimeMs = 0;
                            void* pEXIFData = nullptr;
                            std::unique_ptr<uint8_t[]> pPixelData(static_cast<uint8_t*>(
                                HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, nFrameIndexFixed, pBuffer.get(), nSize)));
                            if (pPixelData) {
                                Helpers::BlendAlpha(reinterpret_cast<uint32*>(pPixelData.get()), nWidth, nHeight, request->ProcessParams.TransparencyMode);
                                request->Image = new CJPEGImage(nWidth, nHeight, pPixelData.get(), pEXIFData, nBPP, 0, IF_HEIF, EXTRA_MANGA_PARAMS);
                                pPixelData.release();
                                bSuccess = true;
                            }
                            free(pEXIFData);
                        }
                        catch (const heif::Error&) {
                            delete request->Image;
                            request->Image = nullptr;
                        }
                    }
                    break;
                    case IF_GIF:
                    case IF_TIFF:
                    case IF_ICO:
                    {
                        // No dedicated wrapper: Qt decodes the entry straight from the buffer (first frame only)
                        bool bQtOutOfMemory = false;
                        request->Image = DecodeBufferWithQt(pBuffer.get(), nSize, nullptr, CImageFrameInfo{ EXTRA_MANGA_PARAMS },
                                                            bQtOutOfMemory, request->ProcessParams.TransparencyMode);
                        if (request->Image != nullptr) {
                            bSuccess = true;
                        }
                        if (bQtOutOfMemory) {
                            request->OutOfMemory = true;
                        }
                    }
                    break;
                    default:
                        // not an image format that can be decoded from a buffer
                        break;
                    }
                }
            }
            // eRead == Empty or Failed: falls through, the entry is skipped like a non-image
        }
        catch (...) {
            delete request->Image;
            request->Image = nullptr;
            bSuccess = false;
            request->ExceptionError = true;
            bAbort = true;
        }

        if (bSuccess || bAbort || request->OutOfMemory || request->ExceptionError) {
            break;
        }

        // The entry is definitely not a (supported) image: erase it and try the next file in the archive
        qDebug() << "ProcessReadZipRequest() Failed to open item index" << nArchiveItemIndex << " name:" << z.name << " bytes:" << z.size;

        zipEntries.erase(zipEntries.begin() + nArchiveItemIndex);
        if ((--m_nZipCount <= 0) || (nArchiveItemIndex >= m_nZipCount)) {
            break;
        }
        request->FrameIndex = nArchiveItemIndex;
        bMultipleFiles = m_nZipCount > 1;
    }
}

void CImageLoadThread::DeleteCachedZip()
{
    m_sLastZipFileName.clear();
    m_nZipCount = 0;
    zipEntries.clear();
}

#include "wrapperAVIF.h"

#include "avif/avif.h"
#include "BasicProcessing.h"
#include "ICCProfileTransform.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace {

struct AvifCache {
    avifDecoder* decoder = nullptr;
    avifRGBImage rgb{};
    std::uint8_t* data = nullptr;   // copy of the file, libavif reads from it while the decoder lives
    std::size_t data_size = 0;
    void* transform = nullptr;
};

AvifCache g_cache;

// Number of threads libavif may use (the library default is 1).
int ThreadCount() {
    const unsigned int hw = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(hw, 1u, 256u));
}

// Builds a JPEG APP1 segment: FF E1, big endian length, "Exif\0\0", payload. Returns nullptr when it does not fit.
void* BuildExifSegment(const std::uint8_t* exif, std::size_t size) {
    if (exif == nullptr || size <= 8 || size >= 65528) {
        return nullptr;
    }
    auto* segment = static_cast<std::uint8_t*>(std::malloc(size + 10));
    if (segment == nullptr) {
        return nullptr;
    }
    const std::size_t length = size + 8;   // length field + "Exif\0\0" + payload
    segment[0] = 0xFF;
    segment[1] = 0xE1;
    segment[2] = static_cast<std::uint8_t>(length >> 8);
    segment[3] = static_cast<std::uint8_t>(length & 0xFF);
    std::memcpy(segment + 4, "Exif\0\0", 6);
    std::memcpy(segment + 10, exif, size);
    return segment;
}

// Replaces the pixels of the cached frame by a transformed copy (the transform functions return new[] memory).
void ReplacePixels(void* newPixels) {
    if (newPixels != nullptr) {
        delete[] g_cache.rgb.pixels;
        g_cache.rgb.pixels = static_cast<std::uint8_t*>(newPixels);
    }
}

} // namespace

void* AvifReader::ReadImage(int& width,
    int& height,
    int& bpp,
    bool& has_animation,
    int frame_index,
    int& frame_count,
    int& frame_time,
    void*& exif_chunk,
    bool& outOfMemory,
    const void* buffer,
    std::size_t sizebytes)
{
    outOfMemory = false;
    width = height = 0;
    bpp = 4;
    has_animation = false;
    frame_count = 1;
    frame_time = 0;
    exif_chunk = nullptr;

    const int nthreads = ThreadCount();

    // Cache animations: the decoder is created on the first call and kept while the animation is read.
    if (g_cache.decoder == nullptr) {
        if (buffer == nullptr || sizebytes == 0) {
            return nullptr;
        }
        g_cache.data = static_cast<std::uint8_t*>(std::malloc(sizebytes));
        if (g_cache.data == nullptr) {
            outOfMemory = true;
            return nullptr;
        }
        std::memcpy(g_cache.data, buffer, sizebytes);
        g_cache.data_size = sizebytes;

        g_cache.decoder = avifDecoderCreate();
        if (g_cache.decoder == nullptr) {
            DeleteCache();
            outOfMemory = true;
            return nullptr;
        }
        g_cache.decoder->maxThreads = nthreads;
        g_cache.decoder->strictFlags = AVIF_STRICT_DISABLED;
        if (avifDecoderSetIOMemory(g_cache.decoder, g_cache.data, g_cache.data_size) != AVIF_RESULT_OK ||
            avifDecoderParse(g_cache.decoder) != AVIF_RESULT_OK) {
            DeleteCache();
            return nullptr;
        }
        g_cache.rgb = avifRGBImage{};
    }

    // Decode a frame
    if (avifDecoderNthImage(g_cache.decoder, static_cast<std::uint32_t>(std::max(frame_index, 0))) != AVIF_RESULT_OK) {
        DeleteCache();
        return nullptr;
    }
    avifRGBImageSetDefaults(&g_cache.rgb, g_cache.decoder->image);
    g_cache.rgb.depth = 8;
    g_cache.rgb.format = AVIF_RGB_FORMAT_BGRA;
    g_cache.rgb.maxThreads = nthreads;

    width = static_cast<int>(g_cache.rgb.width);
    height = static_cast<int>(g_cache.rgb.height);
    has_animation = g_cache.decoder->imageCount > 1;
    frame_count = g_cache.decoder->imageCount;
    frame_time = static_cast<int>(g_cache.decoder->imageTiming.duration * 1000.0);

    const std::size_t size = static_cast<std::size_t>(width) * bpp * height;
    g_cache.rgb.pixels = new(std::nothrow) std::uint8_t[size];
    if (g_cache.rgb.pixels == nullptr) {
        outOfMemory = true;
        DeleteCache();
        return nullptr;
    }
    g_cache.rgb.rowBytes = static_cast<std::uint32_t>(width * bpp);
    if (avifImageYUVToRGB(g_cache.decoder->image, &g_cache.rgb) != AVIF_RESULT_OK) {
        delete[] g_cache.rgb.pixels;
        g_cache.rgb.pixels = nullptr;
        DeleteCache();
        return nullptr;
    }

    // Handle clap, irot and imir boxes
    const avifTransformFlags flags = g_cache.decoder->image->transformFlags;
    if (flags & AVIF_TRANSFORM_CLAP) {
        avifCropRect crop;
        avifDiagnostics diag;
        if (avifCropRectConvertCleanApertureBox(&crop, &g_cache.decoder->image->clap, width, height,
                g_cache.decoder->image->yuvFormat, &diag)) {
            const CPoint point(static_cast<int>(crop.x), static_cast<int>(crop.y));
            const CSize cropSize(static_cast<int>(crop.width), static_cast<int>(crop.height));
            void* pixels = CBasicProcessing::Crop32bpp(width, height, g_cache.rgb.pixels, CRect(point, cropSize));
            if (pixels != nullptr) {
                ReplacePixels(pixels);
                width = cropSize.cx;
                height = cropSize.cy;
            }
        }
    }
    if (flags & AVIF_TRANSFORM_IROT) {
        const int angle = 360 - g_cache.decoder->image->irot.angle * 90;
        if (angle != 360) {
            void* pixels = CBasicProcessing::Rotate32bpp(width, height, g_cache.rgb.pixels, angle);
            if (pixels != nullptr) {
                ReplacePixels(pixels);
                if (angle != 180) {
                    std::swap(width, height);
                }
            }
        }
    }
    if (flags & AVIF_TRANSFORM_IMIR) {
        ReplacePixels(CBasicProcessing::Mirror32bpp(width, height, g_cache.rgb.pixels, g_cache.decoder->image->imir.axis));
    }

    // The color transform only depends on the ICC profile, it is created once per cached file.
    const avifRWData& icc = g_cache.decoder->image->icc;
    if (g_cache.transform == nullptr) {
        g_cache.transform = ICCProfileTransform::CreateTransform(icc.data, icc.size, ICCProfileTransform::FORMAT_BGRA);
    }
    ICCProfileTransform::DoTransform(g_cache.transform, g_cache.rgb.pixels, g_cache.rgb.pixels, width, height);

    const avifRWData& exif = g_cache.decoder->image->exif;
    exif_chunk = BuildExifSegment(exif.data, exif.size);

    // Ownership of the pixels goes to the caller
    void* pPixelData = g_cache.rgb.pixels;
    g_cache.rgb.pixels = nullptr;
    if (!has_animation) {
        DeleteCache();
    }

    return pPixelData;
}

void AvifReader::DeleteCache() {
    if (g_cache.decoder != nullptr) {
        avifDecoderDestroy(g_cache.decoder);
    }
    std::free(g_cache.data);
    if (g_cache.transform != nullptr) {
        ICCProfileTransform::DeleteTransform(g_cache.transform);
    }
    g_cache = AvifCache{};
}

#include "wrapperPNG.h"

#include "png.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

// Uncomment to build without APNG support
//#undef PNG_APNG_SUPPORTED

/*
 * Modified from "load4apng.c"
 * Original: https://sourceforge.net/projects/apng/files/libpng/examples/
 *
 * loads APNG from memory one frame at a time (32bpp), restarts when done
 * including frames composition.
 *
 * needs libpng with APNG support (libpng >= 1.6.4x official, or the apng-patched libpng).
 *
 * Copyright (c) 2012-2014 Max Stepin
 * maxst at users.sourceforge.net
 *
 * zlib license
 * ------------
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

namespace {

constexpr std::size_t kSignatureSize = 8;           // PNG signature, libpng is told to skip it
constexpr std::uint32_t kMaxChunkSize = 0x7FFFFFFFu; // PNG_UINT_31_MAX
constexpr std::uint32_t kMaxExifSize = 65528;        // + 8 bytes of APP1 header must fit into the 16 bit segment length
constexpr std::size_t kSameFileCheckBytes = 256;

/////////////////////////////////////////////////////////////////////////////////////////////
// Byte order independent helpers (PNG is big endian, no unaligned reads, no MSVC intrinsics)
/////////////////////////////////////////////////////////////////////////////////////////////

constexpr std::uint32_t ReadBE32(const unsigned char* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

// Calls visit(type, length, payload) for every complete chunk beginning at 'offset', until visit returns true.
// Stops at the first chunk that is damaged or reaches over the end of the buffer.
template <typename Visitor>
void ForEachChunk(const unsigned char* p, std::size_t size, std::size_t offset, Visitor&& visit) {
    while (offset <= size && size - offset >= 8) {
        const std::uint32_t length = ReadBE32(p + offset);
        if (length > kMaxChunkSize || size - offset - 8 < length) {
            return;
        }
        if (visit(p + offset + 4, length, p + offset + 8)) {
            return;
        }
        // 12 = 4 bytes chunk size + 4 bytes chunk name + 4 bytes CRC32
        const std::size_t advance = std::size_t(length) + 12;
        if (size - offset < advance) {
            return;
        }
        offset += advance;
    }
}

// Builds a JPEG APP1 segment (FF E1, big endian length, "Exif\0\0", TIFF data) from the payload of an eXIf chunk
void* BuildExifSegment(const unsigned char* exif, std::uint32_t exifSize) {
    constexpr std::size_t kHeaderSize = 10;
    auto* segment = static_cast<unsigned char*>(std::malloc(kHeaderSize + exifSize));
    if (segment == nullptr) {
        return nullptr;
    }
    const unsigned segmentLength = exifSize + 8; // length field (2) + "Exif\0\0" (6) + data
    segment[0] = 0xFF;
    segment[1] = 0xE1;
    segment[2] = static_cast<unsigned char>(segmentLength >> 8);
    segment[3] = static_cast<unsigned char>(segmentLength & 0xFF);
    std::memcpy(segment + 4, "Exif\0\0", 6);
    std::memcpy(segment + kHeaderSize, exif, exifSize);
    return segment;
}

// Searches the chunks starting at 'offset' for an eXIf chunk
void* FindExifSegment(const unsigned char* p, std::size_t size, std::size_t offset) {
    void* segment = nullptr;
    ForEachChunk(p, size, offset, [&](const unsigned char* type, std::uint32_t length, const unsigned char* payload) {
        if (std::memcmp(type, "eXIf", 4) != 0) {
            return false;
        }
        if (length > 8 && length < kMaxExifSize) {
            segment = BuildExifSegment(payload, length);
        }
        return true; // there is only one eXIf chunk
    });
    return segment;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Decoder state
/////////////////////////////////////////////////////////////////////////////////////////////

struct MemorySource {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
};

// Values of the fcTL chunk of the current frame
struct FrameHeader {
    png_uint_32 w0 = 0;
    png_uint_32 h0 = 0;
    png_uint_32 x0 = 0;
    png_uint_32 y0 = 0;
    png_uint_16 delayNum = 1;
    png_uint_16 delayDen = 10;
    png_byte dop = 0;
    png_byte bop = 0;
};

struct Decoder {
    png_structp png = nullptr;
    png_infop info = nullptr;
    MemorySource source;
    void* transform = nullptr; // ICC transform or nullptr

    unsigned width = 0;
    unsigned height = 0;
    unsigned channels = 0;
    std::size_t imageBytes = 0;      // width * height * 4

    bool apng = false;               // file has an acTL chunk (frames are composed on 'canvas')
    unsigned frameIndex = 0;
    unsigned frameCount = 0;
    unsigned firstFrame = 0;         // 1 if the default image is not part of the animation
    FrameHeader frameHeader;

    std::unique_ptr<png_bytep[]> rowsFrame;       // row pointers handed to libpng
    // only for APNG:
    std::unique_ptr<unsigned char[]> canvas;      // composed image
    std::unique_ptr<unsigned char[]> frame;       // the frame as decoded
    std::unique_ptr<unsigned char[]> savedCanvas; // for APNG_fcTL_DISPOSE_OP_PREVIOUS
};

struct PngCache {
    std::unique_ptr<unsigned char[]> data; // the file without the PNG signature, libpng reads from here
    std::size_t dataSize = 0;
    Decoder decoder;
};

PngCache g_cache;

void FreeDecoder(Decoder& d) noexcept {
    if (d.transform != nullptr) {
        ICCProfileTransform::DeleteTransform(d.transform);
        d.transform = nullptr;
    }
    if (d.png != nullptr) {
        png_destroy_read_struct(&d.png, &d.info, nullptr);
    }
    d = Decoder{};
}

/////////////////////////////////////////////////////////////////////////////////////////////
// libpng callbacks and the functions that use setjmp.
//
// libpng reports errors with longjmp() to the setjmp() that is active for the png_ptr. The jump buffer lives in the
// frame of the function that called setjmp, so every function that calls libpng needs its own setjmp and the
// functions must not own objects with destructors (that is why they only take raw pointers).
/////////////////////////////////////////////////////////////////////////////////////////////

void ReadFromMemory(png_structp png, png_bytep out, std::size_t length) {
    auto* source = static_cast<MemorySource*>(png_get_io_ptr(png));
    if (source == nullptr) {
        png_error(png, "png_get_io_ptr returned NULL");
    }
    if (length > source->size - source->offset) {
        png_error(png, "Attempted to read out of bounds");
    }
    std::memcpy(out, source->data + source->offset, length);
    source->offset += length;
}

// libpng would print to stderr by default
void IgnoreWarning(png_structp, png_const_charp) {
}

void RaiseError(png_structp png, png_const_charp) {
    png_longjmp(png, 1); // back to the setjmp() of the active function
}

bool ReadInfo(png_structp png, png_infop info, MemorySource* source) {
    if (setjmp(png_jmpbuf(png))) {
        return false;
    }
    png_set_sig_bytes(png, kSignatureSize); // the signature is not in our copy of the file, it was checked before
    png_set_read_fn(png, source, ReadFromMemory);
    png_read_info(png, info);
    return true;
}

// Output is always 8 bit BGRA
bool SetupTransforms(png_structp png, png_infop info) {
    if (setjmp(png_jmpbuf(png))) {
        return false;
    }
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    png_set_bgr(png);
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);
    return true;
}

// Reads the next frame into 'rows'. For an APNG the frame header is read as well.
bool DecodeFrame(png_structp png, png_infop info, png_bytepp rows, bool apng, FrameHeader* header) {
    if (setjmp(png_jmpbuf(png))) {
        return false;
    }
#ifdef PNG_APNG_SUPPORTED
    if (apng) {
        png_read_frame_head(png, info);
        png_get_next_frame_fcTL(png, info, &header->w0, &header->h0, &header->x0, &header->y0,
                                &header->delayNum, &header->delayDen, &header->dop, &header->bop);
    }
#else
    (void)info;
    (void)apng;
    (void)header;
#endif
    png_read_image(png, rows);
    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// APNG composition
/////////////////////////////////////////////////////////////////////////////////////////////

#ifdef PNG_APNG_SUPPORTED
void BlendOver(unsigned char* canvas, const unsigned char* frame, std::size_t stride, const FrameHeader& h) {
    for (png_uint_32 j = 0; j < h.h0; j++) {
        const unsigned char* sp = frame + std::size_t(j) * stride;
        unsigned char* dp = canvas + (std::size_t(j) + h.y0) * stride + std::size_t(h.x0) * 4;

        for (png_uint_32 i = 0; i < h.w0; i++, sp += 4, dp += 4) {
            if (sp[3] == 255) {
                std::memcpy(dp, sp, 4);
            } else if (sp[3] != 0) {
                if (dp[3] != 0) {
                    const int u = sp[3] * 255;
                    const int v = (255 - sp[3]) * dp[3];
                    const int al = u + v;
                    dp[0] = static_cast<unsigned char>((sp[0] * u + dp[0] * v) / al);
                    dp[1] = static_cast<unsigned char>((sp[1] * u + dp[1] * v) / al);
                    dp[2] = static_cast<unsigned char>((sp[2] * u + dp[2] * v) / al);
                    dp[3] = static_cast<unsigned char>(al / 255);
                } else {
                    std::memcpy(dp, sp, 4);
                }
            }
        }
    }
}
#endif

// Decodes the next frame of an APNG and draws it onto the canvas. Call DisposeFrame() afterwards.
bool ComposeNextFrame(Decoder& d) {
#ifdef PNG_APNG_SUPPORTED
    FrameHeader header = d.frameHeader;
    if (!DecodeFrame(d.png, d.info, d.rowsFrame.get(), true, &header)) {
        return false;
    }
    if (header.x0 > d.width || header.w0 > d.width - header.x0 || header.y0 > d.height || header.h0 > d.height - header.y0) {
        return false; // frame outside of the canvas
    }
    if (d.frameIndex == d.firstFrame) {
        header.bop = PNG_fcTL_BLEND_OP_SOURCE;
        if (header.dop == PNG_fcTL_DISPOSE_OP_PREVIOUS) {
            header.dop = PNG_fcTL_DISPOSE_OP_BACKGROUND;
        }
    }
    d.frameHeader = header;

    const std::size_t stride = std::size_t(d.width) * 4;
    if (header.dop == PNG_fcTL_DISPOSE_OP_PREVIOUS) {
        std::memcpy(d.savedCanvas.get(), d.canvas.get(), d.imageBytes);
    }
    if (header.bop == PNG_fcTL_BLEND_OP_OVER) {
        BlendOver(d.canvas.get(), d.frame.get(), stride, header);
    } else {
        for (png_uint_32 j = 0; j < header.h0; j++) {
            std::memcpy(d.canvas.get() + (std::size_t(j) + header.y0) * stride + std::size_t(header.x0) * 4,
                        d.frame.get() + std::size_t(j) * stride, std::size_t(header.w0) * 4);
        }
    }
    return true;
#else
    (void)d;
    return false;
#endif
}

// Applies the dispose operation of the frame that was just shown and advances to the next frame
void DisposeFrame(Decoder& d) {
#ifdef PNG_APNG_SUPPORTED
    const FrameHeader& header = d.frameHeader;
    const std::size_t stride = std::size_t(d.width) * 4;
    if (header.dop == PNG_fcTL_DISPOSE_OP_PREVIOUS) {
        std::memcpy(d.canvas.get(), d.savedCanvas.get(), d.imageBytes);
    } else if (header.dop == PNG_fcTL_DISPOSE_OP_BACKGROUND) {
        for (png_uint_32 j = 0; j < header.h0; j++) {
            std::memset(d.canvas.get() + (std::size_t(j) + header.y0) * stride + std::size_t(header.x0) * 4, 0, std::size_t(header.w0) * 4);
        }
    }
#endif
    d.frameIndex = (d.frameIndex + 1) % d.frameCount;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Start reading
/////////////////////////////////////////////////////////////////////////////////////////////

// Returns false if the file can not be read. 'outOfMemory' is set if it failed because of the image size / memory.
// The caller releases everything on failure.
bool BeginReading(PngCache& c, bool& outOfMemory) {
    Decoder& d = c.decoder;

    d.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (d.png == nullptr) {
        outOfMemory = true;
        return false;
    }
    png_set_error_fn(d.png, nullptr, RaiseError, IgnoreWarning);
    d.info = png_create_info_struct(d.png);
    if (d.info == nullptr) {
        outOfMemory = true;
        return false;
    }

    d.source = MemorySource{ c.data.get(), c.dataSize, 0 };
    if (!ReadInfo(d.png, d.info, &d.source)) {
        return false;
    }

    const unsigned width = png_get_image_width(d.png, d.info);
    const unsigned height = png_get_image_height(d.png, d.info);
    if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
        return false;
    }
    if (static_cast<double>(width) * height > MAX_IMAGE_PIXELS) {
        outOfMemory = true;
        return false;
    }

    if (!SetupTransforms(d.png, d.info)) {
        return false;
    }
    const unsigned channels = png_get_channels(d.png, d.info);
    const std::size_t rowBytes = png_get_rowbytes(d.png, d.info);
    if (channels != 4 || rowBytes != std::size_t(width) * 4) {
        return false;
    }
    if (height > std::numeric_limits<std::size_t>::max() / rowBytes) {
        outOfMemory = true;
        return false;
    }
    d.width = width;
    d.height = height;
    d.channels = channels;
    d.imageBytes = rowBytes * height;

    png_uint_32 frames = 1;
#ifdef PNG_APNG_SUPPORTED
    if (png_get_valid(d.png, d.info, PNG_INFO_acTL)) {
        png_uint_32 plays = 0;
        png_get_acTL(d.png, d.info, &frames, &plays);
        d.apng = true;
        d.firstFrame = (png_get_first_frame_is_hidden(d.png, d.info) != 0) ? 1 : 0;
    }
#endif
    if (frames == 0) {
        return false;
    }
    d.frameCount = frames;
    d.frameHeader.w0 = width;
    d.frameHeader.h0 = height;

    d.rowsFrame.reset(new (std::nothrow) png_bytep[height]);
    if (!d.rowsFrame) {
        outOfMemory = true;
        return false;
    }
    if (d.apng) {
        // The canvas starts transparent. Only an animation needs the extra buffers, a plain PNG is decoded directly into the result.
        d.canvas.reset(new (std::nothrow) unsigned char[d.imageBytes]());
        d.frame.reset(new (std::nothrow) unsigned char[d.imageBytes]);
        d.savedCanvas.reset(new (std::nothrow) unsigned char[d.imageBytes]);
        if (!d.canvas || !d.frame || !d.savedCanvas) {
            outOfMemory = true;
            return false;
        }
        for (unsigned j = 0; j < height; j++) {
            d.rowsFrame[j] = d.frame.get() + std::size_t(j) * rowBytes;
        }
    }

    // ICC profile (libpng converts to BGRA with png_set_bgr)
    png_charp name = nullptr;
    int compressionType = 0;
    png_bytep profile = nullptr;
    png_uint_32 profileLength = 0;
    if (png_get_iCCP(d.png, d.info, &name, &compressionType, &profile, &profileLength) == PNG_INFO_iCCP) {
        d.transform = ICCProfileTransform::CreateTransform(profile, profileLength, ICCProfileTransform::FORMAT_BGRA);
    }
    return true;
}

// Copy of the canvas as the result for the caller, with ICC transform
void* CopyCanvas(const Decoder& d, bool& outOfMemory) {
    auto* pixels = new (std::nothrow) unsigned char[d.imageBytes];
    if (pixels == nullptr) {
        outOfMemory = true;
        return nullptr;
    }
    std::memcpy(pixels, d.canvas.get(), d.imageBytes);
    if (d.transform != nullptr) {
        ICCProfileTransform::DoTransform(d.transform, pixels, pixels, d.width, d.height);
    }
    return pixels;
}

// Plain PNG: decoded directly into the result
void* DecodeSingleImage(Decoder& d, bool& outOfMemory) {
    auto* pixels = new (std::nothrow) unsigned char[d.imageBytes];
    if (pixels == nullptr) {
        outOfMemory = true;
        return nullptr;
    }
    const std::size_t stride = std::size_t(d.width) * 4;
    for (unsigned j = 0; j < d.height; j++) {
        d.rowsFrame[j] = pixels + std::size_t(j) * stride;
    }
    if (!DecodeFrame(d.png, d.info, d.rowsFrame.get(), false, nullptr)) {
        delete[] pixels;
        return nullptr;
    }
    if (d.transform != nullptr) {
        ICCProfileTransform::DoTransform(d.transform, pixels, pixels, d.width, d.height);
    }
    return pixels;
}

// Is 'src' the file that is cached (compares the size and the beginning, which contains IHDR/acTL)?
bool IsCachedFile(const PngCache& c, const unsigned char* src, std::size_t size) {
    return c.dataSize == size - kSignatureSize
           && std::memcmp(c.data.get(), src + kSignatureSize, std::min(c.dataSize, kSameFileCheckBytes)) == 0;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////////////////////
// PngReader
/////////////////////////////////////////////////////////////////////////////////////////////

void* PngReader::ReadImage(int& width,
                           int& height,
                           int& nchannels,
                           bool& has_animation,
                           int& frame_count,
                           int& frame_time,
                           void*& exif_chunk,
                           bool& outOfMemory,
                           const void* buffer,
                           std::size_t size)
{
    exif_chunk = nullptr;
    const auto* src = static_cast<const unsigned char*>(buffer);
    const bool haveBuffer = (src != nullptr && size > 0);

    PngCache& c = g_cache;
    // With a cached animation the caller does not have to supply the file again
    if (c.data && haveBuffer && (size < kSignatureSize || !IsCachedFile(c, src, size))) {
        DeleteCache(); // a different file than the cached animation
    }
    if (!c.data) {
        if (!haveBuffer || size < kSignatureSize) {
            return nullptr;
        }
        c.data.reset(new (std::nothrow) unsigned char[size - kSignatureSize]);
        if (!c.data) {
            outOfMemory = true;
            return nullptr;
        }
        // copy everything except the PNG signature (first 8 bytes)
        std::memcpy(c.data.get(), src + kSignatureSize, size - kSignatureSize);
        c.dataSize = size - kSignatureSize;
    }

    Decoder& d = c.decoder;
    if (d.png == nullptr || d.frameIndex == 0) {
        // (re)start: libpng can only read forward
        FreeDecoder(d);
        if (!BeginReading(c, outOfMemory)) {
            DeleteCache();
            return nullptr;
        }
    }

    void* pixels = nullptr;
    if (d.apng) {
        bool ok = true;
        if (d.frameIndex < d.firstFrame) {
            // the default image is not part of the animation: decode it (it may be the base of the next frame), but do not return it
            ok = ComposeNextFrame(d);
            if (ok) {
                DisposeFrame(d);
            }
        }
        ok = ok && ComposeNextFrame(d);
        if (ok) {
            pixels = CopyCanvas(d, outOfMemory);
            DisposeFrame(d);
        }
    } else {
        pixels = DecodeSingleImage(d, outOfMemory);
    }
    if (pixels == nullptr) {
        DeleteCache();
        return nullptr;
    }

    width = static_cast<int>(d.width);
    height = static_cast<int>(d.height);
    nchannels = static_cast<int>(d.channels);
    has_animation = (d.frameCount > 1);
    frame_count = static_cast<int>(d.frameCount);

    // https://wiki.mozilla.org/APNG_Specification
    // "If the denominator is 0, it is to be treated as if it were 100"
    const unsigned delayDen = d.frameHeader.delayDen != 0 ? d.frameHeader.delayDen : 100;
    frame_time = static_cast<int>(1000.0 * d.frameHeader.delayNum / delayDen);

    exif_chunk = FindExifSegment(c.data.get(), c.dataSize, 0);

    if (!has_animation) {
        DeleteCache();
    }
    return pixels;
}

void PngReader::DeleteCache()
{
    FreeDecoder(g_cache.decoder);
    g_cache.data.reset();
    g_cache.dataSize = 0;
}

bool PngReader::MustUseLibpng(const void* buffer, std::size_t size)
{
    const auto* p = static_cast<const unsigned char*>(buffer);

    // IHDR: signature (8) + length (4) + "IHDR" (4), then width and height
    if (p == nullptr || size < 24) {
        return false;
    }
    // QImage can not hold more than INT_MAX bytes
    const double width = ReadBE32(p + 16);
    const double height = ReadBE32(p + 20);
    if (4.0 * width * height > std::numeric_limits<int>::max()) {
        return true;
    }

    // QImageReader does not support APNG
    // Valid APNGs must have an acTL chunk before the first IDAT chunk, this lets us quickly determine if a PNG is animated
    bool animated = false;
    ForEachChunk(p, size, kSignatureSize, [&](const unsigned char* type, std::uint32_t, const unsigned char*) {
        if (std::memcmp(type, "acTL", 4) == 0) {
            animated = true;
            return true;
        }
        return std::memcmp(type, "IDAT", 4) == 0;
    });
    return animated;
}

void* PngReader::GetEXIFBlock(const void* buffer, std::size_t size)
{
    const auto* p = static_cast<const unsigned char*>(buffer);
    if (p == nullptr) {
        return nullptr;
    }
    return FindExifSegment(p, size, kSignatureSize);
}

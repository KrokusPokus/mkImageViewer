#include "wrapperHEIF.h"

#include "libheif/heif_cxx.h"
#include "ICCProfileTransform.h"
#include "MaxImageDef.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

// Creates the transform of the decoded RGBA pixels to sRGB, or nullptr if there is nothing to do.
// An ICC profile takes precedence; without one the nclx profile (CICP code points) describes the color space.
// The nclx profile is looked for in the colr box of the image first and then in the decoded image, which also
// carries the color information of the bitstream (VUI).
void* CreateColorTransform(const heif::ImageHandle& handle, const heif::Image& image) {
    const std::vector<uint8_t> iccp = image.get_raw_color_profile();
    if (!iccp.empty()) {
        return ICCProfileTransform::CreateTransform(iccp.data(), static_cast<unsigned int>(iccp.size()), ICCProfileTransform::FORMAT_RGBA);
    }

    heif_color_profile_nclx* nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle.get_raw_image_handle(), &nclx).code == heif_error_Ok && nclx != nullptr) {
        void* transform = ICCProfileTransform::CreateTransformFromCICP(nclx->color_primaries, nclx->transfer_characteristics,
                                                                       ICCProfileTransform::FORMAT_RGBA);
        heif_nclx_color_profile_free(nclx);
        return transform;
    }
    if (image.get_color_profile_type() == heif_color_profile_type_nclx) {
        const heif::ColorProfile_nclx imageNclx = image.get_nclx_color_profile(); // does not throw: the type is nclx
        return ICCProfileTransform::CreateTransformFromCICP(imageNclx.get_color_primaries(), imageNclx.get_transfer_characteristics(),
                                                            ICCProfileTransform::FORMAT_RGBA);
    }
    return nullptr;
}

// Returns the first Exif block of the image as a JPEG APP1 segment (malloc()) or nullptr.
// The Exif item of HEIF/AVIF (ISO/IEC 23008-12, A.2.1) starts with a 4 byte big endian offset to the TIFF header,
// counted from the end of that field. Usually it is 6 and skips "Exif\0\0", but 0 (TIFF header right away) is valid
// as well. The JPEG APP1 segment is: FF E1, big endian length, "Exif\0\0", TIFF data.
void* CopyExifBlock(const heif::ImageHandle& handle) {
    const std::vector<heif_item_id> exif_blocks = handle.get_list_of_metadata_block_IDs("Exif");
    if (exif_blocks.empty()) {
        return nullptr;
    }
    const std::vector<uint8_t> exif = handle.get_metadata(exif_blocks[0]);
    if (exif.size() < 4) {
        return nullptr;
    }
    const size_t nTiffOffset = 4 + ((size_t)exif[0] << 24 | (size_t)exif[1] << 16 | (size_t)exif[2] << 8 | (size_t)exif[3]);
    if (nTiffOffset >= exif.size()) {
        return nullptr;
    }
    const uint8_t* pTiff = exif.data() + nTiffOffset;
    const size_t nTiffSize = exif.size() - nTiffOffset;
    const size_t nSegmentSize = 10 + nTiffSize;      // FF E1 + length + "Exif\0\0" + TIFF
    if (nTiffSize <= 8 || nSegmentSize - 2 > 0xFFFF) { // the length field counts everything after FF E1
        return nullptr;
    }
    auto* segment = static_cast<uint8_t*>(malloc(nSegmentSize));
    if (segment == nullptr) {
        return nullptr;
    }
    segment[0] = 0xFF;
    segment[1] = 0xE1;
    segment[2] = static_cast<uint8_t>((nSegmentSize - 2) >> 8);
    segment[3] = static_cast<uint8_t>((nSegmentSize - 2) & 0xFF);
    memcpy(segment + 4, "Exif\0\0", 6);
    memcpy(segment + 10, pTiff, nTiffSize);
    return segment;
}

} // namespace

void * HeifReader::ReadImage(int &width,
                       int &height,
                       int &nchannels,
                       int &frame_count,
                       void* &exif_chunk,
                       bool &outOfMemory,
                       int frame_index,
                       const void *buffer,
                       std::size_t sizebytes)
{
    outOfMemory = false;
    width = height = 0;
    nchannels = 4;

    unsigned char* pPixelData = nullptr;
    exif_chunk = nullptr;

    heif::Context context;
    context.read_from_memory_without_copy(buffer, sizebytes);
    frame_count = context.get_number_of_top_level_images();
    heif_item_id item_id = context.get_list_of_top_level_image_IDs().at(frame_index);
    heif::ImageHandle handle = context.get_image_handle(item_id);
    // height = handle.get_height();
    // width = handle.get_width();
    heif::Image image = handle.decode_image(heif_colorspace_RGB, heif_chroma_interleaved_RGBA);
    int stride;
    uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
    width = image.get_width(heif_channel_interleaved);
    height = image.get_height(heif_channel_interleaved);

    // get_width()/get_height() return -1 on error
    if (width < 1 || height < 1 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
        return nullptr;
    if ((double)width * height > MAX_IMAGE_PIXELS) {
        outOfMemory = true;
        return nullptr;
    }
    if (width * nchannels > stride)
        return nullptr;

    // width, height >= 1 (checked above)
    const size_t nRows = height;
    const size_t nCols = width;
    const size_t size = nRows * nCols * nchannels;
    pPixelData = new(std::nothrow) unsigned char[size];
    if (pPixelData == nullptr) {
        outOfMemory = true;
        return nullptr;
    }
    void* transform = CreateColorTransform(handle, image);
    if (!ICCProfileTransform::DoTransform(transform, data, pPixelData, width, height, stride)) {
        unsigned int* o = (unsigned int*)pPixelData;
        for (size_t i = 0; i < nRows; i++) {
            unsigned int* p = (unsigned int*)(data + i * stride);
            for (size_t j = 0; j < nCols; j++) {
                // RGBA -> BGRA conversion
                *o = std::rotr(__builtin_bswap32(*p), 8);
                p++;
                o++;
            }
        }
    }
    ICCProfileTransform::DeleteTransform(transform);

    exif_chunk = CopyExifBlock(handle);

    return (void*)pPixelData;
}

void* HeifReader::GetEXIFBlock(const void* buffer, std::size_t sizebytes) {
    if (buffer == nullptr || sizebytes == 0) {
        return nullptr;
    }
    try {
        // Reading the context only parses the container, nothing is decoded
        heif::Context context;
        context.read_from_memory_without_copy(buffer, sizebytes);
        return CopyExifBlock(context.get_primary_image_handle());
    } catch (const heif::Error&) {
        return nullptr;
    }
}

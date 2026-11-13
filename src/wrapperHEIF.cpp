#include "wrapperHEIF.h"

#include "libheif/heif_cxx.h"
#include "ICCProfileTransform.h"
#include "MaxImageDef.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

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

    if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
        return nullptr;
    if (std::abs((double)width * height) > MAX_IMAGE_PIXELS) {
        outOfMemory = true;
        return nullptr;
    }
    if (width < 1 || height < 1 || width * nchannels > stride)
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
    std::vector<uint8_t> iccp = image.get_raw_color_profile();
    void* transform = ICCProfileTransform::CreateTransform(iccp.data(), iccp.size(), ICCProfileTransform::FORMAT_RGBA);
    if (!ICCProfileTransform::DoTransform(transform, data, pPixelData, width, height, stride=stride)) {
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

    std::vector<heif_item_id> exif_blocks = handle.get_list_of_metadata_block_IDs("Exif");

    if (!exif_blocks.empty()) {
        std::vector<uint8_t> exif = handle.get_metadata(exif_blocks[0]);
        // 65538 magic number comes from investigations by qbnu
        // see https://github.com/sylikc/jpegview/pull/213#pullrequestreview-1494451359 for more details
        /*
        * These libraries all have their own ideas about where to start the Exif data from.
        * JPEG Exif blocks are in the format
          FF E1 SS SS 45 78 69 66 00 00 [data]

          The SS SS is a big-endian unsigned short representing the size of everything after FF E1.

          libjxl gives 00 00 00 00 [data]
          libheif gives 00 00 00 00 45 78 69 66 00 00 [data]
          libavif, libwebp and libpng give [data], so they have different limits for size.

          If you want I can change it to 65536 + an offset and add notes explaining why.
        */
        if (exif.size() > 8 && exif.size() < 65538) {
            exif_chunk = malloc(exif.size());
            if (exif_chunk != nullptr) {
                memcpy(exif_chunk, exif.data(), exif.size());
                *((unsigned short*)exif_chunk) = __builtin_bswap16(0xFFE1);
                *((unsigned short*)exif_chunk + 1) = __builtin_bswap16(exif.size() - 2);
            }
        }
    }

    return (void*)pPixelData;
}

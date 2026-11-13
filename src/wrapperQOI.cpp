#include "wrapperQOI.h"

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi/qoi.h"
#include "MaxImageDef.h"
#include "Helpers.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <new>

#define LINEAR_TO_SRGB(rgb) ((rgb) > 0 ? ((rgb) < 255 ? (255.0 * (1.055 * pow((rgb)/255.0, 1.0/2.4) - 0.055)) : 255) : 0)

void* QoiReaderWriter::ReadImage(int& width,
    int& height,
    int& nchannels,
    bool& outOfMemory,
    const void* buffer,
    std::size_t sizebytes)
{
    outOfMemory = false;

    // qoi_decode() takes the size as int
    if (sizebytes > static_cast<std::size_t>(INT_MAX))
        return nullptr;

    qoi_desc desc = { 0 };
    unsigned char* pDecodedPixels = (unsigned char*)qoi_decode(buffer, static_cast<int>(sizebytes), &desc, 0);
    if (pDecodedPixels == nullptr)
        return nullptr;
    width = desc.width;
    height = desc.height;
    nchannels = desc.channels;
    if (std::abs((double)width * height) > MAX_IMAGE_PIXELS)
        outOfMemory = true;
    if (outOfMemory || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
        QOI_FREE(pDecodedPixels);
        return nullptr;
    }
    // one line fits into an int (width <= MAX_IMAGE_DIMENSION), the whole image may not
    const std::size_t decoded_stride = static_cast<std::size_t>(width) * nchannels;
    const std::size_t padded_stride = Helpers::DoPadding(width * nchannels, 4);
    const std::size_t size = decoded_stride * height;
    unsigned char* pPixelData = new(std::nothrow) unsigned char[padded_stride * height];
    if (pPixelData != nullptr) {
        // Copy from RGB(A) to BGR(A)
        for (std::size_t i = 0; i < size; i += nchannels) {
            const std::size_t j = i / decoded_stride * padded_stride + i % decoded_stride;
            if (desc.colorspace == QOI_LINEAR) {
                pPixelData[j  ] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i+2]); // NOLINT(clang-analyzer-security.ArrayBound): i+2 < size == Puffergroesse
                pPixelData[j+1] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i+1]);
                pPixelData[j+2] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i  ]);
            } else {
                pPixelData[j  ] = pDecodedPixels[i+2]; // NOLINT(clang-analyzer-security.ArrayBound): i+2 < size == Puffergroesse
                pPixelData[j+1] = pDecodedPixels[i+1];
                pPixelData[j+2] = pDecodedPixels[i  ];
            }
            if (nchannels == 4)
                pPixelData[j+3] = pDecodedPixels[i+3];
        }
    } else {
        outOfMemory = true;
    }
    QOI_FREE(pDecodedPixels);
    return (void*)pPixelData;
}

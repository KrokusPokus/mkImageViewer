#ifndef IMAGEPROCESSINGTYPES_H
#define IMAGEPROCESSINGTYPES_H

enum EFilterType {
    Filter_Downsampling_None,
    Filter_Downsampling_Hermite,
    Filter_Downsampling_Mitchell,
    Filter_Downsampling_Catrom,
    Filter_Downsampling_Lanczos2,
    Filter_Upsampling_Bicubic
};

enum EImageFormat {
    IF_JPEG,
    IF_WindowsBMP,
    IF_PNG,
    IF_GIF,
    IF_TIFF,
    IF_WEBP,
    IF_JXL,
    IF_HEIF,
    IF_AVIF,
    IF_QOI,
    IF_PSD,
    IF_ICO,
    IF_JPEG_Embedded, // JPEG embedded in another file, e.g. camera raw
    IF_TGA,
    IF_ZIP,
    IF_Unknown
};

#endif // IMAGEPROCESSINGTYPES_H

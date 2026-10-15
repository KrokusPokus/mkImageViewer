#ifndef IMAGEPROCESSINGTYPES_H
#define IMAGEPROCESSINGTYPES_H

#include <string>

/// signed 8 bit integer value
typedef signed char int8;
/// unsigned 8 bit integer value
typedef unsigned char uint8;
/// signed 16 bit integer value
typedef signed short int16;
/// unsigned 16 bit integer value
typedef unsigned short uint16;
/// signed 32 bit integer value
typedef signed int int32;
/// unsigned 32 bit integer value
typedef unsigned int uint32;

enum EFilterType {
    Filter_Downsampling_None,
    Filter_Downsampling_Hermite,
    Filter_Downsampling_Mitchell,
    Filter_Downsampling_Catrom,
    Filter_Downsampling_Lanczos2,
    Filter_Downsampling_Best_Quality,  // prefer this filter for sampling down
    Filter_Downsampling_No_Aliasing, // this is a Lanczos type filter
    Filter_Downsampling_Narrow, // as Filter_Downsampling_Best_Quality but narrow support
    Filter_Upsampling_Bicubic
};

enum EResizeFilter {
    Resize_PointFilter,  // Point sampling
    Resize_NoAliasing,   // Lanczos filter
    Resize_SharpenLow,   // 3 tap downsampling filter with some sharpening
    Resize_SharpenMedium // 3 tap downsampling filter with medium sharpening
};

// Image formats (can be other than JPEG...)
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

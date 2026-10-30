#ifndef HELPERS_H
#define HELPERS_H

#include "ImageLoadResult.h"
#include "ImageProcessingTypes.h" // defines int8, uint8, int16, uint16, int32, uint32, EFilterType, EImageFormat
#include "WTLCompat.h"

#include <cfloat>

#include <QString>
#include <QRgb>

class CBaseImage;
class CFileList;

namespace Helpers {

    // Capabilities of CPU, higher capabilities include the others
    enum CPUType {
        CPU_Unknown,
        CPU_Generic,
        CPU_MMX,
        CPU_SSE,
        CPU_AVX2
        // add higher capabilities at the end!
    };

    // Sorting order of image files
    enum ESorting {
        FS_Undefined = -1,
        FS_FileName,
        FS_LastModTime,
        FS_FileSize,
        FS_Random
    };

    // Navigation mode over directories
    enum ENavigationMode {
        NM_Undefined = -1,
        NM_LoopDirectory,
        NM_LoopSubDirectories,
        NM_LoopSameDirectoryLevel
    };

    // Single instance modes
    enum ESingleInstanceMode {
        SI_Never,
        SI_PerFolder,
        SI_Always
    };
    
    // Auto zoom modes
    enum EAutoZoomMode {
        ZM_None,
        ZM_FitToScreenNoZoom,
        ZM_FillScreenNoZoom,
        ZM_FitToScreen,
        ZM_FillScreen,
        ZM_BookMode
    };

    // Transparency modes
    enum ETransparencyMode {
        TP_BLEND = 0,
        TP_CHECKERBOARD,
        TP_BLEND_INVERSE
    };

    // Transition effects for full screen slideshow
    enum ETransitionEffect {
        TE_None,
        TE_Blend,
        TE_SlideRL,
        TE_SlideLR,
        TE_SlideTB,
        TE_SlideBT,
        TE_RollRL,
        TE_RollLR,
        TE_RollTB,
        TE_RollBT,
        TE_ScrollRL,
        TE_ScrollLR,
        TE_ScrollTB,
        TE_ScrollBT,
    };

    inline float DPIScalingFactor = -1.0f; // is updated to ::GetDeviceCaps(dc, LOGPIXELSX)/96.0f during OnInitDialog()

    // Maximum and minimum allowed zoom factors for images
    const double ZoomMax = DBL_MAX; // unbound the maximum zoom, previously set at 16.0 (1600%)
    const double ZoomMin = DBL_MIN; // unbound the minimum zoom, previously set at 0.1 (10%)

    // this is specified all over the code as the maximum image dimension that one side can be
    // this particular variable will have limited usage, but is used to reflect that limitation.  DO NOT CHANGE
    //const int MAX_IMAGE_DIMENSION = 65535;

    // Round to integer
    inline int RoundToInt(double d) {
        return (d < 0) ? (int)(d - 0.5) : (int)(d + 0.5);
    }

    // Scales a pixel value given in 96 DPI to screen pixels, taking the screen DPI into account
    int ScaleWithDPI(int value);

    // Memory that new allocations can get without swapping or the OOM killer: MemAvailable from /proc/meminfo
    // (free memory plus reclaimable file cache - not MemFree, which ignores the cache), in bytes.
    // Limited by the memory limit of the cgroup of the process (memory.max), if there is one.
    // A snapshot only: other processes may take the memory a moment later. Returns -1 if not available (no Linux).
    qint64 AvailableMemoryBytes();

    // Total memory the process can use: MemTotal, or the memory limit of its cgroup if that is smaller. -1 if unknown.
    // Read only once, on the first call (a cgroup limit changed while the app runs is not noticed).
    qint64 TotalMemoryBytes();

    // Read ahead (pre-caching of neighbour images) stops while less than this share of the memory is available
    constexpr int READ_AHEAD_MIN_FREE_PERCENT = 25;

    // True if at least READ_AHEAD_MIN_FREE_PERCENT of TotalMemoryBytes() is available. Also true if the memory
    // cannot be determined (then there is no limit, as before).
    bool StillEnoughFree();

    // Based on: https://github.com/sdneon/jpegview.git
    // QRgb is 0x00rrggbb. 0xFF00000 signals we don't use an override color
    void BlendAlpha(uint32* pImage32, int nWidth, int nHeight, Helpers::ETransparencyMode TransparencyMode);

    // pixel is ARGB, backgroundColor is QRgb. Returns ARGB
    // [GF] Note: PsdReader::ReadImage() still calls this directly for "blending the K channel in CMYK images".
    //        Anything else is going through BlendAlpha().
    static inline uint32 AlphaBlendBackground(uint32 pixel, QRgb backgroundColor, bool bUseCheckerboard = false, int x = -1, int y = -1) {
        uint32 alpha = pixel & 0xFF000000;
        if (alpha == 0xFF000000)
            return pixel;

        if (bUseCheckerboard) {
            //ignore configured backgroundColor, and use white and light gray for checkerboard
            if (((x & 0x10) && ((y & 0x10) == 0))
                || ((y & 0x10) && ((x & 0x10) == 0))) {
                backgroundColor = qRgb(255, 255, 255);
            } else {
                backgroundColor = qRgb(192, 192, 192);
            }
        }

        uint8 bg_r = qRed(backgroundColor);
        uint8 bg_g = qGreen(backgroundColor);
        uint8 bg_b = qBlue(backgroundColor);

        if (alpha == 0) {
            return 0xFF000000 | (bg_r << 16) | (bg_g << 8) | bg_b;
        } else {
            /*
             *          uint8 r = (pixel >> 16) & 0xFF;
             *          uint8 g = (pixel >>  8) & 0xFF;
             *          uint8 b = (pixel      ) & 0xFF;
             *          uint8 a = alpha >> 24;
             *          uint8 one_minus_a = 255 - a;
             *
             *          return
             *              0xFF000000 +
             *              (  (uint8)(((r * a + bg_r * one_minus_a) / 255.0) + 0.5) << 16) +
             *              (  (uint8)(((g * a + bg_g * one_minus_a) / 255.0) + 0.5) <<  8) +
             *              (  (uint8)(((b * a + bg_b * one_minus_a) / 255.0) + 0.5)      );
             */
            // From https://github.com/aviscaerulea/jpegview-nt.git
            // Integer division by 255: (v + 128 + ((v + 128) >> 8)) >> 8
            // This code performs about 30% faster than the original one above and gives bit-identical results.

            uint32 a = alpha >> 24;
            uint32 one_minus_a = 255 - a;
            uint32 vr = ((pixel >> 16) & 0xFF) * a + bg_r * one_minus_a;
            uint32 vg = ((pixel >>  8) & 0xFF) * a + bg_g * one_minus_a;
            uint32 vb = ((pixel      ) & 0xFF) * a + bg_b * one_minus_a;

            return
                0xFF000000 |
                (((vr + 128 + ((vr + 128) >> 8)) >> 8) << 16) |
                (((vg + 128 + ((vg + 128) >> 8)) >> 8) <<  8) |
                (((vb + 128 + ((vb + 128) >> 8)) >> 8)      );
        }
    }

    // Gets the image size to be used when fitting the image to screen, either using 'fit to screen'
    // or 'fill with crop' method. If 'fill with crop' is used, the bLimitAR can be set to avoid
    // filling when to less pixels remain visible
    // Outputs also the zoom factor to resize to this new size.
    // nWidth, nHeight are the original image width and height
    QSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, 
        bool bAllowZoomIn, bool bFillCrop, bool bLimitAR, EAutoZoomMode eAutoZoomMode, double & dZoom);

    // Gets the image size to be used when fitting the image to screen according to the auto zoom mode given
    QSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, EAutoZoomMode eAutoZoomMode, double & dZoom);

    // Pad the given value to the next multiple of padvalue. padvalue must be a power of 2.
    inline int DoPadding(int value, int padvalue) {
        return (value + padvalue - 1) & ~(padvalue - 1);
    }

    // Finds a JPEG marker (beginning with 0xFF) in a JPEG stream. 
    // To find the first non-marker block, set nMarker to zero.
    const void* FindJPEGMarker(const void* pJPEGStream, int nStreamLength, unsigned char nMarker);

    // Finds the EXIF block in the JPEG bytes stream, NULL if no EXIF block or if the block (by its length field)
    // reaches beyond the end of the stream
    void* FindEXIFBlock(void* pJPEGStream, int nStreamLength);

    // try to convert a string from UTF-8. Returns empty string if no valid UTF-8 encoded string.
    QString TryConvertFromUTF8(const uint8_t* pComment, int nLengthInBytes);

    // Gets the content of the JPEG comment tag, empty string if no comment
    QString GetJPEGComment(const void* pJPEGStream, int nStreamLength);

    // Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
    // bImageLoaded: false if the image at 'position' could not be loaded
    int GetFrameIndex(const CImagePosition& position, bool bImageLoaded, int iOffset, bool bSwitchFrame, bool & switchImage);

    uint64_t GetTimeMs();

} // namespace Helpers

#endif // HELPERS_H

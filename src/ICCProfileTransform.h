#ifndef ICCPROFILETRANSFORM_H
#define ICCPROFILETRANSFORM_H

#include <atomic>

class ICCProfileTransform
{
public:
    enum PixelFormat {
        FORMAT_BGRA,
        FORMAT_RGBA,
        FORMAT_BGR,
        FORMAT_RGB,
    };

    // Create a transform from given ICC Profile to standard sRGB color space.
    static void* CreateTransform(
        const void* profile, // pointer to ICC profile
        unsigned int size, // size of ICC profile in bytes
        PixelFormat format // format of input pixels
    );

    // Create a transform from the color space given by CICP code points (ITU-T H.273: colour primaries and
    // transfer characteristics) to standard sRGB. AVIF and HEIF signal the color space this way (nclx) when they
    // carry no ICC profile. Returns nullptr if the image already is sRGB or the combination is not supported
    // (e.g. HDR with PQ or HLG transfer).
    static void* CreateTransformFromCICP(
        unsigned int colorPrimaries, // H.273 ColourPrimaries
        unsigned int transferCharacteristics, // H.273 TransferCharacteristics
        PixelFormat format // format of input pixels
    );

    // Apply color transform to image. Returns true on success, false otherwise.
    static bool DoTransform(
        void* transform, // ICCP transform
        const void* inputBuffer, // 4-byte BGRA or RGBA input depending on transform pixel format
        void* outputBuffer, // 4-byte BGRA output
        unsigned int width, // width of image in pixels
        unsigned int height, // height of image in pixels
        unsigned int stride=0 // number of bytes per row of pixels in the input, only needed if not equal to width * 4
    );

    // Free memory associated with the given transform
    static void DeleteTransform(void* transform);

    // Runtime switch for using embedded color profiles (initialized from the INI, toggled by the user).
    // Atomic: set in the GUI thread, read in the image load threads.
    static void SetEnabled(bool bEnabled) { sEnabled.store(bEnabled, std::memory_order_relaxed); }
    static bool IsEnabled() { return sEnabled.load(std::memory_order_relaxed); }

private:
    static void* sRGBProfile;
    static std::atomic<bool> sEnabled;
};


#endif // ICCPROFILETRANSFORM_H

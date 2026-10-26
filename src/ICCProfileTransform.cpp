#include "ICCProfileTransform.h"
#include "SettingsProvider.h"

// This define is necessary for 32-bit builds to work, for some reason
#define CMS_DLL
#include "lcms2.h"
#define TYPE_LabA_8 (COLORSPACE_SH(PT_Lab)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(1))


void* ICCProfileTransform::sRGBProfile = nullptr;

// Hilfsfunktion zur Prüfung, ob ein Profil sRGB ist
static bool IsStandardsRGB(cmsHPROFILE hProfile) {
    if (hProfile == nullptr) return false;

    // 1. Prüfen über die Profil-Beschreibung (z.B. "sRGB IEC61966-2.1")
    char desc[128] = {0};
    cmsGetProfileInfoASCII(hProfile, cmsInfoDescription, "en", "US", desc, sizeof(desc));
    if (std::strstr(desc, "sRGB") != nullptr) {
        return true;
    }

    // 2. Optional: Alternativer Check über Farbraum-Signatur und Matrix-Eigenschaften
    if (cmsGetColorSpace(hProfile) == cmsSigRgbData && cmsIsMatrixShaper(hProfile)) {
        // Man könnte hier optional noch weitere Heuriken einbauen,
        // aber der Beschreibungstext deckt 99% der Standard-JPEGs ab.
    }

    return false;
}

void* ICCProfileTransform::CreateTransform(const void* profile, unsigned int size, PixelFormat format)
{
    if (profile == nullptr || size == 0) {
        return nullptr; // No ICC Profile
    }

    if (sRGBProfile == nullptr) {
        try {
            sRGBProfile = cmsCreate_sRGBProfile();
        } catch (...) {}
    }

    if (sRGBProfile == nullptr) {
        return nullptr; // lcms2.dll not found or out of memory
    }

    cmsHPROFILE hInProfile = cmsOpenProfileFromMem(profile, size);
    if (hInProfile == nullptr) {
        return nullptr; // Invalid profile
    }

    // Already sRGB?
    if (IsStandardsRGB(hInProfile)) {
        cmsCloseProfile(hInProfile);
        return nullptr;
    }

    // Create transform from embedded profile to sRGB
    cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA;
    cmsUInt32Number inFormat, outFormat;
    switch (format) {
        case FORMAT_BGRA:
            inFormat = TYPE_BGRA_8;
            outFormat = TYPE_BGRA_8;
            break;
        case FORMAT_RGBA:
            inFormat = TYPE_RGBA_8;
            outFormat = TYPE_BGRA_8;
            break;
        case FORMAT_BGR:
            inFormat = TYPE_BGR_8;
            outFormat = TYPE_BGR_8;
            break;
        case FORMAT_RGB:
            inFormat = TYPE_RGB_8;
            outFormat = TYPE_BGR_8;
            break;
        default:
            cmsCloseProfile(hInProfile);
            return nullptr;
    }
    cmsHTRANSFORM transform = cmsCreateTransform(hInProfile, inFormat, sRGBProfile, outFormat, INTENT_RELATIVE_COLORIMETRIC, flags);
    cmsCloseProfile(hInProfile);
    return transform;
}

bool ICCProfileTransform::DoTransform(void* transform, const void* inputBuffer, void* outputBuffer, unsigned int width, unsigned int height, unsigned int stride)
{
    unsigned int numPixels = width * height;
    if (transform == nullptr || inputBuffer == nullptr || outputBuffer == nullptr || numPixels == 0 || !CSettingsProvider::This().UseEmbeddedColorProfiles())
        return false;
    cmsUInt32Number inFormat = cmsGetTransformInputFormat(transform);
    int nchannels;
    switch (inFormat) {
        case TYPE_BGRA_8:
        case TYPE_RGBA_8:
        case TYPE_LabA_8:
            nchannels = 4;
            break;
        case TYPE_BGR_8:
        case TYPE_RGB_8:
        case TYPE_Lab_8:
            nchannels = 3;
            break;
        default:
            return false;
    }
    if (stride == 0)
        stride = width * nchannels;
    cmsDoTransformLineStride(transform, inputBuffer, outputBuffer, width, height, stride, Helpers::DoPadding(width * nchannels, 4), stride * height, Helpers::DoPadding(width * nchannels, 4) * height);
    return true;
}

void ICCProfileTransform::DeleteTransform(void* transform)
{
    if (transform != nullptr)
        cmsDeleteTransform(transform);
}

void* ICCProfileTransform::CreateLabTransform(PixelFormat format) {
    cmsHTRANSFORM transform = nullptr;
    cmsHPROFILE hLabProfile = nullptr;
    try {
        hLabProfile = cmsCreateLab4Profile(cmsD50_xyY());
        if (sRGBProfile == nullptr) {
            sRGBProfile = cmsCreate_sRGBProfile();
        }
    } catch (...) {}

    if (hLabProfile == nullptr || sRGBProfile == nullptr)
        return nullptr; // Could not create profile

    // Create transform from CIELAB D50 (Photoshop "Lab mode") to sRGB
    cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA;
    cmsUInt32Number inFormat, outFormat;
    switch (format) {
        case FORMAT_Lab:
            inFormat = TYPE_Lab_8;
            outFormat = TYPE_BGR_8;
            break;
        case FORMAT_LabA:
            inFormat = TYPE_LabA_8;
            outFormat = TYPE_BGRA_8;
            break;
        default:
            cmsCloseProfile(hLabProfile);
            return nullptr;
    }
    transform = cmsCreateTransform(hLabProfile, inFormat, sRGBProfile, outFormat, INTENT_RELATIVE_COLORIMETRIC, flags);
    cmsCloseProfile(hLabProfile);
    return transform;
}

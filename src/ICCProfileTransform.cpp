#include "ICCProfileTransform.h"

#include "lcms2.h"
#include "Helpers.h"

#include <cstring>

#define TYPE_LabA_8 (COLORSPACE_SH(PT_Lab)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(1))


void* ICCProfileTransform::sRGBProfile = nullptr;
std::atomic<bool> ICCProfileTransform::sEnabled{true};

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

// Creates the transform from hInProfile to sRGB. Takes ownership of hInProfile (it is closed in any case).
static void* CreateTransformToSRGB(cmsHPROFILE hInProfile, ICCProfileTransform::PixelFormat format, void*& sRGBProfile)
{
    if (hInProfile == nullptr) {
        return nullptr;
    }
    if (sRGBProfile == nullptr) {
        sRGBProfile = cmsCreate_sRGBProfile();
    }
    if (sRGBProfile == nullptr) {
        cmsCloseProfile(hInProfile);
        return nullptr; // out of memory
    }

    cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA;
    cmsUInt32Number inFormat, outFormat;
    switch (format) {
        case ICCProfileTransform::FORMAT_BGRA:
            inFormat = TYPE_BGRA_8;
            outFormat = TYPE_BGRA_8;
            break;
        case ICCProfileTransform::FORMAT_RGBA:
            inFormat = TYPE_RGBA_8;
            outFormat = TYPE_BGRA_8;
            break;
        case ICCProfileTransform::FORMAT_BGR:
            inFormat = TYPE_BGR_8;
            outFormat = TYPE_BGR_8;
            break;
        case ICCProfileTransform::FORMAT_RGB:
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

void* ICCProfileTransform::CreateTransform(const void* profile, unsigned int size, PixelFormat format)
{
    if (profile == nullptr || size == 0) {
        return nullptr; // No ICC Profile
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

    return CreateTransformToSRGB(hInProfile, format, sRGBProfile);
}

namespace {

// Chromaticities (CIE xy) of the red, green and blue primaries and of the white point
struct CICPPrimaries {
    double rx, ry, gx, gy, bx, by, wx, wy;
};

constexpr double kD65x = 0.3127, kD65y = 0.3290;
constexpr double kCx = 0.310, kCy = 0.316;

// ITU-T H.273, table 2 (ColourPrimaries). Returns false for unknown or unsupported values.
bool GetCICPPrimaries(unsigned int primaries, CICPPrimaries& p)
{
    switch (primaries) {
        case 1:  // BT.709, sRGB
        case 2:  // unspecified: the most likely is BT.709
            p = { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, kD65x, kD65y }; return true;
        case 4:  // BT.470 System M
            p = { 0.670, 0.330, 0.210, 0.710, 0.140, 0.080, kCx, kCy }; return true;
        case 5:  // BT.601 625 lines, BT.470 System B/G
            p = { 0.640, 0.330, 0.290, 0.600, 0.150, 0.060, kD65x, kD65y }; return true;
        case 6:  // BT.601 525 lines
        case 7:  // SMPTE 240M
            p = { 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, kD65x, kD65y }; return true;
        case 8:  // generic film
            p = { 0.681, 0.319, 0.243, 0.692, 0.145, 0.049, kCx, kCy }; return true;
        case 9:  // BT.2020, BT.2100
            p = { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, kD65x, kD65y }; return true;
        case 11: // SMPTE RP 431-2 (DCI-P3, DCI white point)
            p = { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351 }; return true;
        case 12: // SMPTE EG 432-1 (Display P3)
            p = { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, kD65x, kD65y }; return true;
        case 22: // EBU Tech. 3213-E
            p = { 0.630, 0.340, 0.295, 0.605, 0.155, 0.077, kD65x, kD65y }; return true;
        default: // 0, 3 (reserved), 10 (CIE XYZ) and everything else
            return false;
    }
}

// ITU-T H.273, table 3 (TransferCharacteristics): the curve that maps the encoded values to linear light.
// Returns nullptr for unknown or unsupported values (HDR: PQ and HLG would need tone mapping).
cmsToneCurve* CreateCICPToneCurve(unsigned int transfer)
{
    switch (transfer) {
        case 13: // sRGB (IEC 61966-2-1)
        case 2:  // unspecified: treated as sRGB, like all images without color information
        case 1:  // BT.709
        case 6:  // BT.601
        case 14: // BT.2020 10 bit
        case 15: // BT.2020 12 bit
        {
            // The video curves (BT.709/601/2020) are camera curves (OETF) and only define the display side
            // indirectly. Like Qt and the browsers they are shown with the sRGB curve: the exact inverse OETF
            // would brighten the mid tones visibly (gray 128 -> 140), and most images that carry these values
            // were made from sRGB data anyway.
            const cmsFloat64Number params[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
            return cmsBuildParametricToneCurve(nullptr, 4, params);
        }
        case 4:  // gamma 2.2 (BT.470 System M)
            return cmsBuildGamma(nullptr, 2.2);
        case 5:  // gamma 2.8 (BT.470 System B/G)
            return cmsBuildGamma(nullptr, 2.8);
        case 8:  // linear
            return cmsBuildGamma(nullptr, 1.0);
        default: // 16 (PQ), 18 (HLG) and everything else
            return nullptr;
    }
}

// Transfer characteristics that are shown with the sRGB curve (see CreateCICPToneCurve())
bool IsSRGBTransfer(unsigned int transfer)
{
    return transfer == 1 || transfer == 2 || transfer == 6 || transfer == 13 || transfer == 14 || transfer == 15;
}

} // namespace

void* ICCProfileTransform::CreateTransformFromCICP(unsigned int colorPrimaries, unsigned int transferCharacteristics, PixelFormat format)
{
    // BT.709 primaries with the sRGB curve is sRGB: nothing to do (the most common case)
    if ((colorPrimaries == 1 || colorPrimaries == 2) && IsSRGBTransfer(transferCharacteristics)) {
        return nullptr;
    }

    CICPPrimaries p;
    if (!GetCICPPrimaries(colorPrimaries, p)) {
        return nullptr;
    }
    cmsToneCurve* curve = CreateCICPToneCurve(transferCharacteristics);
    if (curve == nullptr) {
        return nullptr;
    }

    const cmsCIExyY whitePoint = { p.wx, p.wy, 1.0 };
    const cmsCIExyYTRIPLE primaries = { { p.rx, p.ry, 1.0 }, { p.gx, p.gy, 1.0 }, { p.bx, p.by, 1.0 } };
    cmsToneCurve* curves[3] = { curve, curve, curve };
    cmsHPROFILE hInProfile = cmsCreateRGBProfile(&whitePoint, &primaries, curves);
    cmsFreeToneCurve(curve);

    return CreateTransformToSRGB(hInProfile, format, sRGBProfile);
}

bool ICCProfileTransform::DoTransform(void* transform, const void* inputBuffer, void* outputBuffer, unsigned int width, unsigned int height, unsigned int stride)
{
    unsigned int numPixels = width * height;
    if (transform == nullptr || inputBuffer == nullptr || outputBuffer == nullptr || numPixels == 0 || !IsEnabled())
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

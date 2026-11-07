#ifndef PROCESSPARAMS_H
#define PROCESSPARAMS_H

#include "Helpers.h"
#include "WTLCompat.h"

// Processing flags for image processing
enum EProcessingFlags {
    PFLAG_None = 0,
    PFLAG_AutoContrast = 1,
    PFLAG_HighQualityResampling = 2,
    PFLAG_KeepParams = 4, // Keep parameters between images
    PFLAG_NoProcessingAfterLoad = 8
};

static inline EProcessingFlags SetProcessingFlag(EProcessingFlags eFlags, EProcessingFlags eFlagToSet, bool bValue) {
    return bValue ? (EProcessingFlags)(eFlags | eFlagToSet) : (EProcessingFlags)(eFlags & ~eFlagToSet);
};

static inline bool GetProcessingFlag(EProcessingFlags eFlags, EProcessingFlags eFlagToGet) {
    return (eFlags & eFlagToGet) != 0;
}

// Parameters for image processing, geometry independent
class CImageProcessingParams {
public:
    CImageProcessingParams(double dSaturation, double dContrastCorrectionFactor) {
        Saturation = dSaturation;
        ContrastCorrectionFactor = dContrastCorrectionFactor;
    }

    CImageProcessingParams() {
        Saturation = -1;
        ContrastCorrectionFactor = -1;
    }

    double Saturation; // in [0..2], 1 means saturation unchanged
    double ContrastCorrectionFactor; // in [0..1], 0 means no contrast correction
};

// Parameters used for image rotation
// Mirroring and rotating can not be swapped, so the state has a fixed order: the image is first mirrored horizontally
// (if Mirrored) and then rotated clockwise. A vertical mirroring is a horizontal one plus a rotation by 180 degrees.
class CRotationParams {
public:
    CRotationParams() : Rotation(0), Mirrored(false) {}

    explicit CRotationParams(int rotation, bool mirrored = false) : Rotation(rotation), Mirrored(mirrored) {}

    // Rotation of the image after mirroring, if the user changes an image already rotated by nBaseRotation (EXIF)
    static int CombineRotation(int nBaseRotation, int nUserRotation, bool bMirrored) {
        return (((bMirrored ? nUserRotation - nBaseRotation : nUserRotation + nBaseRotation) % 360) + 360) % 360;
    }

    // Rotation after mirroring an image with the given rotation in the current direction of view
    static int RotationAfterMirror(int nRotation, bool bHorizontally) {
        return (((bHorizontally ? -nRotation : 180 - nRotation) % 360) + 360) % 360;
    }

    int Rotation; // Main rotation in degrees, must be in [0, 90, 180, 270]
    bool Mirrored; // Mirrored horizontally before the rotation
};

class CProcessParams {
public:
    // TargetWidth, TargetHeight is the dimension of the target output window, the image is fit into this rectangle
    // rotationParams specifies base rotation and the mirroring by the user; nUserRotation will be combined with it
    // dZoom is the zoom factor compared to initial image size (1.0 means no zoom)
    // offsets are relative to center of image and refer to original image size (not zoomed)
    CProcessParams(int nTargetWidth,
                   int nTargetHeight,
                   const CRotationParams& rotationParams,
                   int nUserRotation,
                   double dZoom,
                   Helpers::EAutoZoomMode eAutoZoomMode,
                   CPoint offsets,
                   Helpers::ETransparencyMode eTransparencyMode,
                   const CImageProcessingParams& imageProcParams,
                   EProcessingFlags eProcFlags)
        : TargetWidth(nTargetWidth),
        TargetHeight(nTargetHeight),
        RotationParams(rotationParams),
        UserRotation(nUserRotation),
        Zoom(dZoom),
        AutoZoomMode(eAutoZoomMode),
        Offsets(offsets),
        TransparencyMode(eTransparencyMode),
        ImageProcParams(imageProcParams),
        ProcFlags(eProcFlags) {}

    int TargetWidth{0};
    int TargetHeight{0};
    CRotationParams RotationParams{};
    int UserRotation{0};
    double Zoom{1.0};
    Helpers::EAutoZoomMode AutoZoomMode{};
    CPoint Offsets{};
    Helpers::ETransparencyMode TransparencyMode{}; // for transparent background
    CImageProcessingParams ImageProcParams{};
    EProcessingFlags ProcFlags{};
};

#endif // PROCESSPARAMS_H

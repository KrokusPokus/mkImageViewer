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
class CRotationParams {
public:
    CRotationParams() : Rotation(0) {}

    explicit CRotationParams(int rotation) {
        Rotation = rotation;
    }

    CRotationParams(const CRotationParams& other) {
        Rotation = other.Rotation;
    }

    CRotationParams(const CRotationParams& other, int rotation) {
        Rotation = rotation;
    }

    int Rotation; // Main rotation in degrees, must be in [0, 90, 180, 270]
};

class CProcessParams {
public:
    // TargetWidth, TargetHeight is the dimension of the target output window, the image is fit into this rectangle
    // rotationParams specifies base rotation; nUserRotation will be added to it
    // dZoom is the zoom factor compared to initial image size (1.0 means no zoom)
    // offsets are relative to center of image and refer to original image size (not zoomed)
    CProcessParams(int nTargetWidth,
                   int nTargetHeight,
                   CSize monitorSize,
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
        MonitorSize(monitorSize),
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
    CSize MonitorSize{};
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

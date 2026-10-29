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

// Processing flags for free rotation
enum ERotationFlags {
    RFLAG_None = 0,
    RFLAG_AutoCrop = 1,
    RFLAG_KeepAspectRatio = 2
};

static inline ERotationFlags SetRotationFlag(ERotationFlags eFlags, ERotationFlags eFlagToSet, bool bValue) {
    return bValue ? (ERotationFlags)(eFlags | eFlagToSet) : (ERotationFlags)(eFlags & ~eFlagToSet);
};

static inline bool GetRotationFlag(ERotationFlags eFlags, ERotationFlags eFlagToGet) {
    return (eFlags & eFlagToGet) != 0;
}

// Parameters used for image rotation
class CRotationParams {
public:
    CRotationParams() : Rotation(0), Flags(RFLAG_None) {}

    explicit CRotationParams(int rotation) {
        Rotation = rotation;
        Flags = RFLAG_None;
    }

    CRotationParams(int rotation, ERotationFlags flags) {
        Rotation = rotation;
        Flags = flags;
    }

    CRotationParams(const CRotationParams& other) {
        Rotation = other.Rotation;
        Flags = other.Flags;
    }

    CRotationParams(const CRotationParams& other, int rotation) {
        Rotation = rotation;
        Flags = other.Flags;
    }

    int Rotation; // Main rotation in degrees, must be in [0, 90, 180, 270]
    ERotationFlags Flags;
};

/*
// Parameters used to process an image, including geometry
class CProcessParams {
public:
    // TargetWidth,TargetHeight is the dimension of the target output window, the image is fit into this rectangle
    // nRotation must be 0, 90, 270 degrees, nUserRotation will be added to it
    // dZoom is the zoom factor compared to initial image size (1.0 means no zoom)
    // offsets are relative to center of image and refer to original image size (not zoomed)
    CProcessParams(int nTargetWidth, int nTargetHeight,
        CSize monitorSize,
        const CRotationParams& rotationParams,
        int nUserRotation,
        double dZoom,
        Helpers::EAutoZoomMode eAutoZoomMode, CPoint offsets,
        Helpers::ETransparencyMode nTransparencyMode,
        const CImageProcessingParams& imageProcParams,
        EProcessingFlags eProcFlags) : ImageProcParams(imageProcParams), RotationParams(rotationParams) {
        UserRotation = nUserRotation;
        TargetWidth = nTargetWidth;
        TargetHeight = nTargetHeight;
        MonitorSize = monitorSize;
        Zoom = dZoom;
        AutoZoomMode = eAutoZoomMode;
        Offsets = offsets;
        TransparencyMode = nTransparencyMode;
        ProcFlags = eProcFlags;
    }

    int TargetWidth;
    int TargetHeight;
    CSize MonitorSize;
    CRotationParams RotationParams;
    int UserRotation;
    double Zoom;
    CPoint Offsets;
    Helpers::ETransparencyMode TransparencyMode; //for transparent background
    CImageProcessingParams ImageProcParams;
    EProcessingFlags ProcFlags;
    Helpers::EAutoZoomMode AutoZoomMode;
};
*/
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

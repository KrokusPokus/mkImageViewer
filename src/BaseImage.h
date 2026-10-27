#ifndef BASEIMAGE_H
#define BASEIMAGE_H

#include "ProcessParams.h"
#include "libjpeg-turbo/include/turbojpeg.h" // for enum TJSAMP

#include <QString>

class CHistogram;
class CLocalDensityCorr;
class CEXIFReader;

// Represents a rectangle to dim out in the image
struct CDimRect {
    CDimRect() {}
    CDimRect(float fFactor, const CRect& rect) { Factor = fFactor; Rect = rect; }
    float Factor; // between 0.0 and 1.0
    CRect Rect;
};

// Class holding a decoded image (not just JPEG - any supported format) and its meta data (if available).
class CBaseImage {
public:
    // Ownership of memory in pPixels goes to class, accessing this pointer after the constructor has been called
    // may causes access violations (use OriginalPixels() instead).
    // nChannels can be 1 (greyscale image), 3 (BGR color image) or 4 (BGRA color image, A ignored)
    // pEXIFData can be a pointer to the APP1 block containing the EXIF data. If this pointer is null
    // no EXIF data is available.
    // The nJPEGHash hash value gives a hash over the compressed JPEG pixels that uniquely identifies the
    // JPEG image. It can be zero in which case a pixel based hash value is internally created.
    // The image format is a hint about the original image format this image was created from.
    // The bIsAnimation, nFrameIndex, nNumberOfFrames and nFrameTimeMs are used for multiframe images, e.g. animated GIFs.
    // Frame index is zero based and the frame time is given in milliseconds.
    // The pLDC object is used internally only for thumbnail image creation to avoid duplication. In all other situations,
    // its value must be NULL.
    // If RAW metadata is specified, ownership of this memory is transferred to this class.

    CBaseImage(int nWidth, int nHeight, void* pPixels, void* pEXIFData, int nChannels,
        int64_t nJPEGHash, EImageFormat eImageFormat, bool bIsAnimation, int nFrameIndex, int nNumberOfFrames, int nFrameTimeMs);

    CBaseImage(int nWidth, int nHeight, void* pPixels, void* pEXIFData, int nChannels,
        int64_t nJPEGHash, EImageFormat eImageFormat, EImageFormat eContainerFormat = IF_Unknown, bool bIsAnimation = false, int nFrameIndex = 9, int nNumberOfFrames = 1, int nFrameTimeMs = 0);

    ~CBaseImage(void);

    // Gets resampled and processed 32 bpp DIB image (up or downsampled).
    // Parameters:
    // fullTargetSize: Full target size of resized image to render (without any clipping to actual clipping size)
    // clippingSize: Sub-rectangle in fullTargetSize to render (the returned DIB has this size)
    // targetOffset: Offset of the clipping rectangle in full target size
    // imageProcParams: Processing parameters, such as contrast and brightness
    // eProcFlags: Processing flags, e.g. apply LDC
    // Return value:
    // The returned DIB is a 32 bpp DIB (BGRA). Note that the returned pointer must not
    // be deleted by the caller and is only valid until another call to GetDIB() is done, the CBaseImage
    // object is disposed or the Rotate() or any other method affecting original pixels is called!
    // Note: The method tries to satisfy consecutive requests as efficient as possible. Calling the GetDIB()
    // method multiple times with the same set of parameters will return the same image starting from the second
    // call without doing image processing anymore.
    void* GetDIB(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset,
        const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags) {
        bool bNotUsed;
        return GetDIBInternal(fullTargetSize, clippingSize, targetOffset, imageProcParams, eProcFlags, bNotUsed);
    }

    // Operations on original pixels of the image

    // Mirrors the image horizontally or vertically.
    // Applies to original pixels!
    bool Mirror(bool bHorizontally);

    // Rotate the image clockwise by 90, 180 or 270 degrees. All other angles are invalid.
    // Applies to original pixels!
    bool Rotate(int nRotation);

    // Gets the hash value of the pixels, for JPEGs the hash is on the compressed pixels
    int64_t GetPixelHash() const { return m_nPixelHash; }

    // Original image size. Operations on the original pixels will change this size!
    int OrigWidth() const { return m_nOrigWidth; }
    int OrigHeight() const { return m_nOrigHeight; }
    CSize OrigSize() const { return CSize(m_nOrigWidth, m_nOrigHeight); }

    // Original image size at the time the CBaseImage was constructed.
    // Operations on the original pixels will not change this size!
    int InitOrigWidth() const { return m_nInitOrigWidth; }
    int InitOrigHeight() const { return m_nInitOrigHeight; }

    // Size of DIB - size of resampled section of the original image. If zero, no DIB is currently available.
    int DIBWidth() const { return m_ClippingSize.cx; }
    int DIBHeight() const { return m_ClippingSize.cy; }

    // Gets the rotation applied to the pixels
    const CRotationParams& GetRotationParams() {return m_rotationParams; }

    // Gets or sets the JPEG chromo sampling. See turbojpeg.h for the TJSAMP enumeration.
    TJSAMP GetJPEGChromoSampling() { return m_eJPEGChromoSampling; }
    void SetJPEGChromoSampling(TJSAMP eSampling) { m_eJPEGChromoSampling = eSampling; }

    // Declare the cached DIB as invalid - forcing it to be regenerated on next GetDIB() call
    void SetDIBInvalid() { m_ClippingSize = CSize(0, 0); }

    // Verify that the image is currently rotated by the specified parameters and rotate the original pixels of the image if not.
    bool VerifyRotation(const CRotationParams& rotationParams);

    // Returns if this image has been cropped or not
    bool IsCropped() { return m_bCropped; }

    // Returns if this image's original pixels have been processed destructively (e.g. cropped or rotated by non-90 degrees steps)
    bool IsDestructivelyProcessed() { return m_bIsDestructivelyProcessed; }

    // Returns if this image has been processed in a way not supported to be stored in the parameter DB.
    bool IsProcessedNoParamDB() { return m_bIsProcessedNoParamDB; }

    // raw access to original pixels - do not delete or store the returned pointer
    void* OriginalPixels() { return  m_pOrigPixels; }
    const void* OriginalPixels() const { return m_pOrigPixels; }
    // remove original pixels from class - OriginalPixels() will return NULL afterwards
    void DetachOriginalPixels() { m_pOrigPixels = NULL; }

    // returns the number of channels in the OriginalPixels (3 or 4, corresponding to 24 bpp and 32 bpp)
    int OriginalChannels() const { return m_nOriginalChannels; }

    // raw access to DIB pixels with no LUT applied - do not delete or store the returned pointer
    // note that this DIB can be NULL due to optimization if currently only the processed DIB is maintained
    void* DIBPixels() { return m_pDIBPixels; }
    const void* DIBPixels() const { return m_pDIBPixels; }

    // Verifies that the original DIB pixels (DIBPixels()) are available
    void VerifyDIBPixelsCreated();

    // Gets the DIB last processed. If none, the last used parameters are taken to generate the DIB - if bGenerateDIBIfNeeded is true 
    void* DIBPixelsLastProcessed(bool bGenerateDIBIfNeeded);

    // Gets the image processing flags as set as default (may varies from file to file)
    EProcessingFlags GetInitialProcessFlags() const { return m_eProcFlagsInitial; }

    // Gets the image processing flags last used to process an image
    EProcessingFlags GetLastProcessFlags() const { return m_eProcFlags; }
    
    // Gets the image processing parameters as set as default (may varies from file to file)
    const CImageProcessingParams& GetInitialProcessParams() const { return m_imageProcParamsInitial; }

    // Gets the image processing parameters last used to process an image
    const CImageProcessingParams& GetLastProcessParams() const { return m_imageProcParams; } 

    // Gets the rotation as set as default (may varies from file to file)
    int GetInitialRotation() const { return m_nInitialRotation; }

    // Gets the zoom as set as default (can vary from file to file)
    double GetInititialZoom() const { return m_dInitialZoom; }

    // Gets the offsets as set as default (can vary from file to file)
    CPoint GetInitialOffsets() const { return m_initialOffsets; }

    // Restores the initial parameters to the parameters dependent on the directory (not the file)
    // Outputs the processing flags set for this directory
    void RestoreInitialParameters(const QString& sFileName, const CImageProcessingParams& imageProcParams, 
        EProcessingFlags & procFlags, int nRotation, double dZoom, CPoint offsets, CSize targetSize, CSize monitorSize);

    // To be called after creation of the object to initialize the initial processing parameters.
    // Input are the global defaults for the processing parameters, output (in pParams) are the
    // processing parameters for this file (maybe different from the global ones)
    void SetFileDependentProcessParams(const QString& sFileName, CProcessParams* pParams);

    // Gets if the image was found in parameter DB
    bool IsInParamDB() const { return m_bInParamDB; }

    // Sets that this image is now in the parameter DB or is removed from the parameter DB
    // (called after the user saves/deletes the image from param DB)
    void SetIsInParamDB(bool bSet) { m_bInParamDB = bSet; }

    // Gets if zoom and offset values are stored for this image in the parameter DB
    bool HasZoomStoredInParamDB() const { return m_bHasZoomStoredInParamDB; }

    // Gets the EXIF data block (including the APP1 marker in the first two bytes) of the image. Returns NULL if none.
    void* GetEXIFData() { return m_pEXIFData; }

    // Gets the size of the EXIF data block in bytes, including the APP1 marker (2 bytes)
    int GetEXIFDataLength() { return m_nEXIFSize; }

    // Gets the EXIF reader for this image if the image contains EXIF data, NULL if not.
    CEXIFReader* GetEXIFReader() { return m_pEXIFReader; }

    // Gets the image format this image has originally
    EImageFormat GetImageFormat() const { return m_eImageFormat; }

    // Gets the container format of this image; IF_Unknown is not
    EImageFormat GetContainerFormat() const { return m_eContainerFormat; }
    bool IsContainer() const { return m_eContainerFormat != IF_Unknown; }
    bool ContainerHasMultipleImages() const { return (m_eContainerFormat != IF_Unknown) && (m_nNumberOfFrames > 1); }

    // Gets if this image is part of an animation (animated GIF)
    bool IsAnimation() const { return m_bIsAnimation; }

    // Gets the frame index if this is a multiframe image, 0 otherwise
    int FrameIndex() const { return m_nFrameIndex; }

    // Gets the number of frames for multiframe images, 1 otherwise
    int NumberOfFrames() const { return m_nNumberOfFrames; }

    // Gets the frame time in milliseconds for animations (animated GIF)
    // Defaults to 100ms for frame times <=10, to match behavior of web browsers
    int FrameTimeMs() const { return m_nFrameTimeMs <= 10 ? 100 : m_nFrameTimeMs; }

    // Gets if the rotation is given by EXIF data
    bool IsRotationByEXIF() const { return m_bRotationByEXIF; }

    // Sets and gets the JPEG comment of this image (COM marker).
    void SetJPEGComment(const QString& sComment) { m_sJPEGComment = sComment; }
    QString GetJPEGComment() { return m_sJPEGComment; }

    // Converts the target offset from 'center of image' based format to pixel coordinate format 
    static CPoint ConvertOffset(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset);

    // Debug: Ticks (milliseconds) of the last GetDIB
    uint64_t GetDIBLoadDelayMs() const { return m_dibLoadDelayMs; }

    // Debug: Loading time of image in ms
    void SetFileLoadDelayMs(uint64_t tc) { m_fileLoadDelayMs = tc; }
    uint64_t GetFileLoadDelayMs() { return m_fileLoadDelayMs; }

private:

    // used internally for re-sampling type
    enum EResizeType {
        NoResize,
        DownSample,
        UpSample
    };

    // Original pixel data - only rotations and crop are done directly on this data because this is non-destructive
    // The data is not modified in all other cases
    void* m_pOrigPixels;
    void* m_pEXIFData;
    int m_nEXIFSize;
    CEXIFReader* m_pEXIFReader;
    QString m_sJPEGComment;
    int m_nOrigWidth, m_nOrigHeight; // these may changes by rotation
    int m_nInitOrigWidth, m_nInitOrigHeight; // original width of image when constructed (before any rotation and crop)
    int m_nOriginalChannels;
    int64_t m_nPixelHash;
    EImageFormat m_eImageFormat, m_eContainerFormat;
    TJSAMP m_eJPEGChromoSampling;

    // multiframe and GIF animation related data
    bool m_bIsAnimation;
    int m_nFrameIndex;
    int m_nNumberOfFrames;
    int m_nFrameTimeMs;

    // Thumbnail related stuff
    bool m_bIsThumbnailImage;
    CHistogram* m_pCachedProcessedHistogram;

    // Processed data of size m_ClippingSize, with LUT/LDC applied and without
    // The version without LUT/LDC is used to efficiently reapply a different LUT/LDC
    // Size of the DIBs is m_ClippingSize
    void* m_pDIBPixelsLUTProcessed;
    void* m_pDIBPixels;
    void* m_pLastDIB; // one of the pointers above

    // Image processing parameters and flags during last call to GetDIB()
    CImageProcessingParams m_imageProcParams;
    EProcessingFlags m_eProcFlags;

    // Initial image processing parameters and flags, as set with SetFileDependentProcessParams()
    CImageProcessingParams m_imageProcParamsInitial;
    EProcessingFlags m_eProcFlagsInitial;
    int m_nInitialRotation;
    double m_dInitialZoom;
    CPoint m_initialOffsets;

    bool m_bCropped; // Image has been cropped
    bool m_bIsDestructivelyProcessed; // Original image pixels destructively processed (i.e. cropped or size changed)
    bool m_bIsProcessedNoParamDB;
    CRotationParams m_rotationParams; // current rotation
    bool m_bRotationByEXIF; // is the rotation given by EXIF

    // This is the geometry that was requested during last GetDIB() call
    CSize m_FullTargetSize; 
    CSize m_ClippingSize; // this is the size of the DIB
    CPoint m_TargetOffset;

    bool m_bInParamDB; // true if image found in param DB
    bool m_bHasZoomStoredInParamDB; // true if image in param DB and entry contains zoom and offset values
    bool m_bFirstReprocessing; // true if never reprocessed before, some optimizations may be not done initially

    uint64_t m_dibLoadDelayMs;
    uint64_t m_fileLoadDelayMs;

    // stuff needed to perform LUT and LDC processing
    uint8* m_pLUTAllChannels; // for global contrast and brightness correction
    uint8* m_pLUTRGB; // B,G,R three channel LUT
    int32* m_pSaturationLUTs; // Saturation LUTs
    CLocalDensityCorr* m_pLDC;
    bool m_bLDCOwned;
    float m_fColorCorrectionFactors[6];
    float m_fColorCorrectionFactorsNull[6];

    // Internal GetDIB() implementation with (low quality) rotation with GetDIB().
    // bUsingOriginalDIB is output parameter and contains if the cached DIB could be used and no processing has been done
    void* GetDIBInternal(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset,
                         const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
                         bool& bParametersChanged);

    // Resample when panning was done, using existing data in DIBs. Old clipping rectangle is given in oldClippingRect
    void ResampleWithPan(void* & pDIBPixels, void* & pDIBPixelsLUTProcessed, CSize fullTargetSize, 
        CSize clippingSize, CPoint targetOffset, CRect oldClippingRect,
        EProcessingFlags eProcFlags, const CImageProcessingParams & imageProcParams, double dRotation, EResizeType eResizeType);

    // Resample to given target size. Returns resampled DIB
    void* Resample(CSize fullTargetSize, CSize clippingSize, CPoint targetOffset, EProcessingFlags eProcFlags, EResizeType eResizeType);

    // pCachedTargetDIB is a pointer at the caller side holding the old processed DIB.
    // Returns a pointer to DIB to be used (either pCachedTargetDIB or pSourceDIB)
    // If bOnlyCheck is set to true, the method does nothing but only checks if the existing processed DIB
    // can be used (return != NULL) or not (return == NULL)
    // The out parameter bParametersChanged returns if one of the parameters relevant for image processing has been changed since the last call
    void* ApplyCorrectionLUTandLDC(const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
        void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
        void * pSourceDIB, CSize dibSize, bool bGeometryChanged, bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB, bool &bParametersChanged);

    void* ApplyCorrectionLUTandLDC(const CImageProcessingParams & imageProcParams, EProcessingFlags eProcFlags,
        void * & pCachedTargetDIB, CSize fullTargetSize, CPoint targetOffset, 
        void * pSourceDIB, CSize dibSize, bool bGeometryChanged, bool bOnlyCheck, bool bCanTakeOwnershipOfSourceDIB) {
        bool bNotUsed;
        return ApplyCorrectionLUTandLDC(imageProcParams, eProcFlags, pCachedTargetDIB, fullTargetSize, targetOffset, 
            pSourceDIB, dibSize, bGeometryChanged, bOnlyCheck, bCanTakeOwnershipOfSourceDIB, bNotUsed);
    }

    // makes sure that the input image (m_pOrigPixels) is a 4 channel BGRA image (converts if necessary)
    bool ConvertSrcTo4Channels();

    // Get if from source to target size it is down or upsampling
    EResizeType GetResizeType(CSize targetSize, CSize sourceSize);

    // Gets the rotation from EXIF if available
    int GetRotationFromEXIF(int nOrigRotation);

    // Sets the m_bIsDestructivelyProcessed flag to true and resets rotation
    void MarkAsDestructivelyProcessed();

    // Called when the original pixels have changed (rotate, crop), all cached pixel data gets invalid
    void InvalidateAllCachedPixelData();
};


#endif // BASEIMAGE_H

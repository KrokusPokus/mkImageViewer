#ifndef IMAGELOADTHREAD_H
#define IMAGELOADTHREAD_H

#include "ProcessParams.h"
#include "WorkThread.h"
#include "libbit7z/include/bittypes.hpp"

#include <functional>
#include <memory>
#include <vector>

#include <QAtomicInt>
#include <QFile>
#include <QImageReader>
#include <QString>

enum class EDecodeStatus { Ok, NotDecodable, OutOfMemory, Exception };

struct CDecodeTarget {
    bool bInArchive = false;
    bool bMultipleFiles = false;
    int  nArchiveCount = 1;
    int  nEntryIndex = 0;
};

class CJPEGImage;

class ZipEntry
{
public:
    QString name;
    QString ext;
    unsigned int index;
    unsigned long long size;

    ZipEntry(const char* a_pchName, unsigned int a_index, unsigned long long a_size);
    ZipEntry(const bit7z::tstring& a_name, unsigned int a_index, unsigned long long a_size);
};

class CImageData
{
public:
    CJPEGImage* Image;
    bool IsRequestFailedOutOfMemory;
    bool IsRequestFailedException;

    CImageData(CJPEGImage* pImage, bool isRequestFailedOutOfMemory, bool isRequestFailedException) {
        Image = pImage;
        IsRequestFailedOutOfMemory = isRequestFailedOutOfMemory;
        IsRequestFailedException = isRequestFailedException;
    }
};

class CImageLoadThread : public CWorkThread
{
public:
    CImageLoadThread(void);
    ~CImageLoadThread(void);

    // Asynchronous loading  of an image.
    // Returns identifier to query for resulting image. The query can be done as soon as the message is
    // received or the event has been signaled.
    // The file to load is given by its filename (with path) and the frame index (for multiframe images). The
    // frame index needs to be zero when the image only has one frame.
    int AsyncLoad(const QString& strFileName, int nFrameIndex, const CProcessParams & processParams, std::function<void(int)> onCompleted);

    // Get loaded image, CImageData::Image is null if not (yet) available - use handle returned by AsyncLoad().
    // Marks the request for deletion - only call once with the same handle
    CImageData GetLoadedImage(int nHandle);

    // Releases the cached image file if an image of the specified name is cached
    void ReleaseFile(const QString& strFileName);

private:

    // Request for loading an image
    class CRequest : public CRequestBase {
    public:
        CRequest(const QString& strFileName, int nFrameIndex, const CProcessParams& processParams, std::function<void(int)> callback)
            : CRequestBase(nullptr), ProcessParams(processParams) {
            FileName = strFileName;
            FrameIndex = nFrameIndex;
            CompletedCallback = callback;
            RequestHandle = ++m_curHandle;
            Image = NULL;
            OutOfMemory = false;
            ExceptionError = false;
        }

        QString FileName;
        int FrameIndex;
        std::function<void(int)> CompletedCallback;
        int RequestHandle;
        CJPEGImage* Image;
        CProcessParams ProcessParams;
        bool OutOfMemory;
        bool ExceptionError;
    };

    class CReleaseFileRequest : public CRequestBase {
    public:
        CReleaseFileRequest(const QString& strFileName) : CRequestBase(nullptr) {
            FileName = strFileName;
            Type = ReleaseFileRequest;
        }

        enum { ReleaseFileRequest = 1 };

        QString FileName;
    };


    QString m_sLastFileName;
    QString m_sLastWebpFileName;
    QString m_sLastPngFileName;
    QString m_sLastJxlFileName;
    QString m_sLastAvifFileName;
    QString m_sLastZipFileName;
    int m_nZipCount;
    std::vector<ZipEntry> zipEntries;

    virtual void ProcessRequest(CRequestBase& request) override;
    virtual void AfterFinishProcess(CRequestBase& request) override;
    void DeleteCachedWebpDecoder();
    void DeleteCachedPngDecoder();
    void DeleteCachedJxlDecoder();
    void DeleteCachedAvifDecoder();
    void DeleteCachedZip();

    // in CImageLoadThread: die neun ProcessRead*Request-Deklarationen entfernen, dafür
    void ProcessReadFileRequest(CRequest*, EImageFormat);
    EDecodeStatus DecodeFromBuffer(CRequest*, EImageFormat, std::unique_ptr<char[]>&, size_t, const CDecodeTarget&);
    QString* CachedFileNameFor(EImageFormat);
    void DeleteCachedDecoder(EImageFormat);
/*
    void ProcessReadAVIFRequest(CRequest* request);
    void ProcessReadBMPRequest(CRequest * request);
    void ProcessReadHEIFRequest(CRequest * request);
    void ProcessReadJPEGRequest(CRequest * request);
    void ProcessReadJXLRequest(CRequest* request);
    void ProcessReadPNGRequest(CRequest * request);
    void ProcessReadPSDRequest(CRequest * request);
    void ProcessReadQOIRequest(CRequest * request);
    void ProcessReadTGARequest(CRequest * request);
    void ProcessReadWEBPRequest(CRequest * request);
*/
    void ProcessReadZipRequest(CRequest* request);
    bool ValidateZipFrameIndex(int nInitialIndex, int &nFrameIndex);

    static void SetFileDependentProcessParams(CRequest * request);
    static bool ProcessImageAfterLoad(CRequest * request);

    //static QAtomicInt m_curHandle;
    static std::atomic<int> m_curHandle;
    enum { ReadRequest = 0, ReleaseFileRequest = 1 };  // in CRequest setzen Type = ReadRequest
    void OnCancelled(CRequestBase&) override;

    std::unique_ptr<char[]> m_pJxlBuffer;   // Eingabepuffer des JXL-Decoders bei Animationen

    // QImageReader
    std::unique_ptr<QFile>        m_pLastImageFile;    // VOR dem Reader deklarieren
    std::unique_ptr<QImageReader> m_pLastImageReader;
    int m_nLastQtFrameIndex = -1; // GIF: zuletzt gelesenes Frame
    int m_nLastQtFrameCount = 1;
    EImageFormat m_eLastQtFormat = IF_Unknown;
    bool m_bLastQtAnimation = false;
    void DeleteCachedQtImageReader();
    void ProcessReadQtImageRequest(CRequest* request);
};

#endif // IMAGELOADTHREAD_H
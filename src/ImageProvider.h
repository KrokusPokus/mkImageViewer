#ifndef IMAGEPROVIDER_H
#define IMAGEPROVIDER_H

#include <list>
#include <memory>
#include <atomic>
#include <QString>
#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QDebug>

#include "ImageLoadResult.h"

class CBaseImage;
class CImageLoadThread;
class CFileList;
class CProcessParams;

class CImageProvider : public QObject {
    Q_OBJECT

public:
    enum EReadAheadDirection {
        NONE,
        FORWARD,
        BACKWARD
    };

    // nNumBuffers: number of images that are read ahead (besides the requested one)
    CImageProvider(QObject* pParent, int nNumBuffers);
    ~CImageProvider(void);

    // Returns the image (nullptr if it could not be loaded), the reason of a failure and the position (file, frame,
    // number of frames). The position is also known if the image could not be loaded, e.g. for a page of an archive.
    CImageLoadResult RequestImage(CFileList* pFileList, EReadAheadDirection eDirection, const QString& sFileName, int nFrameIndex,
                                  const CProcessParams & processParams);

    void NotifyNotUsed(CBaseImage* pImage);
    bool ClearRequest(CBaseImage* pImage, bool releaseLockedFile = false);
    void ClearAllRequests();
    bool FreeAllPossibleMemory();

    // Tells the provider that a file was renamed (full paths). Cached and pending requests of the
    // file continue under the new name, so FindRequest() finds the image again without decoding it anew.
    // Call from the GUI thread only, like all other methods that touch m_requestList.
    void FileHasRenamed(const QString& sOldFileName, const QString& sNewFileName);

    // The file has to be read anew (reload, e.g. after it was changed on disk): the load thread drops what it keeps
    // for the file (decoders, archive listing) and all cached images of the file (every frame or page) are removed.
    // Images in use are not touched. Call from the GUI thread only.
    void ReloadFile(const QString& sFileName);

private:
    struct CImageRequest {
        QString FileName;
        int FrameIndex;
        CBaseImage* Image;
        std::atomic<bool> Ready; // Thread-sicher über atomare Sequenzierung
        int Handle;
        bool InUse;
        bool IsActive;
        ELoadError Error = ELoadError::None;
        CImagePosition Position;    // FileName is not set here (FileName above is the current name)
        int AccessTimeStamp;

        QMutex Mutex;
        QWaitCondition WaitCondition;

        CImageRequest(const QString& fileName, int nFrameIndex)
            : FileName(fileName),
            FrameIndex(nFrameIndex),
            Image(nullptr),
            Ready(false),
            Handle(-1),
            InUse(false),
            IsActive(true),
            AccessTimeStamp(-1) {}

        // Defined in JPEGProvider.cpp: CBaseImage is only forward declared here, so a delete in the header
        // would not run ~CBaseImage() (undefined behavior, all pixel buffers of the image would leak).
        ~CImageRequest();
    };

    using RequestPtr = std::shared_ptr<CImageRequest>;

    std::list<RequestPtr> m_requestList;

    // The one and only load thread, deliberately only one:
    //  - The image readers are not thread safe: PNG, AVIF and JPEG XL keep the decoder state of animations in
    //    global or static caches (g_cache in wrapperPNG.cpp and wrapperAVIF.cpp, JxlReader::cache).
    //  - Latency: the requested image must be delivered as fast as possible, then the most likely next image is
    //    prepared. Decoding and resampling of a single image already use all cores (thread pool, multithreaded
    //    decoders). A second load thread would compete for cores and memory bandwidth and slow down the image the
    //    user is waiting for. The requests are processed one after the other, in the order they were started.
    //    Exception: before the user waits for an image, read ahead requests that are queued are dropped and a
    //    running one that is no longer needed is stopped before resampling (CancelPendingReadAhead()).
    std::unique_ptr<CImageLoadThread> m_pLoadThread;
    int m_nNumBuffers;
    int m_nCurrentTimeStamp;

    void RemoveUnusedImages(bool bRemoveAlsoReadAhead);
    RequestPtr StartRequestAndWaitUntilReady(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams);
    // bReadAhead: pre-caching of a neighbour (skipped by the load thread while memory is short)
    RequestPtr StartNewRequest(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams, bool bReadAhead = false);
    void StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection, const CProcessParams & processParams, int nNumRequests, RequestPtr pLastReadyRequest);
    RequestPtr FindRequest(const QString& sFileName, int nFrameIndex);
    std::list<RequestPtr>::iterator DeleteElementAt(std::list<RequestPtr>::iterator iteratorAt);
    void DeleteElement(RequestPtr pRequest);
    bool IsDestructivelyProcessed(CBaseImage* pImage);
    void SetAllRequestsInactive();

    // Called before waiting for pKeep, so that it does not wait behind images nobody needs now:
    //  - drops all requests the load thread has not started yet (except pKeep); StartNewRequestBundle() queues the
    //    still needed neighbours again afterwards, behind pKeep;
    //  - asks a running request to stop before resampling if its file is not in the read ahead range around
    //    sFileName (a running neighbour that is needed anyway is not stopped: its work would be lost).
    // Both are removed from the list. A stopped request still completes; its result is released together with the
    // request object, which only the completion callback holds then.
    void CancelPendingReadAhead(const RequestPtr& pKeep, CFileList* pFileList, EReadAheadDirection eDirection, const QString& sFileName);
};

#endif // IMAGEPROVIDER_H

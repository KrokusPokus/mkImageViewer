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

    CImageProvider(QObject* pParent, int nNumThreads, int nNumBuffers);
    ~CImageProvider(void);

    CBaseImage* RequestImage(CFileList* pFileList, EReadAheadDirection eDirection, const QString& sFileName, int nFrameIndex,
                             const CProcessParams & processParams, bool& bOutOfMemory, bool& bExceptionError);

    void NotifyNotUsed(CBaseImage* pImage);
    bool ClearRequest(CBaseImage* pImage, bool releaseLockedFile = false);
    void ClearAllRequests();
    bool FreeAllPossibleMemory();

    // Tells the provider that a file was renamed (full paths). Cached and pending requests of the
    // file continue under the new name, so FindRequest() finds the image again without decoding it anew.
    // Call from the GUI thread only, like all other methods that touch m_requestList.
    void FileHasRenamed(const QString& sOldFileName, const QString& sNewFileName);

private:
    struct CImageRequest {
        QString FileName;
        int FrameIndex;
        CBaseImage* Image;
        std::atomic<bool> Ready; // Thread-sicher über atomare Sequenzierung
        int Handle;
        bool InUse;
        bool IsActive;
        bool OutOfMemory;
        bool ExceptionError;
        int AccessTimeStamp;
        CImageLoadThread* HandlingThread;

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
            OutOfMemory(false),
            ExceptionError(false),
            AccessTimeStamp(-1),
            HandlingThread(nullptr) {}

        // Defined in JPEGProvider.cpp: CBaseImage is only forward declared here, so a delete in the header
        // would not run ~CBaseImage() (undefined behavior, all pixel buffers of the image would leak).
        ~CImageRequest();
    };

    using RequestPtr = std::shared_ptr<CImageRequest>;

    std::list<RequestPtr> m_requestList;
    CImageLoadThread** m_pWorkThreads;
    int m_nNumThread;
    int m_nNumBuffers;
    int m_nCurrentTimeStamp;

    CImageLoadThread* SearchThreadForNewRequest(void);
    void RemoveUnusedImages(bool bRemoveAlsoReadAhead);
    RequestPtr StartRequestAndWaitUntilReady(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams);
    RequestPtr StartNewRequest(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams);
    void StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection, const CProcessParams & processParams, int nNumRequests, RequestPtr pLastReadyRequest);
    RequestPtr FindRequest(const QString& sFileName, int nFrameIndex);
    std::list<RequestPtr>::iterator DeleteElementAt(std::list<RequestPtr>::iterator iteratorAt);
    void DeleteElement(RequestPtr pRequest);
    bool IsDestructivelyProcessed(CBaseImage* pImage);
    void SetAllRequestsInactive();
};

#endif // IMAGEPROVIDER_H

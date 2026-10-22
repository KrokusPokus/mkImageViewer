#ifndef JPEGPROVIDER_H
#define JPEGPROVIDER_H

#include <list>
#include <memory>
#include <atomic>
#include <QString>
#include <QObject>
#include <QMutex>
#include <QWaitCondition>

class CJPEGImage;
class CImageLoadThread;
class CFileList;
class CProcessParams;

class CJPEGProvider : public QObject {
    Q_OBJECT

public:
    enum EReadAheadDirection {
        NONE,
        FORWARD,
        BACKWARD
    };

    CJPEGProvider(QObject* pParent, int nNumThreads, int nNumBuffers);
    ~CJPEGProvider(void);

    CJPEGImage* RequestImage(CFileList* pFileList, EReadAheadDirection eDirection, const QString& sFileName, int nFrameIndex,
                             const CProcessParams & processParams, bool& bOutOfMemory, bool& bExceptionError);

    void NotifyNotUsed(CJPEGImage* pImage);
    bool ClearRequest(CJPEGImage* pImage, bool releaseLockedFile = false);
    void ClearAllRequests();
    bool FreeAllPossibleMemory();

private:
    struct CImageRequest {
        QString FileName;
        int FrameIndex;
        CJPEGImage* Image;
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

        ~CImageRequest() {
            delete Image;
        }
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
    bool IsDestructivelyProcessed(CJPEGImage* pImage);
    void SetAllRequestsInactive();
};

#endif // JPEGPROVIDER_H

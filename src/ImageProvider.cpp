#include "ImageProvider.h"
#include "BaseImage.h"
#include "ImageLoadThread.h"
#include "FileList.h"
#include "ProcessParams.h"

#include <climits>
#include <QMutexLocker>
#include <QDebug>

namespace {

bool CanReadAhead(CBaseImage* pImage) {
    bool isMultiFrame = pImage != nullptr && pImage->NumberOfFrames() > 1;
    bool isAnimation = pImage != nullptr && pImage->IsAnimation();
    bool isContainer = pImage != nullptr && pImage->IsContainer();
    if (isMultiFrame) {
        if (isAnimation) {
            return false;
        } else if (isContainer) {
            return true;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

CImageProvider::CImageRequest::~CImageRequest() {
    delete Image; // CBaseImage is a complete type here (JPEGImage.h)
}

CImageProvider::CImageProvider(QObject* pParent, int nNumThreads, int nNumBuffers)
    : QObject(pParent),
    m_nNumThread(std::max(1, nNumThreads)),
    m_nNumBuffers(std::max(0, nNumBuffers)),
    m_nCurrentTimeStamp(0) {
    m_pWorkThreads = new CImageLoadThread*[m_nNumThread];
    for (int i = 0; i < m_nNumThread; i++) {
        m_pWorkThreads[i] = new CImageLoadThread();
    }
}

CImageProvider::~CImageProvider(void) {
    for (int i = 0; i < m_nNumThread; i++) {
        delete m_pWorkThreads[i];
    }
    delete[] m_pWorkThreads;
}

CBaseImage* CImageProvider::RequestImage(CFileList* pFileList, EReadAheadDirection eDirection,
                                        const QString& sFileName, int nFrameIndex, const CProcessParams & processParams,
                                        bool& bOutOfMemory, bool& bExceptionError) {
    if (sFileName.isEmpty()) {
        bOutOfMemory = false;
        bExceptionError = false;
        return nullptr;
    }

    RequestPtr pRequest = FindRequest(sFileName, nFrameIndex);
    bool bWasOutOfMemory = false;
    if (pRequest == nullptr) {
        qDebug() << "CImageProvider::RequestImage() -> StartNewRequest() Frame" << nFrameIndex << "of" << sFileName;
        pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);
    } else {
        qDebug() << "CImageProvider::RequestImage() Retrieving existing request: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    // Warten auf das Laden des Bildes via QWaitCondition
    if (!pRequest->Ready) {
        qDebug() << "CImageProvider::RequestImage() waiting for image to finish loading from disk: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
        QMutexLocker locker(&pRequest->Mutex);
        while (!pRequest->Ready) {
            pRequest->WaitCondition.wait(&pRequest->Mutex);
        }
    } else {
        CBaseImage* pImage = pRequest->Image;
        if (pImage != nullptr) {
            EProcessingFlags procFlags = processParams.ProcFlags;
            pImage->RestoreInitialParameters(sFileName, processParams.ImageProcParams, procFlags,
                                             processParams.RotationParams.Rotation, processParams.Zoom, processParams.Offsets,
                                             CSize(processParams.TargetWidth, processParams.TargetHeight), processParams.MonitorSize);
        }
        qDebug() << "CImageProvider::RequestImage() found in cache: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    pRequest->InUse = true;
    pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;

    if (pRequest->OutOfMemory) {
        bWasOutOfMemory = true;
        if (FreeAllPossibleMemory()) {
            DeleteElement(pRequest);
            pRequest = StartRequestAndWaitUntilReady(sFileName, nFrameIndex, processParams);
            pRequest->InUse = true;
            pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;
        }
    }

    // Erst wenn das aktuelle Request sicher fertig (Ready) ist, das Read-Ahead-Bundle starten
    if (eDirection == FORWARD || eDirection == BACKWARD) {
        if (!bWasOutOfMemory) {
            StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumBuffers - 1, pRequest);
        }
        RemoveUnusedImages(false);
    } else {
        RemoveUnusedImages(true);
    }

    qDebug() << "CImageProvider::RequestImage() m_requestList.size:" << m_requestList.size();

    bOutOfMemory = pRequest->OutOfMemory;
    bExceptionError = pRequest->ExceptionError;
    return pRequest->Image;
}

void CImageProvider::NotifyNotUsed(CBaseImage* pImage) {
    if (!pImage) return;

    for (const auto& request : m_requestList) {
        // Nur prüfen, wenn der Request bereits fertig ist (Ready)
        if (request->Ready && request->Image == pImage) {
            request->InUse = false;
            request->IsActive = false;
            return;
        }
    }
    delete pImage;
}

void CImageProvider::ClearAllRequests() {
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        const auto& pRequest = *iter;
        if (pRequest->InUse) {
            ++iter;
        } else {
            iter = DeleteElementAt(iter);
        }
    }
}

bool CImageProvider::FreeAllPossibleMemory() {
    bool bCouldFreeMemory = false;
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        const auto& pRequest = *iter;
        if (!pRequest->InUse && pRequest->Ready) {
            iter = DeleteElementAt(iter);
            bCouldFreeMemory = true;
        } else {
            ++iter;
        }
    }
    return bCouldFreeMemory;
}

bool CImageProvider::ClearRequest(CBaseImage* pImage, bool releaseLockedFile) {
    if (pImage == nullptr) {
        return false;
    }
    bool bErased = false;
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); iter++) {
        const auto& req = *iter;
        if (req->Ready && req->Image == pImage) {
            if (releaseLockedFile) {
                m_pWorkThreads[0]->ReleaseFile(req->FileName);
            }
            DeleteElementAt(iter);
            bErased = true;
            break;
        }
    }
    return bErased;
}

CImageProvider::RequestPtr CImageProvider::FindRequest(const QString& sFileName, int nFrameIndex) {
    if (sFileName.isEmpty()) {
        return nullptr;
    }

    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); iter++) {
        const auto& pRequest = *iter;
        if (pRequest->FrameIndex == nFrameIndex && pRequest->FileName == sFileName) {
            if (pRequest->Ready && pRequest->Image == nullptr) {
                DeleteElementAt(iter);
                return nullptr;
            }
            return pRequest;
        }
    }
    return nullptr;
}

CImageProvider::RequestPtr CImageProvider::StartRequestAndWaitUntilReady(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams) {
    RequestPtr pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);

    QMutexLocker locker(&pRequest->Mutex);
    while (!pRequest->Ready) {
        pRequest->WaitCondition.wait(&pRequest->Mutex);
    }
    return pRequest;
}

void CImageProvider::StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection,
                                          const CProcessParams & processParams, int nNumRequests, RequestPtr pLastReadyRequest) {
    if (nNumRequests < 1 || pFileList == nullptr || pLastReadyRequest == nullptr) {
        return;
    }

    SetAllRequestsInactive();
    pLastReadyRequest->IsActive = true;

    if (CanReadAhead(pLastReadyRequest->Image) == false) {
        return;
    }

    int iDistance = 0;

    for (int i = 0; i < nNumRequests; i++) {
        if (iDistance < 1) {
            iDistance = -iDistance + 1;
        } else {
            iDistance = -iDistance;
        }

        eDirection = (iDistance > 0) ? FORWARD : BACKWARD;

        bool bSwitchImage = true;
        int nFrameIndex = Helpers::GetFrameIndex(pLastReadyRequest->Image, iDistance, true, bSwitchImage);

        if (bSwitchImage == false && CanReadAhead(pLastReadyRequest->Image) == false) {
            bSwitchImage = true;
            nFrameIndex = 0;
        }

        QString sFileName = bSwitchImage ? pFileList->PeekNextPrev((iDistance>0) ? iDistance : -iDistance, eDirection == FORWARD) : pFileList->Current();

        if (!sFileName.isEmpty()) {
            if (pLastReadyRequest->FileName == sFileName && pLastReadyRequest->FrameIndex == nFrameIndex) {
                continue;
            }

            RequestPtr pRequest = FindRequest(sFileName, nFrameIndex);
            if (pRequest == nullptr) {
                if (GetProcessingFlag(PFLAG_NoProcessingAfterLoad, processParams.ProcFlags)) {
                    CProcessParams paramsCopied = processParams;
                    paramsCopied.ProcFlags = SetProcessingFlag(paramsCopied.ProcFlags, PFLAG_NoProcessingAfterLoad, false);
                    StartNewRequest(sFileName, nFrameIndex, paramsCopied);
                } else {
                    StartNewRequest(sFileName, nFrameIndex, processParams);
                }
            } else {
                pRequest->IsActive = true;
                pRequest->AccessTimeStamp = m_nCurrentTimeStamp;
            }
        }
    }
}

CImageProvider::RequestPtr CImageProvider::StartNewRequest(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams) {
    qDebug() << "StartNewRequest() sFileName:" << sFileName << "nFrameIndex:" << nFrameIndex;
    auto pRequest = std::make_shared<CImageRequest>(sFileName, nFrameIndex);
    m_requestList.push_back(pRequest);
    pRequest->HandlingThread = SearchThreadForNewRequest();

    pRequest->Handle = pRequest->HandlingThread->AsyncLoad(
        pRequest->FileName,
        nFrameIndex,
        processParams,
        [pRequest](int handle) {
            QMutexLocker l(&pRequest->Mutex);
            if (pRequest->HandlingThread) {
                CImageData d = pRequest->HandlingThread->GetLoadedImage(handle);
                pRequest->Image = d.Image;
                pRequest->OutOfMemory = d.IsRequestFailedOutOfMemory;
                pRequest->ExceptionError = d.IsRequestFailedException;
            }
            pRequest->Ready = true;
            pRequest->WaitCondition.wakeAll();
        }
        );
    return pRequest;
}

CImageLoadThread* CImageProvider::SearchThreadForNewRequest(void) {
    for (int i = 0; i < m_nNumThread; i++) {
        bool bFree = true;
        CImageLoadThread* pThisThread = m_pWorkThreads[i];

        for (const auto& request : m_requestList) {
            if (request->HandlingThread == pThisThread && !request->Ready) {
                bFree = false;
                break;
            }
        }
        if (bFree) {
            return pThisThread;
        }
    }

    int nSmallestHandle = INT_MAX;
    CImageLoadThread* pOldestThread = nullptr;

    for (const auto& request : m_requestList) {
        if (request->HandlingThread != nullptr && !request->Ready && request->Handle < nSmallestHandle) {
            nSmallestHandle = request->Handle;
            pOldestThread = request->HandlingThread;
        }
    }

    return (pOldestThread == nullptr) ? m_pWorkThreads[0] : pOldestThread;
}

void CImageProvider::RemoveUnusedImages(bool bRemoveAlsoActiveRequests) {
    bool bRemoved = false;
    int nTimeStampToRemove = -2;
    do {
        bRemoved = false;
        int nSmallestTimeStamp = INT_MAX;
        std::list<RequestPtr>::iterator iter;
        for (iter = m_requestList.begin(); iter != m_requestList.end(); iter++) {
            const auto& pRequest = *iter;
            if (pRequest->InUse == false && pRequest->Ready && (pRequest->IsActive == false || bRemoveAlsoActiveRequests || IsDestructivelyProcessed(pRequest->Image))) {
                if (pRequest->AccessTimeStamp < nSmallestTimeStamp) {
                    nSmallestTimeStamp = pRequest->AccessTimeStamp;
                }
                if (pRequest->AccessTimeStamp == nTimeStampToRemove || IsDestructivelyProcessed(pRequest->Image)) {
                    iter = DeleteElementAt(iter);
                    bRemoved = true;
                    break;
                }
            }
        }
        nTimeStampToRemove = -2;
        const size_t nMaxListSize = std::max(0, m_nNumBuffers - (bRemoveAlsoActiveRequests ? 0 : 1));
        if (m_requestList.size() > nMaxListSize) {
            if (nSmallestTimeStamp < INT_MAX) {
                bRemoved = true;
                nTimeStampToRemove = nSmallestTimeStamp;
            }
        }
    } while (bRemoved);
}

void CImageProvider::SetAllRequestsInactive() {
    for (const auto& request : m_requestList) {
        request->IsActive = false;
    }
}

std::list<CImageProvider::RequestPtr>::iterator CImageProvider::DeleteElementAt(std::list<RequestPtr>::iterator iteratorAt) {
    return m_requestList.erase(iteratorAt);
}

void CImageProvider::DeleteElement(RequestPtr pRequest) {
    m_requestList.remove(pRequest);
}

bool CImageProvider::IsDestructivelyProcessed(CBaseImage* pImage) {
    return pImage != nullptr && pImage->IsDestructivelyProcessed();
}

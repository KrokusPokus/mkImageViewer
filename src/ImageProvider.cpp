#include "ImageProvider.h"
#include "BaseImage.h"
#include "ImageLoadThread.h"
#include "FileList.h"
#include "ProcessParams.h"

#include <climits>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QDebug>

namespace {

bool CanReadAhead(const CImagePosition& position) {
    bool isMultiFrame = position.FrameCount > 1;
    bool isAnimation = position.IsAnimation;
    bool bInArchive = position.InArchive;
    if (isMultiFrame) {
        if (isAnimation) {
            return false;
        } else if (bInArchive) {
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

CImageProvider::CImageProvider(QObject* pParent, int nNumBuffers)
    : QObject(pParent),
    m_pLoadThread(std::make_unique<CImageLoadThread>()),
    m_nNumBuffers(std::max(0, nNumBuffers)),
    m_nCurrentTimeStamp(0) {
}

CImageProvider::~CImageProvider(void) {
    // The load thread is terminated by its destructor (m_pLoadThread) before the requests are released
}

CImageLoadResult CImageProvider::RequestImage(CFileList* pFileList, EReadAheadDirection eDirection,
                                              const QString& sFileName, int nFrameIndex, const CProcessParams & processParams) {
    if (sFileName.isEmpty()) {
        CImageLoadResult result;
        result.Error = ELoadError::FileNotFound;
        return result;
    }

    RequestPtr pRequest = FindRequest(sFileName, nFrameIndex);
    bool bWasOutOfMemory = false;
    if (pRequest == nullptr) {
        //qDebug() << "CImageProvider::RequestImage() -> StartNewRequest() Frame" << nFrameIndex << "of" << sFileName;
        pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);
    } else {
        //qDebug() << "CImageProvider::RequestImage() Retrieving existing request: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    // Warten auf das Laden des Bildes via QWaitCondition
    if (!pRequest->Ready) {
        qDebug() << "CImageProvider::RequestImage() Start waiting for request completion: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
        QMutexLocker locker(&pRequest->Mutex);
        while (!pRequest->Ready) {
            pRequest->WaitCondition.wait(&pRequest->Mutex);
        }
        qDebug() << "CImageProvider::RequestImage() Finished waiting for request completion: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    } else {
        CBaseImage* pImage = pRequest->Image;
        if (pImage != nullptr) {
            pImage->RestoreInitialParameters(processParams.ImageProcParams, processParams.RotationParams.Rotation,
                                             processParams.Zoom, processParams.Offsets);
        }
        //qDebug() << "CImageProvider::RequestImage() found in cache: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    pRequest->InUse = true;
    pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;

    if (pRequest->Error == ELoadError::OutOfMemory) {
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

    //qDebug() << "CImageProvider::RequestImage() m_requestList.size:" << m_requestList.size();

    CImageLoadResult result;
    result.Image = pRequest->Image;
    result.Error = pRequest->Error;
    result.Position = pRequest->Position;
    result.Position.FileName = pRequest->FileName;
    return result;
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
                m_pLoadThread->ReleaseFile(req->FileName);
            }
            DeleteElementAt(iter);
            bErased = true;
            break;
        }
    }
    return bErased;
}

void CImageProvider::FileHasRenamed(const QString& sOldFileName, const QString& sNewFileName) {
    // Request names come from CFileList::Current(): clean absolute paths. Normalize the same way.
    const QString sOld = QDir::cleanPath(QFileInfo(sOldFileName).absoluteFilePath());
    const QString sNew = QDir::cleanPath(QFileInfo(sNewFileName).absoluteFilePath());
    if (sOld.isEmpty() || sNew.isEmpty() || sOld == sNew) {
        return;
    }

    // 1. A rename never overwrites, so the new name did not exist before and any request still
    //    registered under it is stale (file deleted earlier, or loaded before it vanished). It must go,
    //    otherwise FindRequest() would return the first match, the stale one, instead of the renamed image.
    //    Requests InUse are left alone: they belong to the image on screen and are owned by the caller.
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        if ((*iter)->FileName == sNew && !(*iter)->InUse) {
            iter = DeleteElementAt(iter);
        } else {
            ++iter;
        }
    }

    // 2. Continue all requests of the file (every frame index) under the new name. Linux file names are
    //    case-sensitive, so compare exactly (the Windows original used _tcsicmp).
    //    FileName is only touched on the GUI thread: CImageLoadThread::CRequest copies the name in its
    //    constructor. A load that is already running keeps using the old name and fails if the file is
    //    gone, in which case FindRequest() drops the empty result and the next request loads it again.
    for (const auto& pRequest : m_requestList) {
        if (pRequest->FileName == sOld) {
            pRequest->FileName = sNew;
        }
    }

    // 3. The load threads cache decoders and open files by NAME (m_sLastFileName, m_sLastZipFileName,
    //    m_pLastImageFile, ... for animations, archives and multi frame files). After the rename these
    //    entries are stale: they would only be found again if ANOTHER file is later renamed to the old name,
    //    and then frames of the wrong file would be decoded. So drop them. The decoder cache is not renamed
    //    (that would need a request type in CImageLoadThread); the price is that a multi frame file is reopened
    //    once on the next frame request.
    m_pLoadThread->ReleaseFile(sOld);
}

void CImageProvider::ReloadFile(const QString& sFileName) {
    if (sFileName.isEmpty()) {
        return;
    }
    // ReleaseFile() waits until the requests queued before it are processed: afterwards no request of the file is
    // still being decoded with the old state, and all of them are ready.
    m_pLoadThread->ReleaseFile(sFileName);
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        const auto& pRequest = *iter;
        if (pRequest->FileName == sFileName && !pRequest->InUse && pRequest->Ready) {
            iter = DeleteElementAt(iter);
        } else {
            ++iter;
        }
    }
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

    if (CanReadAhead(pLastReadyRequest->Position) == false) {
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
        int nFrameIndex = Helpers::GetFrameIndex(pLastReadyRequest->Position, pLastReadyRequest->Image != nullptr, iDistance, true, bSwitchImage);

        if (bSwitchImage == false && CanReadAhead(pLastReadyRequest->Position) == false) {
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
    //qDebug() << "StartNewRequest() sFileName:" << sFileName << "nFrameIndex:" << nFrameIndex;
    auto pRequest = std::make_shared<CImageRequest>(sFileName, nFrameIndex);
    m_requestList.push_back(pRequest);
    CImageLoadThread* pLoadThread = m_pLoadThread.get(); // lives as long as this provider

    pRequest->Handle = pLoadThread->AsyncLoad(
        pRequest->FileName,
        nFrameIndex,
        processParams,
        [pRequest, pLoadThread](int handle) {
            QMutexLocker l(&pRequest->Mutex);
            CImageData d = pLoadThread->GetLoadedImage(handle);
            pRequest->Image = d.Image;
            pRequest->Error = d.Error;
            pRequest->Position = d.Position;
            pRequest->Ready = true;
            pRequest->WaitCondition.wakeAll();
        }
        );
    return pRequest;
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

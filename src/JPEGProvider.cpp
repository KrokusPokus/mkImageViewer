#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "ImageLoadThread.h"
#include "FileList.h"
#include "ProcessParams.h"

#include <climits>

namespace {

    bool CanReadAhead(CJPEGImage* pImage) {
        bool isMultiFrame = pImage != nullptr && pImage->NumberOfFrames() > 1;
        bool isAnimation = pImage != nullptr && pImage->IsAnimation();
        bool isContainer = pImage != nullptr && pImage->IsContainer();
        int nFrameIndex = 0;
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


// ---------------------------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------------------------

CJPEGProvider::CJPEGProvider(HWND handlerWnd, int nNumThreads, int nNumBuffers)
    : m_hHandlerWnd(handlerWnd),
    m_nNumThread(std::max(1, nNumThreads)),
    m_nNumBuffers(std::max(0, nNumBuffers)) {
    m_nCurrentTimeStamp = 0;
    m_pWorkThreads = new CImageLoadThread*[nNumThreads];
    for (int i = 0; i < nNumThreads; i++) {
        m_pWorkThreads[i] = new CImageLoadThread();
    }
}

CJPEGProvider::~CJPEGProvider(void) {
    for (int i = 0; i < m_nNumThread; i++) {
        delete m_pWorkThreads[i];
    }
    delete[] m_pWorkThreads;
    for (CImageRequest* request : m_requestList) {
        delete request->Image;
        delete request;
    }
}

// ---------------------------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------------------------

CJPEGImage* CJPEGProvider::RequestImage(CFileList* pFileList, EReadAheadDirection eDirection,
                                        const QString& sFileName, int nFrameIndex, const CProcessParams & processParams,
                                        bool& bOutOfMemory, bool& bExceptionError) {
    if (sFileName.isEmpty()) {
        bOutOfMemory = false;
        bExceptionError = false;
        return nullptr;
    }

    /*
    Outline on how this is supposed to work:
        0. We assume m_nNumThread=1, since the ImageLoaders aren't thread safe and JxlReader/PngReaderer/WebpReaderWriter can't do out of order frame access
        1. Process and finish current request, assuming the default 
        2. Start read-ahead requests 50/50 in forward and backward direction.
        3. Remove anything from cache that's not
            a.) the currently requested image
            b.) a read-ahead request
            c.) in case the current image is multiframe, but not container - all frames of current image
    */

    // Search if we have the requested image already present or in progress
    CImageRequest* pRequest = FindRequest(sFileName, nFrameIndex);
    bool bWasOutOfMemory = false;
    if (pRequest == nullptr) {
/* Debugging */ qDebug() << "CJPEGProvider::RequestImage() -> StartNewRequest() Frame" << nFrameIndex << "of" << sFileName;
        // no request pending for this file, add to request queue and start async
        pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);
        // wait with read ahead when direction changed - maybe user just wants to re-see last image
        if (eDirection == FORWARD || eDirection == BACKWARD) {
            // start parallel if more than one thread
            StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumThread - 1, pRequest);
        }
    }
    else {
/* Debugging */ qDebug() << "CJPEGProvider::RequestImage() Retrieving existing request: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    // wait for request if not yet ready
    if (!pRequest->Ready) {
/* Debugging */ qDebug() << "CJPEGProvider::RequestImage() waiting for image to finish loading from disk: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
        ::WaitForSingleObject(pRequest->EventFinished, INFINITE);
        GetLoadedImageFromWorkThread(pRequest);
    } else {
        CJPEGImage* pImage = pRequest->Image;
        if (pImage != nullptr) {
            // make sure the initial parameters are reset as when keep params was on before they are wrong
            EProcessingFlags procFlags = processParams.ProcFlags;
            pImage->RestoreInitialParameters(sFileName, processParams.ImageProcParams, procFlags,
                processParams.RotationParams.Rotation, processParams.Zoom, processParams.Offsets, 
                CSize(processParams.TargetWidth, processParams.TargetHeight), processParams.MonitorSize);
        }
/* Debugging */ qDebug() << "CJPEGProvider::RequestImage() found in cache: Frame" << pRequest->FrameIndex << "of" << pRequest->FileName;
    }

    // set before removing unused images!
    pRequest->InUse = true;
    pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;

    if (pRequest->OutOfMemory) {
        // The request could not be satisfied because the system is out of memory.
        // Clear all memory and try again - maybe some readahead requests can be deleted
        bWasOutOfMemory = true;
        if (FreeAllPossibleMemory()) {
            DeleteElement(pRequest);
            pRequest = StartRequestAndWaitUntilReady(sFileName, nFrameIndex, processParams);
            pRequest->InUse = true;
            pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;
        }
    }

    // Check if we shall start new requests. (But don't start another request if we are short of memory.)
    if (eDirection == FORWARD || eDirection == BACKWARD) {
        if (!bWasOutOfMemory) {
            StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumBuffers - 1, pRequest);
        }
        RemoveUnusedImages(false);
    }

    bOutOfMemory = pRequest->OutOfMemory;
    bExceptionError = pRequest->ExceptionError;
    return pRequest->Image;
}

void CJPEGProvider::NotifyNotUsed(CJPEGImage* pImage) {
    if (!pImage) return;

    // mark image as unused but do not remove yet from request queue
    for (CImageRequest* request : m_requestList) {
        if (request->Image == pImage) {
            request->InUse = false;
            request->IsActive = false;
            return;
        }
    }
    // image not found in request queue - delete it as it is no longer used and will not be cached
    if (pImage != nullptr) delete pImage;
}

void CJPEGProvider::ClearAllRequests() {
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        CImageRequest* pRequest = *iter;
        if (pRequest->InUse) {
            if (!pRequest->Ready) {
                pRequest->Deleted = true; // Markieren für späteres Löschen nach Abschluss
            }
            ++iter;
        } else {
            iter = DeleteElementAt(iter);
        }
    }
}

bool CJPEGProvider::FreeAllPossibleMemory() {
    bool bCouldFreeMemory = false;
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ) {
        CImageRequest* pRequest = *iter;
        if (!pRequest->InUse && pRequest->Ready) {
            iter = DeleteElementAt(iter); // Deletes element and sets Iterator to the following one
            bCouldFreeMemory = true;
        } else {
            ++iter;
        }
    }
    return bCouldFreeMemory;
}

// Tells the provider that a file has been renamed externally so that pending requests to read this file can be updated.
// Used by the internal rename capability inside ImageProcPanel
//     CString sCurFileName(sCurrentFileName);
//     m_pMainDlg->GetFileList()->FileHasRenamed(sCurFileName, sNewFileName);
//     m_pMainDlg->GetJPEGProvider()->FileHasRenamed(sCurFileName, sNewFileName);
/*
void CJPEGProvider::FileHasRenamed(const QString& oldFileName, const QString& newFileName) {
    for (auto* request : m_requestList) {
        if (oldFileName == request->FileName) {
            request->FileName = newFileName;
        }
    }
}
*/

// Only called from the main GUI when navigating away from an image
bool CJPEGProvider::ClearRequest(CJPEGImage* pImage, bool releaseLockedFile) {
    if (pImage == nullptr) {
        return false;
    }
    bool bErased = false;
    std::list<CImageRequest*>::iterator iter;
    for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
        if ((*iter)->Image == pImage) {
            if (releaseLockedFile) {
                m_pWorkThreads[0]->ReleaseFile((*iter)->FileName);
            }
            // images that are not ready cannot be removed yet
            if ((*iter)->Ready) {
                DeleteElementAt(iter);
                bErased = true;
            } else {
                (*iter)->Deleted = true;
            }

            break;
        }
    }
    return bErased;
}

void CJPEGProvider::OnImageLoadCompleted(int nHandle) {
    for (auto iter = m_requestList.begin(); iter != m_requestList.end(); ++iter) {
        CImageRequest* request = *iter;
        if (request->Handle == nHandle) {
            GetLoadedImageFromWorkThread(request);

            // KORREKTUR: Wenn der Request während des Ladens zum Löschen markiert wurde,
            // wird er jetzt beim Abschluss direkt sauber aufgeräumt.
            if (request->Deleted) {
                DeleteElementAt(iter);
            }
            break;
        }
    }
}

CJPEGProvider::CImageRequest* CJPEGProvider::FindRequest(const QString& sFileName, int nFrameIndex) {
    if (sFileName.isEmpty()) {
        return nullptr;
    }

//* Debugging */    TCHAR debugtext[1024];

    std::list<CImageRequest*>::iterator iter;
    for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
        if (!(*iter)->Deleted
            && (*iter)->FrameIndex == nFrameIndex
            && (*iter)->FileName == sFileName) {

//* Debugging */    swprintf(debugtext, 1024, TEXT("FindRequest(%s, %d) Ready=%d  ExceptionError=%d  OutOfMemory=%d"), sFileName.c_str(), nFrameIndex, (*iter)->Ready, (*iter)->ExceptionError, (*iter)->OutOfMemory);
//* Debugging */    ::OutputDebugStringW(debugtext);

            // If request is ready, but no image could be loaded, purge from cache so we can try reloading
            if ((*iter)->Ready && (*iter)->Image == nullptr) {

//* Debugging */    swprintf(debugtext, 1024, TEXT("FindRequest will delete iter for %s"), (*iter)->FileName.c_str());
//* Debugging */    ::OutputDebugStringW(debugtext);

                DeleteElementAt(iter);
                return nullptr; 
            }
            
            return *iter;
        }
    }
    return nullptr;
}

CJPEGProvider::CImageRequest* CJPEGProvider::StartRequestAndWaitUntilReady(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams) {
    CImageRequest* pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);
    ::WaitForSingleObject(pRequest->EventFinished, INFINITE);
    GetLoadedImageFromWorkThread(pRequest);
    return pRequest;
}

void CJPEGProvider::StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection, 
                                          const CProcessParams & processParams, int nNumRequests, CImageRequest* pLastReadyRequest) {
    if (nNumRequests < 1 || pFileList == nullptr) {
        return;
    }

    // Set all reqests inactive for now
    // The caching relevant requests will be set active again in StartNewRequestBundle().
    SetAllRequestsInactive();
    if (pLastReadyRequest != nullptr) {
        pLastReadyRequest->IsActive = true;    // Set current request back to active.
    }

    // Stop here if pLastReadyRequest is an animation. We won't bother reading ahead since it might interfere with playback.
    if (pLastReadyRequest != nullptr && CanReadAhead(pLastReadyRequest->Image) == false) {
        return;
    }

    int iDistance = 0;

    for (int i = 0; i < nNumRequests; i++) {
        // This will produce 1, -1, 2, -2, etc. until we run out of nNumRequests
        if (iDistance < 1) {
            iDistance = -iDistance + 1;
        } else {
            iDistance = -iDistance;
        }

        eDirection = (iDistance > 0) ? FORWARD : BACKWARD;

        bool bSwitchImage = true;
        int nFrameIndex = (pLastReadyRequest != nullptr) ? Helpers::GetFrameIndex(pLastReadyRequest->Image, iDistance, true, bSwitchImage) : 0;

        // Note: Those readers do not support reading a specific frame
        // They only work with sequential access, where each call reads one further frame, which causes issues with bidirectional read-ahead.
        //        JxlReader::ReadImage()
        //        PngReader::ReadImage()
        //        WebpReaderWriter::ReadImage()
        // Edit: We return early before the loop, so we'll never get here for animations anyway. Kept in since we might want to change the logic some day.
        if (bSwitchImage == false && pLastReadyRequest != nullptr && CanReadAhead(pLastReadyRequest->Image) == false) {
            bSwitchImage = true;
            nFrameIndex = 0;
        }

        QString sFileName = bSwitchImage ? pFileList->PeekNextPrev((iDistance>0) ? iDistance : -iDistance, eDirection == FORWARD) : pFileList->Current();

        if (!sFileName.isEmpty()) {
            // When precaching, don't try to cache the pLastReadyRequest again.
            // The only situation where we would have to access the same file again is with
            if (pLastReadyRequest->FileName == sFileName && pLastReadyRequest->FrameIndex == nFrameIndex) {
                continue;
            }

            CImageRequest* pRequest = FindRequest(sFileName, nFrameIndex);
            if (pRequest == nullptr) {
                if (GetProcessingFlag(PFLAG_NoProcessingAfterLoad, processParams.ProcFlags)) {
                    // The read ahead threads need this flag to be deleted - we can speculatively process the image with good hit rate
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

CJPEGProvider::CImageRequest* CJPEGProvider::StartNewRequest(const QString& sFileName, int nFrameIndex, const CProcessParams & processParams) {
/* Debugging */    qDebug() << "StartNewRequest() sFileName:" << sFileName << "nFrameIndex:" << nFrameIndex;
    CImageRequest* pRequest = new CImageRequest(sFileName, nFrameIndex);
    m_requestList.push_back(pRequest);
    pRequest->HandlingThread = SearchThreadForNewRequest();
    pRequest->Handle = pRequest->HandlingThread->AsyncLoad(pRequest->FileName, nFrameIndex,
        processParams, m_hHandlerWnd, pRequest->EventFinished);
    return pRequest;
}

void CJPEGProvider::GetLoadedImageFromWorkThread(CImageRequest* pRequest) {
    if (pRequest->HandlingThread != nullptr) {
        CImageData imageData = pRequest->HandlingThread->GetLoadedImage(pRequest->Handle);
        pRequest->Image = imageData.Image;
        pRequest->OutOfMemory = imageData.IsRequestFailedOutOfMemory;
        pRequest->ExceptionError = imageData.IsRequestFailedException;
        pRequest->Ready = true;
        pRequest->HandlingThread = nullptr;
    }
}

CImageLoadThread* CJPEGProvider::SearchThreadForNewRequest(void) {
    // Any thread without a request in flight?
    for (int i = 0; i < m_nNumThread; i++) {
        bool bFree = true;
        CImageLoadThread* pThisThread = m_pWorkThreads[i];

        for (CImageRequest* request : m_requestList) {
            if (request->HandlingThread == pThisThread) {
                bFree = false;
                break;
            }
        }
        if (bFree) {
            return pThisThread;
        }
    }

    // all threads are occupied, return thread working on smallest handle (will finish earliest)
    int nSmallestHandle = INT_MAX;
    CImageLoadThread* pOldestThread = nullptr;

    for (CImageRequest* request : m_requestList) {
        if (request->HandlingThread != nullptr && request->Handle < nSmallestHandle) {
            nSmallestHandle = request->Handle;
            pOldestThread = request->HandlingThread;
        }
    }

    return (pOldestThread == nullptr) ? m_pWorkThreads[0] : pOldestThread;
}

void CJPEGProvider::RemoveUnusedImages(bool bRemoveAlsoActiveRequests) {
    bool bRemoved = false;
    int nTimeStampToRemove = -2;
    do {
        bRemoved = false;
        int nSmallestTimeStamp = INT_MAX;
        std::list<CImageRequest*>::iterator iter;
        for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
            if ((*iter)->InUse == false && (*iter)->Ready && ((*iter)->IsActive == false || bRemoveAlsoActiveRequests || IsDestructivelyProcessed((*iter)->Image))) {
                // search element with smallest timestamp
                if ((*iter)->AccessTimeStamp < nSmallestTimeStamp) {
                    nSmallestTimeStamp = (*iter)->AccessTimeStamp;
                }
                // remove the readahead images - if we get here with read ahead, the strategy was wrong and
                // the read ahead image is not used.
                if ((*iter)->AccessTimeStamp == nTimeStampToRemove || IsDestructivelyProcessed((*iter)->Image)) {
                    DeleteElementAt(iter);
                    bRemoved = true;
                    break;
                }
            }
        }
        nTimeStampToRemove = -2;
        // Make one buffer free for next readahead (except when bRemoveAlsoActiveRequests)
        const size_t nMaxListSize = std::max(0, m_nNumBuffers - (bRemoveAlsoActiveRequests ? 0 : 1));
        if (m_requestList.size() > nMaxListSize) {
            // remove element with smallest timestamp
            if (nSmallestTimeStamp < INT_MAX) {
                bRemoved = true;
                nTimeStampToRemove = nSmallestTimeStamp;
            }
        }
    } while (bRemoved); // repeat until no element could be removed anymore
}

void CJPEGProvider::SetAllRequestsInactive() {    
    for (CImageRequest* request : m_requestList) {
        request->IsActive = false;
    }
}

auto CJPEGProvider::DeleteElementAt(std::list<CImageRequest*>::iterator iteratorAt)
-> std::list<CJPEGProvider::CImageRequest*>::iterator {
    delete (*iteratorAt)->Image;
    delete *iteratorAt;
    return m_requestList.erase(iteratorAt); // Returns Iterator to next Element
}

void CJPEGProvider::DeleteElement(CImageRequest* pRequest) {
    // [From] https://github.com/aviscaerulea/jpegview-nt.git
    // "Remove from list first"
    m_requestList.remove(pRequest);
    delete pRequest->Image;
    delete pRequest;
}

bool CJPEGProvider::IsDestructivelyProcessed(CJPEGImage* pImage) {
    return pImage != nullptr && pImage->IsDestructivelyProcessed();
}

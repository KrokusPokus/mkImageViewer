#include "ProcessingThreadPool.h"
#include "ImageProcessingTypes.h"

#include <algorithm>

///////////////////////////////////////////////////////////////////////////////////
// Supporting classes
///////////////////////////////////////////////////////////////////////////////////

// A request wrapping another request and adding information about the image strip to process.
class CWrappedRequest : public CRequestBase {
public:
    // Anpassung an CCompletionEvent statt HANDLE
    CWrappedRequest(CProcessingRequest * pRequest, int nOffset, int nSize, std::shared_ptr<CCompletionEvent> eventFinished)
        : CRequestBase(std::move(eventFinished)) {
        InnerRequest = pRequest;
        Offset = nOffset;
        SizeY = nSize;
    }

    CProcessingRequest* InnerRequest;
    int Offset;
    int SizeY;
};

// Worker thread in thread pool, executing image processing operations on image strips
class CProcessingThread : public CWorkThread {
public:
    CProcessingThread(void) {}
    ~CProcessingThread(void) {}

    // Start a processing request on this thread asynchronously (nimmt nun shared_ptr)
    void StartProcess(std::shared_ptr<CWrappedRequest> pRequest) {
        ProcessAsync(pRequest);
    }

    // Processes a request synchronously on the calling thread
    static void DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY);

private:
    virtual void ProcessRequest(CRequestBase& request) override;
};

///////////////////////////////////////////////////////////////////////////////////
// CProcessingThreadPool
///////////////////////////////////////////////////////////////////////////////////

CProcessingThreadPool& CProcessingThreadPool::This() {
    // Thread-safe initialization (C++11). There is no destructor: the worker threads are not stopped at program
    // exit (as before), StopAllThreads() must be called explicitly if needed.
    static CProcessingThreadPool instance;
    return instance;
}

void CProcessingThreadPool::CreateThreadPoolThreads(int nNumCores) {
    m_nNumThreads = nNumCores - 1;
    if (m_nNumThreads > 0) {
        m_threads = new CProcessingThread*[m_nNumThreads];
        for (int i = 0; i < m_nNumThreads; i++) {
            m_threads[i] = new CProcessingThread();
        }
    } else {
        m_threads = NULL;
    }
}

void CProcessingThreadPool::StopAllThreads() {
    for (int i = 0; i < m_nNumThreads; i++) {
        m_threads[i]->Terminate();
        delete m_threads[i];
    }
    delete[] m_threads;
    m_nNumThreads = 0;
    m_threads = NULL;
}

bool CProcessingThreadPool::Process(CProcessingRequest* pRequest) {
    int nTargetCX = pRequest->ClippedTargetSize.cx;
    int nTargetCY = pRequest->ClippedTargetSize.cy;
    if (m_nNumThreads == 0) {
        CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
    } else {
        // Small jobs are processed on the calling thread. The work depends on the number of target pixels and,
        // for downsampling, on the number of source pixels involved (e.g. 24 MP to a 300x200 thumbnail).
        double dSourcePixels = 0;
        if (pRequest->FullTargetSize.cx > 0 && pRequest->FullTargetSize.cy > 0) {
            dSourcePixels = (double)pRequest->SourceSize.cx * nTargetCX / pRequest->FullTargetSize.cx *
                            pRequest->SourceSize.cy * nTargetCY / pRequest->FullTargetSize.cy;
        }
        double dWorkPixels = std::max((double)nTargetCX * nTargetCY, dSourcePixels);
        if (dWorkPixels < 100000 || nTargetCY < 2 * pRequest->StripPadding) {
            CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
        } else {
            // Distribute the lines in blocks of StripPadding lines as evenly as possible: each thread gets
            // nBlocks / nNumThreadsUsed blocks, the first (nBlocks % nNumThreadsUsed) threads one block more.
            // All slices except the last one (processed by the calling thread) are multiples of StripPadding.
            int nBlocks = (nTargetCY + pRequest->StripPadding - 1) / pRequest->StripPadding;
            int nNumThreadsUsed = std::min(m_nNumThreads + 1, nBlocks);
            assert(nNumThreadsUsed > 1);
            auto sliceCY = [&](int nThread) {
                return (nBlocks / nNumThreadsUsed + (nThread < nBlocks % nNumThreadsUsed ? 1 : 0)) * pRequest->StripPadding;
            };

            // Modernes atomares Counter-Objekt und CCompletionEvent statt Windows-Handles
            auto nRequestThreadCounter = std::make_shared<std::atomic<int>>(nNumThreadsUsed - 1);
            auto eventFinished = std::make_shared<CCompletionEvent>();

            int nCurrCY = 0;
            // Verwendung eines std::vector statt roher Arrays zur sicheren Speicherverwaltung
            std::vector<std::shared_ptr<CWrappedRequest>> allWrappedRequests;
            allWrappedRequests.reserve(nNumThreadsUsed - 1);

            for (int i = 0; i < nNumThreadsUsed - 1; i++) {
                auto wrappedReq = std::make_shared<CWrappedRequest>(pRequest, nCurrCY, sliceCY(i), eventFinished);
                wrappedReq->EventFinishedCounter = nRequestThreadCounter;
                allWrappedRequests.push_back(wrappedReq);
                m_threads[i]->StartProcess(wrappedReq);
                nCurrCY += sliceCY(i);
            }

            CProcessingThread::DoProcess(pRequest, nCurrCY, nTargetCY - nCurrCY);

            // Auf Beendigung warten via CCompletionEvent
            eventFinished->Wait();

            // Requests als gelöscht markieren, damit der Worker sie aus der Queue räumt
            for (auto& req : allWrappedRequests) {
                req->Deleted = true;
            }
        }
    }
    return pRequest->Success;
}

CProcessingThreadPool::CProcessingThreadPool(void) {
    m_threads = NULL;
    m_nNumThreads = 0;
}

void CProcessingThread::DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY) {
    if (!pRequest->ProcessSlice(nOffsetY, nSizeY)) {
        pRequest->Success = false;
    }
}

///////////////////////////////////////////////////////////////////////////////////
// CProcessingRequest
///////////////////////////////////////////////////////////////////////////////////

bool CProcessingRequest::ProcessSlice(int nOffsetY, int nSizeY) {
    const uint32 MAX_SRC_PIXELS_PER_STRIP = 1024 * 100;
    uint32 nNumberOfPixelsInSource = (uint32)((SourceSize.cx * (double)ClippedTargetSize.cx / FullTargetSize.cx) *
                                               (SourceSize.cy * (double)nSizeY / FullTargetSize.cy));
    uint32 nStrips = 1 + nNumberOfPixelsInSource / MAX_SRC_PIXELS_PER_STRIP;
    uint32 nStripHeight = nSizeY / nStrips;
    uint32 minimalStripHeight = std::min(16, StripPadding);

    if (nStrips >  1) {
        nStripHeight = nStripHeight & ~(StripPadding - 1);
        nStripHeight = std::min<uint32>(nSizeY, std::max(nStripHeight, minimalStripHeight));
    }
    int nSizeProcessed = 0;
    int nCurrentSizeY = nStripHeight;
    while (nSizeProcessed < nSizeY) {
        int nCurrentOffsetY = nOffsetY + nSizeProcessed;
        if (!ProcessStrip(nCurrentOffsetY, nCurrentSizeY)) {
            return false;
        }
        nSizeProcessed += nCurrentSizeY;
        nCurrentSizeY = std::min<uint32>(nStripHeight, nSizeY - nSizeProcessed);
    }
    return true;
}

void CProcessingThread::ProcessRequest(CRequestBase& request) {
    CWrappedRequest& wrappedRequest = static_cast<CWrappedRequest&>(request);
    DoProcess(wrappedRequest.InnerRequest, wrappedRequest.Offset, wrappedRequest.SizeY);
}

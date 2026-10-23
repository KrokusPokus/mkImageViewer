#include "ProcessingThreadPool.h"
#include "SettingsProvider.h"

CProcessingThreadPool* CProcessingThreadPool::sm_instance;

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
    if (sm_instance == NULL) {
        sm_instance = new CProcessingThreadPool();
    }
    return *sm_instance;
}

void CProcessingThreadPool::CreateThreadPoolThreads() {
    m_nNumThreads = CSettingsProvider::This().NumberOfCoresToUse() - 1;
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
        if (nTargetCX * nTargetCY < 100000 || nTargetCY < 2 * pRequest->StripPadding) {
            CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
        } else {
            int nNumThreadsUsed = m_nNumThreads + 1;
            int nSliceCY;
            while ((nSliceCY = ~(pRequest->StripPadding - 1) & (nTargetCY / nNumThreadsUsed)) < pRequest->StripPadding) {
                nNumThreadsUsed--;
            }
            assert(nNumThreadsUsed > 1);
            int nLastCY = nTargetCY - (nNumThreadsUsed - 1) * nSliceCY;

            // Modernes atomares Counter-Objekt und CCompletionEvent statt Windows-Handles
            auto nRequestThreadCounter = std::make_shared<std::atomic<int>>(nNumThreadsUsed - 1);
            auto eventFinished = std::make_shared<CCompletionEvent>();

            int nCurrCY = 0;
            // Verwendung eines std::vector statt roher Arrays zur sicheren Speicherverwaltung
            std::vector<std::shared_ptr<CWrappedRequest>> allWrappedRequests;
            allWrappedRequests.reserve(nNumThreadsUsed - 1);

            for (int i = 0; i < nNumThreadsUsed - 1; i++) {
                auto wrappedReq = std::make_shared<CWrappedRequest>(pRequest, nCurrCY, nSliceCY, eventFinished);
                wrappedReq->EventFinishedCounter = nRequestThreadCounter;
                allWrappedRequests.push_back(wrappedReq);
                m_threads[i]->StartProcess(wrappedReq);
                nCurrCY += nSliceCY;
            }

            CProcessingThread::DoProcess(pRequest, nCurrCY, nLastCY);

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
    const uint32 MAX_SRC_PIXELS_PER_STRIP = 1024 * 100;
    uint32 nNumberOfPixelsInSource = (uint32)((pRequest->SourceSize.cx * (double)pRequest->ClippedTargetSize.cx / pRequest->FullTargetSize.cx) *
                                               (pRequest->SourceSize.cy * (double)nSizeY / pRequest->FullTargetSize.cy));
    uint32 nStrips = 1 + nNumberOfPixelsInSource / MAX_SRC_PIXELS_PER_STRIP;
    uint32 nStripHeight = nSizeY / nStrips;
    uint32 minimalStripHeight = std::min(16, pRequest->StripPadding);

    if (nStrips >  1) {
        nStripHeight = nStripHeight & ~(pRequest->StripPadding - 1);
        nStripHeight = std::min<uint32>(nSizeY, std::max(nStripHeight, minimalStripHeight));
    }
    int nSizeProcessed = 0;
    int nCurrentSizeY = nStripHeight;
    while (nSizeProcessed < nSizeY) {
        int nCurrentOffsetY = nOffsetY + nSizeProcessed;
        if (!pRequest->ProcessStrip(nCurrentOffsetY, nCurrentSizeY)) {
            pRequest->Success = false;
            break;
        }
        nSizeProcessed += nCurrentSizeY;
        nCurrentSizeY = std::min<uint32>(nStripHeight, nSizeY - nSizeProcessed);
    }
}

void CProcessingThread::ProcessRequest(CRequestBase& request) {
    CWrappedRequest& wrappedRequest = static_cast<CWrappedRequest&>(request);
    DoProcess(wrappedRequest.InnerRequest, wrappedRequest.Offset, wrappedRequest.SizeY);
}

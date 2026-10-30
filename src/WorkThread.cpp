#include "WorkThread.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CWorkThread::CWorkThread(bool bCoInitialize)
    : m_bCoInitialize(bCoInitialize) {
}

CWorkThread::~CWorkThread() {
    assert((!m_thread.joinable() || m_bTerminate.load())
           && "Derived classes of CWorkThread must call Terminate() first thing in their destructor");
    Terminate();
}

void CWorkThread::ProcessAsync(RequestPtr request) {
    if (!request) {
        throw std::invalid_argument("CWorkThread::ProcessAsync: null request");
    }

    std::vector<RequestPtr> discarded;  // destroyed after the lock is released
    std::vector<RequestPtr> toFinish;
    bool bQueued = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_bAccepting) {
            // Terminating: nobody will ever process this. Complete as cancelled so waiters wake up.
            CancelLocked(request, toFinish);
        } else {
            StartLocked();
            CollectDeletedLocked(discarded, toFinish);  // keeps the queue from growing
            m_requestList.push_back(std::move(request));
            bQueued = true;
        }
    }
    FinishCancelled(toFinish);
    if (bQueued) {
        m_cv.notify_one();  // queued under m_mutex and tested under m_mutex: no lost wake-up
    }
}

bool CWorkThread::ProcessAndWait(RequestPtr request) {
    if (!request) {
        throw std::invalid_argument("CWorkThread::ProcessAndWait: null request");
    }
    if (std::this_thread::get_id() == m_workerId.load()) {
        throw std::logic_error("CWorkThread::ProcessAndWait called from the worker thread (would deadlock)");
    }
    if (!request->EventFinished) {
        request->EventFinished = std::make_shared<CCompletionEvent>();
    }
    const std::shared_ptr<CCompletionEvent> event = request->EventFinished;

    ProcessAsync(request);
    event->Wait();

    RemoveRequest(request);     // the caller keeps its own reference
    if (request->Error) {
        std::rethrow_exception(request->Error);
    }
    return !request->Cancelled;
}

void CWorkThread::RemoveRequest(const RequestPtr& request) {
    if (!request) {
        return;
    }

    std::vector<RequestPtr> toFinish;
    RequestPtr removed;  // destroyed outside the lock
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        request->Deleted = true;
        const auto it = std::find(m_requestList.begin(), m_requestList.end(), request);
        if (it == m_requestList.end()) {
            return;
        }
        if (it->get() != m_pCurrent) {
            CancelLocked(*it, toFinish);    // no-op if already processed
        }
        removed = std::move(*it);
        m_requestList.erase(it);
    }
    FinishCancelled(toFinish);  // Deleted is set: OnCancelled() is not called
}

void CWorkThread::Terminate() {
    RequestStop();

    if (m_workerId.load() == std::this_thread::get_id()) {
        // Called from the worker itself (e.g. from a callback): it cannot join itself. The worker leaves
        // its loop when the current callback returns; the join happens in the destructor.
        CancelPendingRequests();
        return;
    }

    {
        std::lock_guard<std::mutex> joinLock(m_joinMutex);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    // Worker is gone (or never started): release everything that is still waiting.
    CancelPendingRequests();
}

void CWorkThread::Abort() {
    RequestStop();
    CancelPendingRequests();
}

std::vector<CWorkThread::RequestPtr> CWorkThread::CancelUnstartedRequests(const std::function<bool(const CRequestBase&)>& pred) {
    std::vector<RequestPtr> cancelled;
    std::vector<RequestPtr> toFinish;
    {
        // The worker takes the next request under the same lock: a request is either running (skipped)
        // or will never be started.
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_requestList.begin(); it != m_requestList.end(); ) {
            const RequestPtr& r = *it;
            if (r.get() != m_pCurrent && !r->Processed && pred(*r)) {
                r->Deleted = true;              // cancelled by the owner: no OnCancelled()
                CancelLocked(r, toFinish);
                cancelled.push_back(r);
                it = m_requestList.erase(it);
            } else {
                ++it;
            }
        }
    }
    FinishCancelled(toFinish);  // signals waiters (EventFinished); OnCancelled() is skipped because of Deleted
    return cancelled;
}

CWorkThread::RequestPtr CWorkThread::RequestAbortRunning(const std::function<bool(const CRequestBase&)>& pred) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pCurrent == nullptr) {
        return nullptr;
    }
    // The running request stays in the list until it is finished, so it can be found there
    for (const RequestPtr& r : m_requestList) {
        if (r.get() == m_pCurrent) {
            if (!r->Processed && pred(*r)) {
                r->AbortRequested = true;
                return r;
            }
            break;
        }
    }
    return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

void CWorkThread::StartLocked() {
    if (m_bStarted) {
        return;
    }
    // The worker blocks on m_mutex until the caller releases it, so the members below are set before it runs.
    m_thread = std::jthread([this](std::stop_token stop) { ThreadMain(std::move(stop)); });
    m_stop = m_thread.get_stop_source();
    m_workerId.store(m_thread.get_id());
    m_bStarted = true;
}

void CWorkThread::ThreadMain(std::stop_token stop) {

    // Declared before the lock: destroyed after it, i.e. requests die outside the lock.
    std::vector<RequestPtr> discarded;
    std::vector<RequestPtr> toFinish;

    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        // The predicate is evaluated under m_mutex, requests are queued under m_mutex: no lost wake-up.
        m_cv.wait(lock, stop, [this] { return HasUnprocessedLocked(); });
        if (stop.stop_requested()) {
            break;
        }

        CollectDeletedLocked(discarded, toFinish);
        RequestPtr request = FirstUnprocessedLocked();
        m_pCurrent = request.get();
        lock.unlock();

        FinishCancelled(toFinish);
        discarded.clear();

        if (request) {
            try {
                ProcessRequest(*request);
            } catch (...) {
                request->Error = std::current_exception();  // rethrown by ProcessAndWait()
            }
            request->Processed = true;
            SignalFinished(*request);
            try {
                // Also during shutdown: Terminate() joins before the derived members die, so this is safe.
                AfterFinishProcess(*request);
            } catch (...) {
                // already completed and signalled, nobody left to report this to
            }
        }

        lock.lock();
        m_pCurrent = nullptr;
        if (request) {
            // Keep the request alive until after m_pCurrent is cleared (its address must not be reused
            // while it is still compared against), but destroy it outside the lock.
            discarded.push_back(std::move(request));
        }
    }
}

void CWorkThread::RequestStop() {
    std::stop_source stop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_bAccepting = false;   // from now on ProcessAsync() cancels instead of queueing
        stop = m_stop;
    }
    m_bTerminate.store(true);
    stop.request_stop();        // also wakes the worker from the condition variable (no-op if never started)
}

void CWorkThread::CancelPendingRequests() {
    std::vector<RequestPtr> toFinish;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& r : m_requestList) {
            if (r.get() != m_pCurrent) {    // the running request completes and signals by itself
                CancelLocked(r, toFinish);
            }
        }
    }
    FinishCancelled(toFinish);
}

bool CWorkThread::HasUnprocessedLocked() const {
    return std::any_of(m_requestList.begin(), m_requestList.end(),
                       [](const RequestPtr& r) { return !r->Processed; });
}

CWorkThread::RequestPtr CWorkThread::FirstUnprocessedLocked() const {
    for (const auto& r : m_requestList) {
        if (!r->Processed) {
            return r;
        }
    }
    return nullptr;
}

void CWorkThread::CancelLocked(const RequestPtr& request, std::vector<RequestPtr>& toFinish) {
    if (request->Processed) {
        return;     // already completed or cancelled: never signal twice
    }
    request->Cancelled = true;
    request->Processed = true;
    toFinish.push_back(request);
}

void CWorkThread::CollectDeletedLocked(std::vector<RequestPtr>& discarded, std::vector<RequestPtr>& toFinish) {
    for (auto it = m_requestList.begin(); it != m_requestList.end(); ) {
        if ((*it)->Deleted && it->get() != m_pCurrent) {
            CancelLocked(*it, toFinish);
            discarded.push_back(std::move(*it));
            it = m_requestList.erase(it);
        } else {
            ++it;
        }
    }
}

void CWorkThread::FinishCancelled(std::vector<RequestPtr>& toFinish) {
    for (const auto& r : toFinish) {
        SignalFinished(*r);
        if (!r->Deleted) {
            try {
                OnCancelled(*r);
            } catch (...) {
            }
        }
    }
    toFinish.clear();
}

void CWorkThread::SignalFinished(CRequestBase& request) {
    if (!request.EventFinished) {
        return;
    }
    if (!request.EventFinishedCounter || request.EventFinishedCounter->fetch_sub(1) - 1 <= 0) {
        request.EventFinished->Set();
    }
}

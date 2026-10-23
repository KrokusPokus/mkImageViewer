#ifndef WORKTHREAD_H
#define WORKTHREAD_H

// Portable (C++20, no Qt, no Win32) single-worker request queue.
//
// Usage contract:
//  * Derive, implement ProcessRequest() (and optionally AfterFinishProcess()/OnCancelled()).
//  * The derived destructor MUST call Terminate() as its first statement. Otherwise the worker could
//    call virtual functions on an already destroyed derived object. Debug builds assert this.
//  * Requests are created with std::make_shared and are owned by shared_ptr: whoever still holds
//    a reference (queue, worker, caller, a lambda) keeps the request alive.
//  * The worker thread is started lazily on the first ProcessAsync()/ProcessAndWait(), i.e. always
//    after the derived constructor has finished. No explicit Start() is needed.

#include <atomic>
#include <condition_variable>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

// Manual-reset event (replacement for Win32 CreateEvent(TRUE, FALSE)).
class CCompletionEvent {
public:
    void Set() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_bSet = true;
        }
        m_cv.notify_all();
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_bSet; });
    }
    bool IsSet() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_bSet;
    }
private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_bSet = false;
};

class CRequestBase {
public:
    CRequestBase() = default;
    explicit CRequestBase(std::shared_ptr<CCompletionEvent> eventFinished)
        : EventFinished(std::move(eventFinished)) {}
    virtual ~CRequestBase() = default;
    CRequestBase(const CRequestBase&) = delete;
    CRequestBase& operator=(const CRequestBase&) = delete;

    int Type = 0;                                              // for derived classes to tell request kinds apart
    std::shared_ptr<CCompletionEvent> EventFinished;           // optional
    std::shared_ptr<std::atomic<int>> EventFinishedCounter;    // optional: event is set when the counter reaches 0
    std::atomic<bool> Processed{false};                        // finished (successfully, failed or cancelled)
    std::atomic<bool> Deleted{false};                          // owner is no longer interested in the request
    std::atomic<bool> Cancelled{false};                        // was never run (shutdown / removed before start)
    std::exception_ptr Error;                                  // exception thrown by ProcessRequest(), if any
};

class CWorkThread {
public:
    using RequestPtr = std::shared_ptr<CRequestBase>;

    explicit CWorkThread(bool bCoInitialize = false);
    virtual ~CWorkThread();
    CWorkThread(const CWorkThread&) = delete;
    CWorkThread& operator=(const CWorkThread&) = delete;

    // Queues the request. Returns immediately. After Terminate()/Abort() the request is completed as cancelled.
    void ProcessAsync(RequestPtr request);

    // Queues the request and blocks until it is finished. Must not be called from the worker thread.
    // Returns false if the request was cancelled. Rethrows an exception thrown by ProcessRequest().
    bool ProcessAndWait(RequestPtr request);

    // Removes a request from the queue. A not yet started request is cancelled, a running one completes.
    void RemoveRequest(const RequestPtr& request);

    // Stops the worker and waits for it (blocks while a request is running; there is no kill). Idempotent.
    // Pending requests are completed as cancelled. Callable from the worker itself (then without join).
    void Terminate();

    // Like Terminate(), but does not wait for the worker (it is joined later in the destructor).
    void Abort();

protected:
    // Runs on the worker thread.
    virtual void ProcessRequest(CRequestBase& request) = 0;

    // Runs on the worker thread after the request has been marked Processed and its event was signalled.
    virtual void AfterFinishProcess(CRequestBase&) {}

    // Called (on any thread, without internal locks held) for a request that will never run, unless the
    // owner already marked it Deleted. Meant to wake up whoever waits for it, e.g. call the completion callback.
    virtual void OnCancelled(CRequestBase&) {}

    // Runs fn(const std::list<RequestPtr>&) under the queue lock and returns its result.
    // fn must not call back into CWorkThread.
    template <class Fn>
    auto WithRequestList(Fn&& fn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return fn(static_cast<const std::list<RequestPtr>&>(m_requestList));
    }

private:
    void ThreadMain(std::stop_token stop);
    void StartLocked();
    void RequestStop();
    void CancelPendingRequests();
    bool HasUnprocessedLocked() const;
    RequestPtr FirstUnprocessedLocked() const;
    void CancelLocked(const RequestPtr& request, std::vector<RequestPtr>& toFinish);
    void CollectDeletedLocked(std::vector<RequestPtr>& discarded, std::vector<RequestPtr>& toFinish);
    void FinishCancelled(std::vector<RequestPtr>& toFinish);
    static void SignalFinished(CRequestBase& request);

    const bool m_bCoInitialize;

    std::mutex m_mutex;                          // guards everything below, except where noted
    std::condition_variable_any m_cv;
    std::list<RequestPtr> m_requestList;
    CRequestBase* m_pCurrent = nullptr;          // request being processed; identity only, never dereferenced
    bool m_bStarted = false;
    bool m_bAccepting = true;
    std::stop_source m_stop;

    std::atomic<bool> m_bTerminate{false};
    std::atomic<std::thread::id> m_workerId{std::thread::id{}};
    std::mutex m_joinMutex;                      // serialises concurrent Terminate() calls
    std::jthread m_thread;                       // declared last: destroyed (joined) first
};

#endif // WORKTHREAD_H

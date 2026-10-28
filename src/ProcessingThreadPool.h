#ifndef PROCESSINGTHREADPOOL_H
#define PROCESSINGTHREADPOOL_H

#include "WorkThread.h"
#include "WTLCompat.h"

class CProcessingThread;

// Request for performing an image processing operation parallel on all thread pool threads
class CProcessingRequest : public CRequestBase {
public:
    CProcessingRequest(const void* pSourcePixels, CSize sourceSize, void* pTargetPixels,
                       CSize fullTargetSize, CPoint fullTargetOffset, CSize clippedTargetSize) {
        SourcePixels = pSourcePixels;
        SourceSize = sourceSize;
        TargetPixels = pTargetPixels;
        FullTargetSize = fullTargetSize;
        FullTargetOffset = fullTargetOffset;
        ClippedTargetSize = clippedTargetSize;
        StripPadding = 4;
        Success = true;
    }

    // Process one slice of the image (rows offsetY to offsetY + sizeY - 1) on the calling thread. Each thread of
    // the pool processes one slice. The default implementation splits the slice into strips and calls
    // ProcessStrip() for each of them.
    virtual bool ProcessSlice(int offsetY, int sizeY);

    // Process one strip of the image, starting at row offsetY and having the specified y-size
    virtual bool ProcessStrip(int offsetY, int sizeY) = 0;

    CSize SourceSize;
    const void* SourcePixels;
    void* TargetPixels;
    CSize FullTargetSize;
    CPoint FullTargetOffset;
    CSize ClippedTargetSize;
    int StripPadding;
    bool Success;
};

class CProcessingThreadPool {
public:
    static CProcessingThreadPool& This();
    // nNumCores: total number of threads used for processing (worker threads + calling thread)
    void CreateThreadPoolThreads(int nNumCores);
    void StopAllThreads();
    bool Process(CProcessingRequest* pRequest);

private:
    CProcessingThread** m_threads;
    int m_nNumThreads;

    CProcessingThreadPool(void);
};

#endif // PROCESSINGTHREADPOOL_H

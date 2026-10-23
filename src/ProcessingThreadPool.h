#ifndef PROCESSINGTHREADPOOL_H
#define PROCESSINGTHREADPOOL_H

#include "WorkThread.h"

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
    void CreateThreadPoolThreads();
    void StopAllThreads();
    bool Process(CProcessingRequest* pRequest);

private:
    static CProcessingThreadPool* sm_instance;

    CProcessingThread** m_threads;
    int m_nNumThreads;

    CProcessingThreadPool(void);
};

#endif // PROCESSINGTHREADPOOL_H

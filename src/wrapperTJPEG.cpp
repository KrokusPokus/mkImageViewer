#include "wrapperTJPEG.h"

#include "ICCProfileTransform.h"
#include "MaxImageDef.h"

#include <cstddef>
#include <new>

void * TurboJpeg::ReadImage(int &width,
                       int &height,
                       int &nchannels,
                       TJSAMP &chromoSubsampling,
                       bool &outOfMemory,
                       const void *buffer,
                       std::size_t sizebytes)
{
    outOfMemory = false;
    width = height = 0;
    nchannels = 3;
    chromoSubsampling = TJSAMP_420;

    tjhandle hDecoder = tj3Init(TJINIT_DECOMPRESS);
    if (hDecoder == nullptr) {
        return nullptr;
    }

    unsigned char* pPixelData = nullptr;
    int nResult = tj3DecompressHeader(hDecoder, (unsigned char*)buffer, sizebytes);
    if (nResult == 0) {
        width = tj3Get(hDecoder, TJPARAM_JPEGWIDTH);
        height = tj3Get(hDecoder, TJPARAM_JPEGHEIGHT);
        chromoSubsampling = (TJSAMP)tj3Get(hDecoder, TJPARAM_SUBSAMP);
        if (width <= 0 || height <= 0) {
            // tj3Get() returns -1 on error, nothing to decode
        } else if ((double)width * height > MAX_IMAGE_PIXELS) {
            outOfMemory = true;
        } else if (width <= MAX_IMAGE_DIMENSION && height <= MAX_IMAGE_DIMENSION && chromoSubsampling != TJSAMP_UNKNOWN) {
            pPixelData = new(std::nothrow) unsigned char[TJPAD(static_cast<std::size_t>(width) * 3) * height];
            if (pPixelData != nullptr) {
                nResult = tj3Decompress8(hDecoder, (unsigned char*)buffer, sizebytes, pPixelData, TJPAD(width * 3), TJPF_BGR);
                if (nResult != 0) {
                    delete[] pPixelData;
                    pPixelData = nullptr;
                } else {
                    unsigned char *iccBuf = nullptr;
                    size_t iccSize = 0;
                    nResult = tj3GetICCProfile(hDecoder, &iccBuf, &iccSize);
                    if (nResult == 0) {
                        void* transform = ICCProfileTransform::CreateTransform(iccBuf, iccSize, ICCProfileTransform::FORMAT_BGR);
                        if (transform != nullptr) {
                            ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, width, height, TJPAD(width * 3));
                            ICCProfileTransform::DeleteTransform(transform);
                        }
                    tj3Free(iccBuf);
                    }
                }
            } else {
                outOfMemory = true;
            }
        }
    }

    tj3Destroy(hDecoder);

    return pPixelData;
}

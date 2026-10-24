#include "XMMImage.h"
#include "Helpers.h"
#include "LookupTables.h"    // needed for sRGB8_LinRGB12[256]

#include <new> // Für std::align_val_t

CXMMImage::CXMMImage(int nWidth, int nHeight, int padding) {
    Init(nWidth, nHeight, false, padding);
}

CXMMImage::CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding) {
    Init(nWidth, nHeight, bPadHeight, padding);
}

//GF: version for f32 SSE & AVX2
CXMMImage::CXMMImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, 
    const void* pDIB, int nChannels, int padding) {
    int nSectionWidth = nLastX - nFirstX + 1;
    int nSectionHeight = nLastY - nFirstY + 1;
    Init(nSectionWidth, nSectionHeight, false, padding);

    if (m_pMemory != NULL) {
        int nSrcLineWidthPadded = Helpers::DoPadding(nWidth * nChannels, 4);
        const uint8* pSrc = (uint8*)pDIB + (long long)nFirstY*(long long)nSrcLineWidthPadded + (long long)nFirstX*(long long)nChannels;
        float* pDst = (float*) m_pMemory;
        for (int j = 0; j < nSectionHeight; j++) {
            if (nChannels == 4) {
                for (int i = 0; i < nSectionWidth; i++) {
                    uint32 sourcePixel = ((uint32*)pSrc)[i];
                    int d = i;
                    uint32 nBlue = sourcePixel & 0xFF;
                    uint32 nGreen = (sourcePixel >> 8) & 0xFF;
                    uint32 nRed = (sourcePixel >> 16) & 0xFF;

                    pDst[d] = ((float)sRGB8_LinRGB12[nBlue]);
                    d += m_nPaddedWidth;
                    pDst[d] = ((float)sRGB8_LinRGB12[nGreen]);
                    d += m_nPaddedWidth;
                    pDst[d] = ((float)sRGB8_LinRGB12[nRed]);
                }
            } else {
                for (int i = 0; i < nSectionWidth; i++) {
                    int s = i*3;
                    int d = i;
                    pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s]]);
                    d += m_nPaddedWidth;
                    pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+1]]);
                    d += m_nPaddedWidth;
                    pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+2]]);
                }
            }
            pDst += 3*m_nPaddedWidth;
            pSrc += nSrcLineWidthPadded;
        }
    }
}

CXMMImage::~CXMMImage(void) {
    if (m_pMemory != NULL) {
        // free the memory pages
        //::VirtualFree(m_pMemory, 0, MEM_RELEASE);
        ::operator delete(m_pMemory, std::align_val_t(4096));
        m_pMemory = NULL;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////

void CXMMImage::Init(int nWidth, int nHeight, bool bPadHeight, int padding) {
    // pad scanlines
    m_nPaddedWidth = Helpers::DoPadding(nWidth, padding);
    if (bPadHeight) {
        m_nPaddedHeight = Helpers::DoPadding(nHeight, padding);
    } else {
        m_nPaddedHeight = nHeight;
    }
    m_nWidth = nWidth;
    m_nHeight = nHeight;
    int nMemSize = GetMemSize();

    /*
    // Allocate memory aligned on page boundaries
    m_pMemory = ::VirtualAlloc(
                        NULL,      // let the call determine the start address
                        nMemSize, // the size
                        MEM_RESERVE | MEM_COMMIT,    // I want that memory, now
                        PAGE_READWRITE);            // need both read and write
    */

    // Speicher seiten- bzw. block-aligned allokieren (z.B. auf 4096 Bytes / Page-Boundary wie bei VirtualAlloc)
    m_pMemory = static_cast<char*>(::operator new(nMemSize, std::align_val_t(4096)));
}

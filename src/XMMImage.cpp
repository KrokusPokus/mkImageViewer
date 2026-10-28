#include "XMMImage.h"
#include "Helpers.h"
#include "LookupTables.h"    // needed for sRGB8_LinRGB12[256]

#include <cassert>
#include <cstddef>
#include <cstring>
#include <immintrin.h>
#include <new> // Für std::align_val_t

namespace {

// Per-thread cache of scratch buffers for CXMMImage. The buffers only grow and are reused for all strips
// (and across resampling calls), avoiding an allocation per strip, which caused mmap/munmap, page faults,
// zeroing of fresh pages by the kernel and TLB shootdowns.
// A buffer must be released on the thread that acquired it (CXMMImage objects are local to one strip).
// Acquire() returns nullptr if memory is exhausted (no exception, CXMMImage::AlignedPtr() is then NULL).
class CScratchBuffers {
public:
    ~CScratchBuffers() {
        for (Slot& slot : m_slots) {
            ::operator delete(slot.pMemory, std::align_val_t(ALIGNMENT));
        }
    }

    void* Acquire(size_t nSize) {
        // smallest free buffer that is large enough
        Slot* pBest = nullptr;
        for (Slot& slot : m_slots) {
            if (!slot.bInUse && slot.nCapacity >= nSize && (pBest == nullptr || slot.nCapacity < pBest->nCapacity)) {
                pBest = &slot;
            }
        }
        if (pBest == nullptr) {
            // none large enough: grow the largest free buffer
            for (Slot& slot : m_slots) {
                if (!slot.bInUse && (pBest == nullptr || slot.nCapacity > pBest->nCapacity)) {
                    pBest = &slot;
                }
            }
            if (pBest == nullptr) {
                // all buffers in use (not expected), allocate without caching
                return ::operator new(nSize, std::align_val_t(ALIGNMENT), std::nothrow);
            }
            ::operator delete(pBest->pMemory, std::align_val_t(ALIGNMENT));
            pBest->pMemory = nullptr;
            pBest->nCapacity = 0;
            size_t nCapacity = (nSize + GRANULARITY - 1) & ~(GRANULARITY - 1);
            pBest->pMemory = ::operator new(nCapacity, std::align_val_t(ALIGNMENT), std::nothrow);
            if (pBest->pMemory == nullptr) {
                return nullptr;
            }
            pBest->nCapacity = nCapacity;
        }
        pBest->bInUse = true;
        return pBest->pMemory;
    }

    void Release(void* pMemory) {
        for (Slot& slot : m_slots) {
            if (slot.pMemory == pMemory) {
                slot.bInUse = false;
                if (slot.nCapacity > MAX_RETAINED_SIZE) {
                    // do not keep very large buffers alive while idle
                    ::operator delete(slot.pMemory, std::align_val_t(ALIGNMENT));
                    slot.pMemory = nullptr;
                    slot.nCapacity = 0;
                }
                return;
            }
        }
        ::operator delete(pMemory, std::align_val_t(ALIGNMENT));
    }

private:
    static constexpr size_t ALIGNMENT = 4096;
    static constexpr size_t GRANULARITY = 64 * 1024;
    static constexpr size_t MAX_RETAINED_SIZE = 32 * 1024 * 1024;
    static constexpr int NUM_SLOTS = 4; // at most two CXMMImage objects are alive at the same time per strip

    struct Slot {
        void* pMemory = nullptr;
        size_t nCapacity = 0;
        bool bInUse = false;
    };
    Slot m_slots[NUM_SLOTS];
};

thread_local CScratchBuffers t_scratchBuffers;

// sRGB8_LinRGB12 as float, for lookups with _mm256_i32gather_ps
struct CLinRGB12FloatLUT {
    float Values[256];
    CLinRGB12FloatLUT() {
        for (int i = 0; i < 256; i++) {
            Values[i] = (float)sRGB8_LinRGB12[i];
        }
    }
};
const CLinRGB12FloatLUT s_sRGB8_LinRGB12_Float;

}

CXMMImage::CXMMImage(int nWidth, int nHeight, int padding) {
    Init(nWidth, nHeight, false, padding);
}

CXMMImage::CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding) {
    Init(nWidth, nHeight, bPadHeight, padding);
}

CXMMImage::CXMMImage(int nWidth, int nHeight, bool bPadHeight, int padding, bool bAvoidCacheAliasing) {
    Init(nWidth, nHeight, bPadHeight, padding, bAvoidCacheAliasing);
}

//GF: version for f32 SSE & AVX2
CXMMImage::CXMMImage(int nWidth, int nHeight, int nFirstX, int nLastX, int nFirstY, int nLastY, 
    const void* pDIB, int nChannels, int padding) {
    int nSectionWidth = nLastX - nFirstX + 1;
    int nSectionHeight = nLastY - nFirstY + 1;
    Init(nSectionWidth, nSectionHeight, false, padding, true);

    if (m_pMemory != NULL) {
        int nSrcLineWidthPadded = Helpers::DoPadding(nWidth * nChannels, 4);
        const uint8* pSrc = (uint8*)pDIB + (long long)nFirstY*(long long)nSrcLineWidthPadded + (long long)nFirstX*(long long)nChannels;
        for (int j = 0; j < nSectionHeight; j++) {
            ConvertLine(j, pSrc, nChannels, padding);
            pSrc += nSrcLineWidthPadded;
        }
    }
}

void CXMMImage::ConvertLine(int nLine, const uint8* pSrc, int nChannels, int padding) {
    float* pDst = GetLine(nLine);
    ConvertPixels(pDst, pSrc, nChannels, 0, m_nWidth);
    ZeroPaddingColumns(pDst, padding);
}

void CXMMImage::ConvertLine_AVX(int nLine, const uint8* pSrc, int nChannels, int padding) {
    const float* pLUT = s_sRGB8_LinRGB12_Float.Values;
    float* pDst0 = GetLine(nLine);
    float* pDst1 = pDst0 + m_nPaddedWidth;
    float* pDst2 = pDst1 + m_nPaddedWidth;
    int i = 0;
    if (nChannels == 4) {
        const __m256i ymmMask = _mm256_set1_epi32(0xFF);
        for (; i + 8 <= m_nWidth; i += 8) {
            __m256i ymmPixels = _mm256_loadu_si256((const __m256i*)(pSrc + i * 4));
            __m256i ymmIdx0 = _mm256_and_si256(ymmPixels, ymmMask);
            __m256i ymmIdx1 = _mm256_and_si256(_mm256_srli_epi32(ymmPixels, 8), ymmMask);
            __m256i ymmIdx2 = _mm256_and_si256(_mm256_srli_epi32(ymmPixels, 16), ymmMask);
            _mm256_storeu_ps(pDst0 + i, _mm256_i32gather_ps(pLUT, ymmIdx0, 4));
            _mm256_storeu_ps(pDst1 + i, _mm256_i32gather_ps(pLUT, ymmIdx1, 4));
            _mm256_storeu_ps(pDst2 + i, _mm256_i32gather_ps(pLUT, ymmIdx2, 4));
        }
    } else {
        // 8 pixels are 24 bytes: bytes 0..15 and 16..23 are loaded separately (no read beyond the 8 pixels),
        // the bytes of each channel are collected with pshufb (-1 gives zero) and combined
        const __m128i xmmLo0 = _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i xmmHi0 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, 5, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i xmmLo1 = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i xmmHi1 = _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i xmmLo2 = _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i xmmHi2 = _mm_setr_epi8(-1, -1, -1, -1, -1, 1, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1);
        for (; i + 8 <= m_nWidth; i += 8) {
            __m128i xmmLo = _mm_loadu_si128((const __m128i*)(pSrc + i * 3));
            __m128i xmmHi = _mm_loadl_epi64((const __m128i*)(pSrc + i * 3 + 16));
            __m256i ymmIdx0 = _mm256_cvtepu8_epi32(_mm_or_si128(_mm_shuffle_epi8(xmmLo, xmmLo0), _mm_shuffle_epi8(xmmHi, xmmHi0)));
            __m256i ymmIdx1 = _mm256_cvtepu8_epi32(_mm_or_si128(_mm_shuffle_epi8(xmmLo, xmmLo1), _mm_shuffle_epi8(xmmHi, xmmHi1)));
            __m256i ymmIdx2 = _mm256_cvtepu8_epi32(_mm_or_si128(_mm_shuffle_epi8(xmmLo, xmmLo2), _mm_shuffle_epi8(xmmHi, xmmHi2)));
            _mm256_storeu_ps(pDst0 + i, _mm256_i32gather_ps(pLUT, ymmIdx0, 4));
            _mm256_storeu_ps(pDst1 + i, _mm256_i32gather_ps(pLUT, ymmIdx1, 4));
            _mm256_storeu_ps(pDst2 + i, _mm256_i32gather_ps(pLUT, ymmIdx2, 4));
        }
    }
    ConvertPixels(pDst0, pSrc, nChannels, i, m_nWidth);
    ZeroPaddingColumns(pDst0, padding);
}

void CXMMImage::ConvertPixels(float* pDst, const uint8* pSrc, int nChannels, int nFirst, int nEnd) const {
    if (nChannels == 4) {
        for (int i = nFirst; i < nEnd; i++) {
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
        for (int i = nFirst; i < nEnd; i++) {
            int s = i*3;
            int d = i;
            pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s]]);
            d += m_nPaddedWidth;
            pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+1]]);
            d += m_nPaddedWidth;
            pDst[d] = ((float)sRGB8_LinRGB12[pSrc[s+2]]);
        }
    }
}

void CXMMImage::ZeroPaddingColumns(float* pDst, int padding) const {
    // zero the padding columns read by the filter (up to the next multiple of 'padding')
    int nPadColumns = Helpers::DoPadding(m_nWidth, padding) - m_nWidth;
    if (nPadColumns > 0) {
        for (int c = 0; c < 3; c++) {
            memset(pDst + c*m_nPaddedWidth + m_nWidth, 0, nPadColumns * sizeof(float));
        }
    }
}

CXMMImage::~CXMMImage(void) {
    if (m_pMemory != NULL) {
        t_scratchBuffers.Release(m_pMemory);
        m_pMemory = NULL;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////

void CXMMImage::Init(int nWidth, int nHeight, bool bPadHeight, int padding, bool bAvoidCacheAliasing) {
    // pad scanlines
    m_nPaddedWidth = Helpers::DoPadding(nWidth, padding);
    if (bAvoidCacheAliasing) {
        // The vertical filter reads the same column from many consecutive lines (3 channels x filter length).
        // If the line size in bytes is a multiple of 4096 (or a large power of two divisor of it), all these
        // accesses map to the same few L1/L2 cache sets and evict each other. With the line size being an odd
        // multiple of 64 bytes, up to 64 (L1) resp. 1024 (L2) consecutive lines map to different sets.
        // Short lines (below 1 KiB) are left alone: they spread over enough sets anyway, and the extra padding
        // would cost noticeably more memory traffic (e.g. strips of 8 lines after rotation).
        assert(16 % padding == 0);
        const int MIN_WIDTH_FOR_ALIASING = 1024 / sizeof(float);
        if (m_nPaddedWidth >= MIN_WIDTH_FOR_ALIASING) {
            m_nPaddedWidth = Helpers::DoPadding(nWidth, 16);
            if (((m_nPaddedWidth / 16) & 1) == 0) {
                m_nPaddedWidth += 16;
            }
        }
    }
    if (bPadHeight) {
        m_nPaddedHeight = Helpers::DoPadding(nHeight, padding);
    } else {
        m_nPaddedHeight = nHeight;
    }
    m_nWidth = nWidth;
    m_nHeight = nHeight;
    size_t nMemSize = (size_t)GetLineSize() * 3 * m_nPaddedHeight;

    /*
    // Allocate memory aligned on page boundaries
    m_pMemory = ::VirtualAlloc(
                        NULL,      // let the call determine the start address
                        nMemSize, // the size
                        MEM_RESERVE | MEM_COMMIT,    // I want that memory, now
                        PAGE_READWRITE);            // need both read and write
    */

    // page aligned memory, reused from the per-thread scratch buffers
    m_pMemory = t_scratchBuffers.Acquire(nMemSize);
}

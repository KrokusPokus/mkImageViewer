#include "ResizeFilter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <numbers>
#include <vector>

namespace {
// We cannot enlarge to a factor above the number of kernels without artifacts
constexpr int NUM_KERNELS_RESIZE = 128;
constexpr int NUM_KERNELS_RESIZE_LOG2 = 7;

constexpr double kPi = std::numbers::pi;
constexpr double kPiDiv2 = kPi / 2;
constexpr double kPiSqr = kPi * kPi;

//////////////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////////////

inline int16 roundToInt16(double d) {
    return static_cast<int16>((d < 0) ? (d - 0.5) : (d + 0.5));
}

// Normalization means: sum of filter elements equals CResizeFilter::FP_ONE
void NormalizeFilter(int16* pFilter, int nLen) {
    int nSum = 0;
    for (int i = 0; i < nLen; i++) {
        nSum += pFilter[i];
    }
    if (nSum > 0) {
        int nTotal = 0;
        for (int i = 0; i < nLen; i++) {
            pFilter[i] = static_cast<int16>((pFilter[i] * CResizeFilter::FP_ONE) / nSum);
            nTotal += pFilter[i];
        }
        pFilter[0] += static_cast<int16>(CResizeFilter::FP_ONE - nTotal);
    }
}

//////////////////////////////////////////////////////////////////////////////////////
// Filter kernel evaluation
//////////////////////////////////////////////////////////////////////////////////////

// Cubic Filter Family: Mitchell-Netravali filters (BC-splines)

// Hermite (B=0, C=0)
inline double EvaluateCore_Hermite(double dX) {
    const double ParamB = 0.0;
    const double ParamC = 0.0;

    if (std::abs(dX) < 1) {
        return (((12 - 9*ParamB - 6*ParamC) * std::abs(dX*dX*dX)) + ((-18 + 12*ParamB + 6*ParamC) * (dX*dX)) + (6 - 2*ParamB)) / 6.0;
    }
    // GF: Not needed. Hermite' has the smallest support ('1.0') of all the BC Cubic filters, and does not contain a negative lobe.
    //    else if (abs(dX) < 2)
    //        return (((-ParamB - 6*ParamC) * abs(dX*dX*dX)) + ((6*ParamB + 30*ParamC) * (dX*dX)) + (((-12 * ParamB) - (48 * ParamC)) * abs(dX)) + (8*ParamB + 24*ParamC)) / 6.0;
    return 0.0;
}

// Mitchell (B=1/3, C=1/3)
inline double EvaluateCore_Mitchell(double dX) {
    const double ParamB = 1.0/3.0;
    const double ParamC = 1.0/3.0;

    if (std::abs(dX) < 1) {
        return (((12 - 9*ParamB - 6*ParamC) * std::abs(dX*dX*dX)) + ((-18 + 12*ParamB + 6*ParamC) * (dX*dX)) + (6 - 2*ParamB)) / 6.0;
    } else if (std::abs(dX) < 2) {
        return (((-ParamB - 6*ParamC) * std::abs(dX*dX*dX)) + ((6*ParamB + 30*ParamC) * (dX*dX)) + (((-12 * ParamB) - (48 * ParamC)) * std::abs(dX)) + (8*ParamB + 24*ParamC)) / 6.0;
    }
    return 0.0;
}

// Catrom (B=0, C=0.5)
inline double EvaluateCore_Catrom(double dX) {
    const double ParamB = 0.0;
    const double ParamC = 0.5;

    if (std::abs(dX) < 1) {
        return (((12 - 9*ParamB - 6*ParamC) * std::abs(dX*dX*dX)) + ((-18 + 12*ParamB + 6*ParamC) * (dX*dX)) + (6 - 2*ParamB)) / 6.0;
    } else if (std::abs(dX) < 2) {
        return (((-ParamB - 6*ParamC) * std::abs(dX*dX*dX)) + ((6*ParamB + 30*ParamC) * (dX*dX)) + (((-12 * ParamB) - (48 * ParamC)) * std::abs(dX)) + (8*ParamB + 24*ParamC)) / 6.0;
    }
    return 0.0;
}

// 2-lobe Lanczos filter (Equivalent to IM Lanczos2, support is [-2.0, 2.0]
inline double EvaluateCore_Lanczos2(double dX) {
    if (std::abs(dX) < 1e-6) {
        return 1.0;
    } else if (std::abs(dX) < 2) {
        return (2 * std::sin(kPi*dX) * std::sin(kPiDiv2*dX)) / (kPiSqr*dX*dX);
    }
    return 0.0;
}

// Evaluate a filter kernel at position dX. The filter kernel is assumed to have zero solutions at
// integer values and is centered around zero.
inline double EvaluateKernel(double dX, EFilterType eFilter) {
    switch (eFilter) {
    case Filter_Downsampling_Hermite:
        return EvaluateCore_Hermite(dX);
    case Filter_Downsampling_Mitchell:
        return EvaluateCore_Mitchell(dX);
    case Filter_Downsampling_Catrom:
        return EvaluateCore_Catrom(dX);
    case Filter_Downsampling_Lanczos2:
        return EvaluateCore_Lanczos2(dX);
    default:
        break;
    }
    return 0.0;
}

// Evaluation of filter kernel using an integration over the source pixel width.
// This implements a convolution of a box filter with the filter kernel.
// Note that dX is given in the source pixel space
double EvaluateKernelIntegrated(double dX, EFilterType eFilter, double dMultX) {
    double dXScaled = dX*dMultX;

    // take integral of target function from [dX - 0.5, dX + 0.5]
    constexpr int NUM_STEPS = 32;
    double dStartX = dXScaled - dMultX*0.5;
    double dStepX = dMultX*(1.0 / (NUM_STEPS - 1));
    double dSum = 0.0;
    for (int i = 0; i < NUM_STEPS; i++) {
        dSum += EvaluateKernel(dStartX, eFilter);
        dStartX += dStepX;
    }
    return dSum;
}

double EvaluateCubicFilterKernel(double dFrac, int nKernelElement) {
    // [GF] The original version was using the equivalent of Catrom for upscaling,
    //      but calculated it with a cdFactorA=-0.5 instead of separate ParamB/ParamC.

    // Cubic Spline (emulates gaussian, but is faster)
    //const double ParamB = 1.0;
    //const double ParamC = 0.0;

    // Catrom
    const double ParamB = 0.0;
    const double ParamC = 0.5;

    double dAbsDiff;
    switch (nKernelElement) {
    case 0:
        dAbsDiff = 1.0 + dFrac;
        return (((-ParamB - 6*ParamC) * std::abs(dAbsDiff*dAbsDiff*dAbsDiff)) + ((6*ParamB + 30*ParamC) * (dAbsDiff*dAbsDiff)) + (((-12 * ParamB) - (48 * ParamC)) * std::abs(dAbsDiff)) + (8*ParamB + 24*ParamC)) / 6.0;
    case 1:
        dAbsDiff = dFrac;
        return (((12 - 9*ParamB - 6*ParamC) * std::abs(dAbsDiff*dAbsDiff*dAbsDiff)) + ((-18 + 12*ParamB + 6*ParamC) * (dAbsDiff*dAbsDiff)) + (6 - 2*ParamB)) / 6.0;
    case 2:
        dAbsDiff = (1.0 - dFrac);
        return (((12 - 9*ParamB - 6*ParamC) * std::abs(dAbsDiff*dAbsDiff*dAbsDiff)) + ((-18 + 12*ParamB + 6*ParamC) * (dAbsDiff*dAbsDiff)) + (6 - 2*ParamB)) / 6.0;
    case 3:
        dAbsDiff = 1.0 + (1.0 - dFrac);
        return (((-ParamB - 6*ParamC) * std::abs(dAbsDiff*dAbsDiff*dAbsDiff)) + ((6*ParamB + 30*ParamC) * (dAbsDiff*dAbsDiff)) + (((-12 * ParamB) - (48 * ParamC)) * std::abs(dAbsDiff)) + (8*ParamB + 24*ParamC)) / 6.0;
    }
    return -1;
}

// Filter is normalized in fixed point format, sum of elements is FP_ONE
// nFrac is fractional part (sub-pixel offset), coded in [0..65535] --> [0...1]
// Returned filter in pFilterOut has length BICUBIC_FILTER_LEN
void GetBicubicFilter(uint16 nFrac, int16* pFilterOut) {
    constexpr int BICUBIC_FILTER_LEN = 4;
    double dFrac = nFrac*(1.0/65535.0);
    double dFilter[BICUBIC_FILTER_LEN];
    double dSum = 0.0;
    for (int i = 0; i < BICUBIC_FILTER_LEN; i++) {
        dFilter[i] = EvaluateCubicFilterKernel(dFrac, i);
        dSum += dFilter[i];
    }
    for (int i = 0; i < BICUBIC_FILTER_LEN; i++) {
        pFilterOut[i] = roundToInt16(CResizeFilter::FP_ONE * dFilter[i] / dSum);
    }

    NormalizeFilter(pFilterOut, BICUBIC_FILTER_LEN);
}
}

//////////////////////////////////////////////////////////////////////////////////////
// Public
//////////////////////////////////////////////////////////////////////////////////////

CResizeFilter::CResizeFilter(int nSourceSize, int nTargetSize, EFilterType eFilter, FilterSIMDType filterSIMDType)
    : m_kernels{},
    m_kernelsXMM{},
    m_kernelsAVX{},
    m_nRefCnt{ 0 }
{
    m_nSourceSize = nSourceSize;
    m_nTargetSize = nTargetSize;
    m_eFilter = eFilter;
    m_filterSIMDType = filterSIMDType;

    if (filterSIMDType == FilterSIMDType_AVX) {
        CalculateAVXFilterKernels();
    } else if (filterSIMDType == FilterSIMDType_SSE) {
        CalculateXMMFilterKernels();
    } else {
        CalculateFilterKernels();
    }
}

CResizeFilter::~CResizeFilter() {
    delete[] m_kernels.Indices;
    delete[] m_kernels.Kernels;
    delete[] m_kernelsXMM.Indices;
    delete[] m_kernelsXMM.UnalignedMemory;
    delete[] m_kernelsAVX.Indices;
    delete[] m_kernelsAVX.UnalignedMemory;
}

bool CResizeFilter::ParametersMatch(int nSourceSize, int nTargetSize, EFilterType eFilter, FilterSIMDType filterSIMDType) {
    return nSourceSize == m_nSourceSize && nTargetSize == m_nTargetSize && eFilter == m_eFilter && m_filterSIMDType == filterSIMDType;
}

void CResizeFilter::GetBicubicFilterKernels(int nNumKernels, int16* pKernels) {
    std::uint32_t nIncFrac = 65535/(nNumKernels - 1);
    std::uint32_t nKFrac = 0;
    for (int i = 0; i < nNumKernels; i++) {
        int16* pThisKernel = &(pKernels[i * 4]);
        GetBicubicFilter(static_cast<uint16>(nKFrac), pThisKernel);
        nKFrac += nIncFrac;
    }
}

//////////////////////////////////////////////////////////////////////////////////////
// Private
//////////////////////////////////////////////////////////////////////////////////////

void CResizeFilter::CalculateFilterKernels() {
    CalculateFilterParams(m_eFilter);

    if ((m_nTargetSize > m_nSourceSize && m_eFilter != Filter_Upsampling_Bicubic) ||
        m_nTargetSize == 0 || m_nSourceSize > 65535 || m_nTargetSize > 65535) {
        return;
    }

    // Calculate the increment here as the number of increments in one pixel
    // gives the number of border kernels we need.
    // The exact increment depends on the filter
    std::uint32_t nIncrementX;
    std::uint32_t nX;
    if (m_eFilter == Filter_Upsampling_Bicubic) {
        if (m_nSourceSize == 1 || m_nTargetSize == 1) {
            nIncrementX = (static_cast<std::uint32_t>(m_nSourceSize) << 16)/m_nTargetSize;
        } else {
            nIncrementX = (static_cast<std::uint32_t>(m_nSourceSize - 1) << 16)/(m_nTargetSize - 1);
        }
        nIncrementX = std::max<std::uint32_t>(1, nIncrementX);
        nX = 0;
    } else {
        nIncrementX = (static_cast<std::uint32_t>(m_nSourceSize) << 16)/m_nTargetSize + 1;
        nX = (nIncrementX - 65536) >> 1;
    }

    int nBorderKernelsPerPixel = static_cast<int>(std::max(1.0f, 65536.0f/nIncrementX) + 0.999999f);
    int nTotalKernels = NUM_KERNELS_RESIZE + nBorderKernelsPerPixel*(2*m_nFilterOffset + 1);
    m_kernels.Indices = new FilterKernel*[m_nTargetSize];
    m_kernels.Kernels = new FilterKernel[nTotalKernels]{}; // value-initialized: all zero

    // calculate kernels for different fractional values (no border kernels yet)
    std::uint32_t nIncFrac = 65535/(NUM_KERNELS_RESIZE - 1);
    std::uint32_t nKFrac = 0;
    for (int i = 0; i < NUM_KERNELS_RESIZE; i++) {
        FilterKernel* pThisKernel = &(m_kernels.Kernels[i]);
        pThisKernel->FilterLen = m_nFilterLen;
        pThisKernel->FilterOffset = m_nFilterOffset;
        int16* pKernel = GetFilter(static_cast<uint16>(nKFrac), m_eFilter);
        std::memcpy(&(pThisKernel->Kernel), pKernel, m_nFilterLen*sizeof(int16));
        nKFrac += nIncFrac;
    }

    int nIdxBorderKernel = NUM_KERNELS_RESIZE;
    for (int i = 0; i < m_nTargetSize; i++) {
        std::uint32_t nXInt = nX >> 16;
        std::uint32_t nXFrac = nX & 0xFFFF;
        if (static_cast<int>(nXInt) < m_nFilterOffset) {
            // left border handling, the (m_nFilterOffset - nXInt) left elements are cut from the filter
            int16* pBorderFilter = GetFilter(static_cast<uint16>(nXFrac), m_eFilter) + (m_nFilterOffset - nXInt);
            int nFilterLen = m_nFilterLen - (m_nFilterOffset - static_cast<int>(nXInt));
            nFilterLen = std::min(nFilterLen, m_nSourceSize);
            NormalizeFilter(pBorderFilter, nFilterLen);
            FilterKernel* pThisKernel = &(m_kernels.Kernels[nIdxBorderKernel]);
            pThisKernel->FilterLen = nFilterLen;
            pThisKernel->FilterOffset = static_cast<int>(nXInt);
            std::memcpy(&(pThisKernel->Kernel), pBorderFilter, nFilterLen * sizeof(int16));
            m_kernels.Indices[i] = pThisKernel;
            nIdxBorderKernel++;
        } else if (static_cast<int>(nXInt) - m_nFilterOffset + m_nFilterLen > m_nSourceSize) {
            // right border handling
            int16* pBorderFilter = GetFilter(static_cast<uint16>(nXFrac), m_eFilter);
            int nFilterLen = m_nSourceSize - static_cast<int>(nXInt) + m_nFilterOffset;
            nFilterLen = std::min(nFilterLen, m_nSourceSize);
            NormalizeFilter(pBorderFilter, nFilterLen);
            FilterKernel* pThisKernel = &(m_kernels.Kernels[nIdxBorderKernel]);
            pThisKernel->FilterLen = nFilterLen;
            pThisKernel->FilterOffset = m_nFilterOffset;
            std::memcpy(&(pThisKernel->Kernel), pBorderFilter, nFilterLen * sizeof(int16));
            m_kernels.Indices[i] = pThisKernel;
            nIdxBorderKernel++;
        } else {
            // normal kernels
            std::uint32_t nFilterIdx = nXFrac >> (16 - NUM_KERNELS_RESIZE_LOG2);
            m_kernels.Indices[i] = &(m_kernels.Kernels[nFilterIdx]);
        }
        nX += nIncrementX;
    }
    m_kernels.NumKernels = nIdxBorderKernel;
}

// GF version with 4 x float32 (instead of 8 x int16)
void CResizeFilter::CalculateXMMFilterKernels() {
    CalculateFilterKernels();
    if (m_nTargetSize == 0) {
        return;
    }

    // Get size of kernel array - this is not trivial as the kernels have different sizes and
    // are packed
    int nTotalKernelElements = 0;
    for (int i = 0; i < m_kernels.NumKernels; i++) {
        nTotalKernelElements += m_kernels.Kernels[i].FilterLen;
    }
    const std::size_t nSizeOfKernels = static_cast<std::size_t>(m_kernels.NumKernels) * 32 + sizeof(XMMKernelElement) * nTotalKernelElements;

    m_kernelsXMM.NumKernels = m_kernels.NumKernels;
    m_kernelsXMM.Indices = new XMMFilterKernel*[m_nTargetSize];
    m_kernelsXMM.UnalignedMemory = new uint8[nSizeOfKernels + 31];
    m_kernelsXMM.Kernels = reinterpret_cast<XMMFilterKernel*>((reinterpret_cast<std::uintptr_t>(m_kernelsXMM.UnalignedMemory) + 31) & ~static_cast<std::uintptr_t>(31));
    std::memset(m_kernelsXMM.Kernels, 0, nSizeOfKernels);

    // create an array of the start address of the filter kernels
    std::vector<XMMFilterKernel*> kernelStartAddress(static_cast<std::size_t>(m_kernelsXMM.NumKernels));
    // create the XMM kernels, pack the kernels
    XMMFilterKernel* pCurKernelXMM = m_kernelsXMM.Kernels;
    for (int i = 0; i < m_kernelsXMM.NumKernels; i++) {
        int nCurFilterLen = m_kernels.Kernels[i].FilterLen;
        kernelStartAddress[i] = pCurKernelXMM;
        pCurKernelXMM->FilterLen = nCurFilterLen;
        pCurKernelXMM->FilterOffset = m_kernels.Kernels[i].FilterOffset;
        for (int j = 0; j < nCurFilterLen; j++) {
            for (int k = 0; k < 4; k++) {
                pCurKernelXMM->Kernel[j].valueRepeated[k] = (static_cast<float>(m_kernels.Kernels[i].Kernel[j]) / static_cast<float>(FP_ONE));    // so output should be [0.0...1.0]
            }
        }
        pCurKernelXMM = reinterpret_cast<XMMFilterKernel*>(reinterpret_cast<std::uintptr_t>(pCurKernelXMM) + 32 + sizeof(XMMKernelElement)*nCurFilterLen);
    }

    for (int i = 0; i < m_nTargetSize; i++) {
        const std::ptrdiff_t nIndex = m_kernels.Indices[i] - m_kernels.Kernels;
        m_kernelsXMM.Indices[i] = kernelStartAddress[static_cast<std::size_t>(nIndex)];
    }
}

// GF version with 8 x float32
void CResizeFilter::CalculateAVXFilterKernels() {
    CalculateFilterKernels();
    if (m_nTargetSize == 0) {
        return;
    }

    // Get size of kernel array - this is not trivial as the kernels have different sizes and
    // are packed
    int nTotalKernelElements = 0;
    for (int i = 0; i < m_kernels.NumKernels; i++) {
        nTotalKernelElements += m_kernels.Kernels[i].FilterLen;
    }
    const std::size_t nSizeOfKernels = static_cast<std::size_t>(m_kernels.NumKernels) * 64 + sizeof(AVXKernelElement) * nTotalKernelElements;

    m_kernelsAVX.NumKernels = m_kernels.NumKernels;
    m_kernelsAVX.Indices = new AVXFilterKernel*[m_nTargetSize];
    m_kernelsAVX.UnalignedMemory = new uint8[nSizeOfKernels + 63];
    m_kernelsAVX.Kernels = reinterpret_cast<AVXFilterKernel*>((reinterpret_cast<std::uintptr_t>(m_kernelsAVX.UnalignedMemory) + 63) & ~static_cast<std::uintptr_t>(63));
    std::memset(m_kernelsAVX.Kernels, 0, nSizeOfKernels);

    // create an array of the start address of the filter kernels
    std::vector<AVXFilterKernel*> kernelStartAddress(static_cast<std::size_t>(m_kernelsAVX.NumKernels));
    // create the AVX kernels, pack the kernels
    AVXFilterKernel* pCurKernelAVX = m_kernelsAVX.Kernels;
    for (int i = 0; i < m_kernelsAVX.NumKernels; i++) {
        int nCurFilterLen = m_kernels.Kernels[i].FilterLen;
        kernelStartAddress[i] = pCurKernelAVX;
        pCurKernelAVX->FilterLen = nCurFilterLen;
        pCurKernelAVX->FilterOffset = m_kernels.Kernels[i].FilterOffset;
        for (int j = 0; j < nCurFilterLen; j++) {
            for (int k = 0; k < 8; k++) {
                pCurKernelAVX->Kernel[j].valueRepeated[k] = (static_cast<float>(m_kernels.Kernels[i].Kernel[j]) / static_cast<float>(FP_ONE));    // so output should be [0.0...1.0]
            }
        }
        pCurKernelAVX = reinterpret_cast<AVXFilterKernel*>(reinterpret_cast<std::uintptr_t>(pCurKernelAVX) + 64 + sizeof(AVXKernelElement)*nCurFilterLen);
    }

    for (int i = 0; i < m_nTargetSize; i++) {
        const std::ptrdiff_t nIndex = m_kernels.Indices[i] - m_kernels.Kernels;
        m_kernelsAVX.Indices[i] = kernelStartAddress[static_cast<std::size_t>(nIndex)];
    }
}

void CResizeFilter::CalculateFilterParams(EFilterType eFilter) {
    if ((eFilter == Filter_Downsampling_Hermite)
        || (eFilter == Filter_Downsampling_Mitchell)
        || (eFilter == Filter_Downsampling_Catrom)
        || (eFilter == Filter_Downsampling_Lanczos2)) {
        double dFactor = static_cast<double>(m_nSourceSize)/m_nTargetSize;
        m_dMultX = 1.0/dFactor;
        m_nFilterLen = static_cast<int>(5*dFactor);
        m_nFilterLen = std::min(MAX_FILTER_LEN, m_nFilterLen);
        m_nFilterOffset = (m_nFilterLen - 1)/2;
    } else if (eFilter == Filter_Upsampling_Bicubic) {
        m_dMultX = 1.0;
        m_nFilterLen = 4;
        m_nFilterOffset = 1;
    } else {
        m_dMultX = 0.0;
        m_nFilterLen = 0;
        m_nFilterOffset = 0;
    }
}


// Filter is normalized in fixed point format, sum of elements is FP_ONE
// nFrac is fractional part (sub-pixel offset), coded in [0..65535] --> [0...1]
int16* CResizeFilter::GetFilter(uint16 nFrac, EFilterType eFilter) {
    double dFrac = nFrac*(1.0/65535.0);
    double dFilter[MAX_FILTER_LEN];
    double dSum = 0.0;
    for (int i = 0; i < m_nFilterLen; i++) {
        if (eFilter == Filter_Upsampling_Bicubic) {
            dFilter[i] = EvaluateCubicFilterKernel(dFrac, i);
        } else if ((eFilter == Filter_Downsampling_Hermite) || (eFilter == Filter_Downsampling_Mitchell) || (eFilter == Filter_Downsampling_Catrom) || (eFilter == Filter_Downsampling_Lanczos2)) {
            dFilter[i] = EvaluateKernel(m_dMultX*(-m_nFilterOffset + i - dFrac), eFilter);
        } else {
            dFilter[i] = EvaluateKernelIntegrated(-m_nFilterOffset + i - dFrac, eFilter, m_dMultX);
        }
        dSum += dFilter[i];
    }
    int nSum = 0;
    for (int i = 0; i < m_nFilterLen; i++) {
        m_Filter[i] = roundToInt16(FP_ONE * dFilter[i] / dSum);
        nSum += m_Filter[i];
    }
    m_Filter[0] += static_cast<int16>(FP_ONE - nSum);
    for (int j = m_nFilterLen; j < MAX_FILTER_LEN; j++) {
        m_Filter[j] = 0;
    }
    return m_Filter;
}

//////////////////////////////////////////////////////////////////////////////////////
// CResizeFilterCache
//////////////////////////////////////////////////////////////////////////////////////

CResizeFilterCache& CResizeFilterCache::This() {
    // Thread-safe initialization (C++11), destroyed at program exit
    static CResizeFilterCache instance;
    return instance;
}

CResizeFilterCache::~CResizeFilterCache() {
    for (CResizeFilter* pFilter : m_filterList) {
        delete pFilter;
    }
}

const CResizeFilter& CResizeFilterCache::GetFilter(int nSourceSize, int nTargetSize, EFilterType eFilter, FilterSIMDType filterSIMDType) {
    std::scoped_lock lock(m_mutex);

    for (auto iter = m_filterList.begin(); iter != m_filterList.end(); ++iter) {
        if ((*iter)->ParametersMatch(nSourceSize, nTargetSize, eFilter, filterSIMDType)) {
            // found matching filter: move it to the top of the list and return it
            m_filterList.splice(m_filterList.begin(), m_filterList, iter);
            CResizeFilter& matchingFilter = *m_filterList.front();
            matchingFilter.m_nRefCnt++;
            return matchingFilter;
        }
    }

    // no matching filter found, create a new one
    CResizeFilter* pNewFilter = new CResizeFilter(nSourceSize, nTargetSize, eFilter, filterSIMDType);
    pNewFilter->m_nRefCnt++;
    m_filterList.push_front(pNewFilter);

    return *pNewFilter;
}

void CResizeFilterCache::ReleaseFilter(const CResizeFilter& filter) {
    constexpr std::size_t MAX_SIZE = 4;

    std::scoped_lock lock(m_mutex);
    filter.m_nRefCnt--;
    if (m_filterList.size() > MAX_SIZE) {
        // cache too large - try to free the least recently used entry that is not in use
        for (auto iter = m_filterList.rbegin(); iter != m_filterList.rend(); ++iter) {
            if ((*iter)->m_nRefCnt <= 0) {
                CResizeFilter* pElementTBRemoved = *iter;
                m_filterList.erase(std::next(iter).base());
                delete pElementTBRemoved;
                break;
            }
        }
    }
}

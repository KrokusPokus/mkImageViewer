#include "EXIFReader.h"
#include "Helpers.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <QByteArray>

double CEXIFReader::UNKNOWN_DOUBLE_VALUE = 283740261.192864;

namespace {
uint32 ReadUInt(void * ptr, bool bLittleEndian) {
    uint32 nValue = *static_cast<uint32*>(ptr);
    if (!bLittleEndian) {
        return __builtin_bswap32(nValue);
    } else {
        return nValue;
    }
}

void WriteUInt(void * ptr, uint32 nValue, bool bLittleEndian) {
    uint32 nVal = nValue;
    if (!bLittleEndian) {
        nVal = __builtin_bswap32(nVal);
    }
    *static_cast<uint32*>(ptr) = nVal;
}

uint16 ReadUShort(void * ptr, bool bLittleEndian) {
    uint16 nValue = *static_cast<uint16*>(ptr);
    if (!bLittleEndian) {
        return __builtin_bswap16(nValue);
    } else {
        return nValue;
    }
}

void WriteUShort(void * ptr, uint16 nValue, bool bLittleEndian) {
    uint16 nVal = nValue;
    if (!bLittleEndian) {
        nVal = __builtin_bswap16(nVal);
    }
    *static_cast<uint16*>(ptr) = nVal;
}

// The TIFF data of the EXIF block (behind "Exif\0\0"): all offsets in the EXIF data are relative to its start.
// Offsets and lengths come from the file and can be corrupt or manipulated, so every access with them is checked
// with Contains() (in 64 bit, offset + length can not overflow).
struct TiffData {
    uint8* p = nullptr;  // start of the TIFF header
    uint32 size = 0;     // number of bytes from p to the end of the APP1 block

    bool Contains(uint64_t offset, uint64_t length) const { return offset <= size && length <= size - offset; }
};

// The entries (12 bytes each) between ptr and ptrLast must be inside the data, the caller checks this
uint8* FindTag(uint8* ptr, uint8* ptrLast, uint16 nTag, bool bLittleEndian) {
    while (ptr < ptrLast) {
        if (ReadUShort(ptr, bLittleEndian) == nTag) {
            return ptr;
        }
        ptr += 12;
    }
    return nullptr;
}

void ReadStringTag(QString & strOut, uint8* ptr, const TiffData& tiff, bool bLittleEndian, bool bTryReadAsUTF8 = false, uint32 maxLength = 65536) {
    try {
        if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 2) {
            uint32 nSize = ReadUInt(ptr + 4, bLittleEndian);
            if (nSize > maxLength) {
                strOut.clear();
                return;
            }
            const char* pString;
            if (nSize <= 4) {
                pString = reinterpret_cast<const char*>(ptr + 8); // stored in the entry itself
            } else {
                const uint32 nOffset = ReadUInt(ptr + 8, bLittleEndian);
                if (!tiff.Contains(nOffset, nSize)) {
                    strOut.clear();
                    return;
                }
                pString = reinterpret_cast<const char*>(tiff.p + nOffset);
            }

            int nMaxChars = static_cast<int>(std::min<uint32>(nSize, strnlen(pString, nSize)));
            if (bTryReadAsUTF8) {
                QString strUTF8 = Helpers::TryConvertFromUTF8(reinterpret_cast<const uint8_t*>(pString), nMaxChars);
                if (!strUTF8.isEmpty()) {
                    strOut = strUTF8;
                    return;
                }
            }
            strOut = QString::fromLocal8Bit(pString, nMaxChars);
        } else {
            strOut.clear();
        }
    } catch (...) {
        strOut.clear();
    }
}

void ReadUserCommentTag(QString & strOut, uint8* ptr, const TiffData& tiff, bool bLittleEndian) {
    try {
        if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 7) {
            uint32 nSize32 = ReadUInt(ptr + 4, bLittleEndian);
            const uint32 nOffset = ReadUInt(ptr + 8, bLittleEndian);
            if (nSize32 > 10 && nSize32 <= 4096 && tiff.Contains(nOffset, nSize32)) {
                int nSize = static_cast<int>(nSize32);
                // 8 bytes character code, then the comment
                const char* sCodeDesc = reinterpret_cast<const char*>(tiff.p + nOffset);
                if (memcmp(sCodeDesc, "ASCII\0", 6) == 0) {
                    strOut = QString::fromLocal8Bit(sCodeDesc + 8, nSize - 8);
                } else if (memcmp(sCodeDesc, "UNICODE\0", 8) == 0) {
                    bool bLE = bLittleEndian;
                    nSize -= 8;
                    sCodeDesc += 8;
                    if (sCodeDesc[0] == static_cast<char>(0xFF) && sCodeDesc[1] == static_cast<char>(0xFE)) {
                        bLE = true;
                        nSize -= 2; sCodeDesc += 2;
                    } else if (sCodeDesc[0] == static_cast<char>(0xFE) && sCodeDesc[1] == static_cast<char>(0xFF)) {
                        bLE = false;
                        nSize -= 2; sCodeDesc += 2;
                    }
                    if (bLE) {
                        strOut = QString::fromUtf16(reinterpret_cast<const char16_t*>(sCodeDesc), nSize / 2);
                    } else {
                        QByteArray ba(sCodeDesc, nSize);
                        for (int i = 0; i < nSize; i += 2) {
                            if (i + 1 < ba.size()) {
                                char t = ba[i];
                                ba[i] = ba[i + 1];
                                ba[i + 1] = t;
                            }
                        }
                        strOut = QString::fromUtf16(reinterpret_cast<const char16_t*>(ba.constData()), nSize / 2);
                    }
                }
            }
        }
    } catch (...) {
        strOut.clear();
    }
}

uint32 ReadLongTag(uint8* ptr, bool bLittleEndian) {
    if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 4) {
        return ReadUInt(ptr + 8, bLittleEndian);
    } else {
        return 0;
    }
}

uint16 ReadShortTag(uint8* ptr, bool bLittleEndian) {
    if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 3) {
        return ReadUShort(ptr + 8, bLittleEndian);
    } else {
        return 0;
    }
}

uint32 ReadShortOrLongTag(uint8* ptr, bool bLittleEndian) {
    if (ptr != nullptr) {
        uint16 nType = ReadUShort(ptr + 2, bLittleEndian);
        if (nType == 3) {
            return ReadUShort(ptr + 8, bLittleEndian);
        } else if (nType == 4) {
            return ReadUInt(ptr + 8, bLittleEndian);
        }
    }
    return 0;
}

// Absolute value of a signed 32 bit value as unsigned (also for INT_MIN, where abs() is undefined)
uint32 AbsSigned(uint32 nValue) {
    const int64_t n = static_cast<int32_t>(nValue);
    return static_cast<uint32>(n < 0 ? -n : n);
}

int ReadRationalTag(Rational & rational, uint8* ptr, const TiffData& tiff, int index, bool bLittleEndian) {
    rational.Numerator = 0;
    rational.Denominator = 0;
    if (ptr != nullptr) {
        uint16 nType = ReadUShort(ptr + 2, bLittleEndian);
        uint16 nCount = ReadUInt(ptr + 4, bLittleEndian);
        if (index < nCount) {
            if (nType == 5 || nType == 10) {
                const uint64_t nOffset = static_cast<uint64_t>(ReadUInt(ptr + 8, bLittleEndian)) + static_cast<uint64_t>(index) * 8;
                if (!tiff.Contains(nOffset, 8)) {
                    return nType; // value outside of the data: unknown
                }
                rational.Numerator = ReadUInt(tiff.p + nOffset, bLittleEndian);
                rational.Denominator = ReadUInt(tiff.p + nOffset + 4, bLittleEndian);
                if (rational.Numerator != 0 && rational.Denominator != 0) {
                    uint32 nModulo;
                    uint32 nA = (nType == 10) ? AbsSigned(rational.Numerator) : rational.Numerator;
                    uint32 nB = (nType == 10) ? AbsSigned(rational.Denominator) : rational.Denominator;
                    do {
                        nModulo = nA % nB;
                        nA = nB;
                        nB = nModulo;
                    } while (nB != 0);

                    if (nType == 10) {
                        // in 64 bit: INT_MIN / -1 would overflow in 32 bit
                        rational.Numerator = static_cast<uint32>(static_cast<int64_t>(static_cast<int32_t>(rational.Numerator)) / static_cast<int64_t>(nA));
                        rational.Denominator = static_cast<uint32>(static_cast<int64_t>(static_cast<int32_t>(rational.Denominator)) / static_cast<int64_t>(nA));
                    } else {
                        rational.Numerator /= nA;
                        rational.Denominator /= nA;
                    }
                }
            }
        }
        return nType;
    }
    return 0;
}

int ReadRationalTag(Rational & rational, uint8* ptr, const TiffData& tiff, bool bLittleEndian) {
    return ReadRationalTag(rational, ptr, tiff, 0, bLittleEndian);
}

bool ReadSignedRationalTag(SignedRational & rational, uint8* ptr, const TiffData& tiff, bool bLittleEndian) {
    return ReadRationalTag(reinterpret_cast<Rational &>(rational), ptr, tiff, bLittleEndian);
}

double ReadDoubleTag(uint8* ptr, const TiffData& tiff, int index, bool bLittleEndian) {
    Rational rational(0, 0);
    int nType = ReadRationalTag(rational, ptr, tiff, index, bLittleEndian);
    if (rational.Denominator != 0) {
        if (nType == 5) {
            return static_cast<double>(rational.Numerator) / rational.Denominator;
        } else {
            int nNum = static_cast<int>(rational.Numerator);
            int nDenum = static_cast<int>(rational.Denominator);
            return static_cast<double>(nNum) / nDenum;
        }
    }
    return CEXIFReader::UNKNOWN_DOUBLE_VALUE;
}

double ReadDoubleTag(uint8* ptr, const TiffData& tiff, bool bLittleEndian) {
    return ReadDoubleTag(ptr, tiff, 0, bLittleEndian);
}

} // namespace

bool CEXIFReader::ParseDateString(QDateTime & date, const QString& str) {
    // EXIF Format: "YYYY:MM:DD HH:MM:SS"
    date = QDateTime::fromString(str.trimmed(), "yyyy:MM:dd hh:mm:ss");
    return date.isValid();
}

CEXIFReader::CEXIFReader(void* pApp1Block, int nBlockSize, EImageFormat eImageFormat)
    : m_exposureTime{ 0, 0 },
    m_acqDate{},
    m_dateTime{}
{
    m_bFlashFired = false;
    m_bFlashFlagPresent = false;
    m_dFocalLength = m_dExposureBias = m_dFNumber = UNKNOWN_DOUBLE_VALUE;
    m_nISOSpeed = 0;
    m_nImageOrientation = 0;
    m_pTagOrientation = nullptr;
    m_bLittleEndian = true;
    m_nThumbWidth = -1;
    m_nThumbHeight = -1;
    m_pLastIFD0 = nullptr;
    m_pIFD1 = nullptr;
    m_pLastIFD1 = nullptr;
    m_pLatitude = nullptr;
    m_pLongitude = nullptr;
    m_dAltitude = UNKNOWN_DOUBLE_VALUE;

    m_pApp1 = static_cast<uint8*>(pApp1Block);
    // APP1 marker (2) + length (2) + "Exif\0\0" (6) + TIFF header (8)
    if (m_pApp1 == nullptr || nBlockSize < 18 || m_pApp1[0] != 0xFF || m_pApp1[1] != 0xE1) {
        return;
    }
    // The length stored in the block must not reach beyond the buffer
    const int nApp1Size = std::min(nBlockSize, m_pApp1[2] * 256 + m_pApp1[3] + 2);
    if (nApp1Size < 18) {
        return;
    }
    const TiffData tiff{ m_pApp1 + 10, static_cast<uint32>(nApp1Size - 10) };

    uint8* pTIFFHeader = tiff.p;
    bool bLittleEndian;
    if (*reinterpret_cast<short*>(pTIFFHeader) == 0x4949) {
        bLittleEndian = true;
    } else if (*reinterpret_cast<short*>(pTIFFHeader) == 0x4D4D) {
        bLittleEndian = false;
    } else {
        return;
    }
    m_bLittleEndian = bLittleEndian;

    // An IFD is: number of entries (2 bytes), the entries (12 bytes each), offset of the next IFD (4 bytes)
    const uint32 nOffsetIFD0 = ReadUInt(pTIFFHeader + 4, bLittleEndian);
    if (!tiff.Contains(nOffsetIFD0, 2)) {
        return;
    }
    uint8* pIFD0 = tiff.p + nOffsetIFD0;
    uint16 nNumTags = ReadUShort(pIFD0, bLittleEndian);
    pIFD0 += 2;
    if (!tiff.Contains(static_cast<uint64_t>(nOffsetIFD0) + 2, static_cast<uint64_t>(nNumTags) * 12 + 4)) {
        return;
    }
    uint8* pLastIFD0 = pIFD0 + nNumTags * 12;
    uint32 nOffsetIFD1 = ReadUInt(pLastIFD0, bLittleEndian);
    m_pLastIFD0 = pLastIFD0;

    uint8* pTagOrientation = nullptr;
    if (eImageFormat != IF_JXL && eImageFormat != IF_HEIF && eImageFormat != IF_AVIF) {
        pTagOrientation = FindTag(pIFD0, pLastIFD0, 0x112, bLittleEndian);
    }
    if (pTagOrientation != nullptr) {
        m_nImageOrientation = ReadShortTag(pTagOrientation, bLittleEndian);
    }
    m_pTagOrientation = pTagOrientation;

    uint8* pTagModel = FindTag(pIFD0, pLastIFD0, 0x110, bLittleEndian);
    ReadStringTag(m_sModel, pTagModel, tiff, bLittleEndian);

    uint8* pTagImageDesc = FindTag(pIFD0, pLastIFD0, 0x10E, bLittleEndian);
    ReadStringTag(m_sImageDescription, pTagImageDesc, tiff, bLittleEndian, true);

    if (!m_sModel.isEmpty()) {
        QString sMake;
        uint8* pTagMake = FindTag(pIFD0, pLastIFD0, 0x10F, bLittleEndian);
        ReadStringTag(sMake, pTagMake, tiff, bLittleEndian);
        if (!sMake.isEmpty()) {
            int nSpace = sMake.indexOf(QLatin1Char(' '));
            QString sMakeL(nSpace > 0 ? sMake.left(nSpace) : sMake);
            if (m_sModel.indexOf(sMakeL) == -1) {
                m_sModel = sMakeL + QStringLiteral(" ") + m_sModel;
            }
        }
    }

    uint8* pTagSoftware = FindTag(pIFD0, pLastIFD0, 0x0131, bLittleEndian);
    ReadStringTag(m_sSoftware, pTagSoftware, tiff, bLittleEndian);

    uint8* pTagModDate = FindTag(pIFD0, pLastIFD0, 0x0132, bLittleEndian);
    QString sModDate;
    ReadStringTag(sModDate, pTagModDate, tiff, bLittleEndian);
    ParseDateString(m_dateTime, sModDate);

    uint8* pTagEXIFIFD = FindTag(pIFD0, pLastIFD0, 0x8769, bLittleEndian);
    if (pTagEXIFIFD == nullptr) {
        return;
    }

    uint8* pTagGPSIFD = FindTag(pIFD0, pLastIFD0, 0x8825, bLittleEndian);
    if (pTagGPSIFD != nullptr) {
        ReadGPSData(tiff.p, tiff.size, pTagGPSIFD, bLittleEndian);
    }

    uint32 nOffsetEXIF = ReadLongTag(pTagEXIFIFD, bLittleEndian);
    if (nOffsetEXIF == 0) {
        return;
    }
    if (!tiff.Contains(nOffsetEXIF, 2)) {
        return;
    }
    uint8* pEXIFIFD = tiff.p + nOffsetEXIF;
    nNumTags = ReadUShort(pEXIFIFD, bLittleEndian);
    pEXIFIFD += 2;
    if (!tiff.Contains(static_cast<uint64_t>(nOffsetEXIF) + 2, static_cast<uint64_t>(nNumTags) * 12)) {
        return;
    }
    uint8* pLastEXIF = pEXIFIFD + nNumTags * 12;

    uint8* pTagAcquisitionDate = FindTag(pEXIFIFD, pLastEXIF, 0x9003, bLittleEndian);
    QString sAcqDate;
    ReadStringTag(sAcqDate, pTagAcquisitionDate, tiff, bLittleEndian);
    ParseDateString(m_acqDate, sAcqDate);

    uint8* pTagExposureTime = FindTag(pEXIFIFD, pLastEXIF, 0x829A, bLittleEndian);
    ReadRationalTag(m_exposureTime, pTagExposureTime, tiff, bLittleEndian);

    uint8* pTagExposureBias = FindTag(pEXIFIFD, pLastEXIF, 0x9204, bLittleEndian);
    m_dExposureBias = ReadDoubleTag(pTagExposureBias, tiff, bLittleEndian);

    uint8* pTagFlash = FindTag(pEXIFIFD, pLastEXIF, 0x9209, bLittleEndian);
    uint16 nFlash = ReadShortTag(pTagFlash, bLittleEndian);
    m_bFlashFired = (nFlash & 1) != 0;
    m_bFlashFlagPresent = pTagFlash != nullptr;

    uint8* pTagFocalLength = FindTag(pEXIFIFD, pLastEXIF, 0x920A, bLittleEndian);
    m_dFocalLength = ReadDoubleTag(pTagFocalLength, tiff, bLittleEndian);

    uint8* pTagFNumber = FindTag(pEXIFIFD, pLastEXIF, 0x829D, bLittleEndian);
    m_dFNumber = ReadDoubleTag(pTagFNumber, tiff, bLittleEndian);

    uint8* pTagISOSpeed = FindTag(pEXIFIFD, pLastEXIF, 0x8827, bLittleEndian);
    uint8* pTagISOSpeed2 = FindTag(pEXIFIFD, pLastEXIF, 0x8833, bLittleEndian);

    m_nISOSpeed = (pTagISOSpeed != nullptr) ? ReadShortTag(pTagISOSpeed, bLittleEndian) :
                      ReadLongTag(pTagISOSpeed2, bLittleEndian);

    uint8* pTagUserComment = FindTag(pEXIFIFD, pLastEXIF, 0x9286, bLittleEndian);
    ReadUserCommentTag(m_sUserComment, pTagUserComment, tiff, bLittleEndian);
    if (m_sUserComment == QStringLiteral("User comments")) {
        m_sUserComment.clear();
    }

    if (nOffsetIFD1 != 0) {
        if (!tiff.Contains(nOffsetIFD1, 2)) {
            return;
        }
        uint8* pIFD1 = tiff.p + nOffsetIFD1;
        nNumTags = ReadUShort(pIFD1, bLittleEndian);
        pIFD1 += 2;
        if (!tiff.Contains(static_cast<uint64_t>(nOffsetIFD1) + 2, static_cast<uint64_t>(nNumTags) * 12)) {
            return;
        }
        m_pIFD1 = pIFD1;
        uint8* pLastIFD1 = m_pIFD1 + nNumTags * 12;
        m_pLastIFD1 = pLastIFD1;
        uint8* pTagCompression = FindTag(m_pIFD1, pLastIFD1, 0x103, bLittleEndian);
        if (pTagCompression == nullptr) {
            return;
        }
        if (ReadShortTag(pTagCompression, bLittleEndian) == 6) {
            uint8* pTagOffsetSOI = FindTag(m_pIFD1, pLastIFD1, 0x0201, bLittleEndian);
            uint8* pTagJPEGLen = FindTag(m_pIFD1, pLastIFD1, 0x0202, bLittleEndian);
            if (pTagOffsetSOI != nullptr && pTagJPEGLen != nullptr) {
                uint32 nOffsetSOI = ReadLongTag(pTagOffsetSOI, bLittleEndian);
                uint32 nJPEGBytes = ReadLongTag(pTagJPEGLen, bLittleEndian);
                if (!tiff.Contains(nOffsetSOI, nJPEGBytes)) {
                    return; // the thumbnail is not (completely) inside of the EXIF data
                }
                uint8* pSOI = tiff.p + nOffsetSOI;
                const uint8* pSOF = reinterpret_cast<const uint8*>(Helpers::FindJPEGMarker(pSOI, static_cast<int>(nJPEGBytes), 0xC0));
                // the SOF segment is read up to byte 8 (height, width)
                if (pSOF != nullptr && pSOF + 9 <= pSOI + nJPEGBytes) {
                    m_nThumbWidth = pSOF[7] * 256 + pSOF[8];
                    m_nThumbHeight = pSOF[5] * 256 + pSOF[6];
                }
            }
        } else {
            uint8* pTagThumbWidth = FindTag(m_pIFD1, pLastIFD1, 0x0001, bLittleEndian);
            uint8* pTagThumbHeight = FindTag(m_pIFD1, pLastIFD1, 0x0101, bLittleEndian);
            if (pTagThumbWidth != nullptr && pTagThumbHeight != nullptr) {
                m_nThumbWidth = ReadShortOrLongTag(pTagThumbWidth, bLittleEndian);
                m_nThumbHeight = ReadShortOrLongTag(pTagThumbHeight, bLittleEndian);
            }
        }
    }
}

CEXIFReader::~CEXIFReader(void) {
    delete m_pLatitude;
    delete m_pLongitude;
}

void CEXIFReader::ReadGPSData(uint8* pTIFFHeader, uint32 nTIFFSize, uint8* pTagGPSIFD, bool bLittleEndian) {
    const TiffData tiff{ pTIFFHeader, nTIFFSize };
    uint32 nOffsetGPS = ReadLongTag(pTagGPSIFD, bLittleEndian);
    if (nOffsetGPS == 0 || !tiff.Contains(nOffsetGPS, 2)) {
        return;
    }
    uint8* pGPSIFD = tiff.p + nOffsetGPS;
    int nNumTags = ReadUShort(pGPSIFD, bLittleEndian);
    pGPSIFD += 2;
    if (!tiff.Contains(static_cast<uint64_t>(nOffsetGPS) + 2, static_cast<uint64_t>(nNumTags) * 12)) {
        return;
    }
    uint8* pLastGPS = pGPSIFD + nNumTags * 12;

    uint8* pTagLatitudeRef = FindTag(pGPSIFD, pLastGPS, 0x1, bLittleEndian);
    if (pTagLatitudeRef == nullptr)
        return;
    QString latitudeRef;
    ReadStringTag(latitudeRef, pTagLatitudeRef, tiff, bLittleEndian, false, 2);

    uint8* pTagLatitude = FindTag(pGPSIFD, pLastGPS, 0x2, bLittleEndian);
    m_pLatitude = ReadGPSCoordinate(tiff.p, tiff.size, pTagLatitude, latitudeRef, bLittleEndian);

    uint8* pTagLongitudeRef = FindTag(pGPSIFD, pLastGPS, 0x3, bLittleEndian);
    if (pTagLongitudeRef == nullptr)
        return;
    QString longitudeRef;
    ReadStringTag(longitudeRef, pTagLongitudeRef, tiff, bLittleEndian, false, 2);

    uint8* pTagLongitude = FindTag(pGPSIFD, pLastGPS, 0x4, bLittleEndian);
    m_pLongitude = ReadGPSCoordinate(tiff.p, tiff.size, pTagLongitude, longitudeRef, bLittleEndian);

    uint8* pTagAltitude = FindTag(pGPSIFD, pLastGPS, 0x6, bLittleEndian);
    if (pTagAltitude != nullptr) {
        m_dAltitude = ReadDoubleTag(pTagAltitude, tiff, bLittleEndian);
        uint8* pTagAltitudeRef = FindTag(pGPSIFD, pLastGPS, 0x5, bLittleEndian);
        if (pTagAltitudeRef != nullptr && *(pTagAltitudeRef + 8) == 1) {
            m_dAltitude *= -1;
        }
    }
}

GPSCoordinate* CEXIFReader::ReadGPSCoordinate(uint8* pTIFFHeader, uint32 nTIFFSize, uint8* pTagLatOrLong, const QString& reference, bool bLittleEndian) {
    if (pTagLatOrLong == nullptr)
        return nullptr;
    const TiffData tiff{ pTIFFHeader, nTIFFSize };
    double dDeg = ReadDoubleTag(pTagLatOrLong, tiff, 0, bLittleEndian);
    double dMin = ReadDoubleTag(pTagLatOrLong, tiff, 1, bLittleEndian);
    double dSec = ReadDoubleTag(pTagLatOrLong, tiff, 2, bLittleEndian);
    if (dDeg == UNKNOWN_DOUBLE_VALUE || dMin == UNKNOWN_DOUBLE_VALUE || dSec == UNKNOWN_DOUBLE_VALUE)
        return nullptr;
    // GPSCoordinate converts the values to int: absurd values (corrupt data) would not fit
    const double cdMaxValue = 1e6;
    if (!(fabs(dDeg) <= cdMaxValue && fabs(dMin) <= cdMaxValue && fabs(dSec) <= cdMaxValue))
        return nullptr;

    return new GPSCoordinate(reference, dDeg, dMin, dSec);
}

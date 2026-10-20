#include "EXIFReader.h"
#include "Helpers.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <QByteArray>

double CEXIFReader::UNKNOWN_DOUBLE_VALUE = 283740261.192864;

namespace {
uint32 ReadUInt(void * ptr, bool bLittleEndian) {
    uint32 nValue = *static_cast<uint32*>(ptr);
    if (!bLittleEndian) {
        return _byteswap_ulong(nValue);
    } else {
        return nValue;
    }
}

void WriteUInt(void * ptr, uint32 nValue, bool bLittleEndian) {
    uint32 nVal = nValue;
    if (!bLittleEndian) {
        nVal = _byteswap_ulong(nVal);
    }
    *static_cast<uint32*>(ptr) = nVal;
}

uint16 ReadUShort(void * ptr, bool bLittleEndian) {
    uint16 nValue = *static_cast<uint16*>(ptr);
    if (!bLittleEndian) {
        return _byteswap_ushort(nValue);
    } else {
        return nValue;
    }
}

void WriteUShort(void * ptr, uint16 nValue, bool bLittleEndian) {
    uint16 nVal = nValue;
    if (!bLittleEndian) {
        nVal = _byteswap_ushort(nVal);
    }
    *static_cast<uint16*>(ptr) = nVal;
}

uint8* FindTag(uint8* ptr, uint8* ptrLast, uint16 nTag, bool bLittleEndian) {
    while (ptr < ptrLast) {
        if (ReadUShort(ptr, bLittleEndian) == nTag) {
            return ptr;
        }
        ptr += 12;
    }
    return nullptr;
}

void ReadStringTag(QString & strOut, uint8* ptr, uint8* pTIFFHeader, bool bLittleEndian, bool bTryReadAsUTF8 = false, uint32 maxLength = 65536) {
    try {
        if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 2) {
            uint32 nSize = ReadUInt(ptr + 4, bLittleEndian);
            if (nSize > maxLength) {
                strOut.clear();
                return;
            }
            const char* pString = (nSize <= 4) ? reinterpret_cast<const char*>(ptr + 8)
                                               : reinterpret_cast<const char*>(pTIFFHeader + ReadUInt(ptr + 8, bLittleEndian));

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

void ReadUserCommentTag(QString & strOut, uint8* ptr, uint8* pTIFFHeader, bool bLittleEndian) {
    try {
        if (ptr != nullptr && ReadUShort(ptr + 2, bLittleEndian) == 7) {
            int nSize = ReadUInt(ptr + 4, bLittleEndian);
            if (nSize > 10 && nSize <= 4096) {
                const char* sCodeDesc = reinterpret_cast<const char*>(pTIFFHeader + ReadUInt(ptr + 8, bLittleEndian));
                if (strcmp(sCodeDesc, "ASCII") == 0) {
                    strOut = QString::fromLocal8Bit(sCodeDesc + 8, nSize - 8);
                } else if (strcmp(sCodeDesc, "UNICODE") == 0) {
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

int ReadRationalTag(Rational & rational, uint8* ptr, uint8* pTIFFHeader, int index, bool bLittleEndian) {
    rational.Numerator = 0;
    rational.Denominator = 0;
    if (ptr != nullptr) {
        uint16 nType = ReadUShort(ptr + 2, bLittleEndian);
        uint16 nCount = ReadUInt(ptr + 4, bLittleEndian);
        if (index < nCount) {
            if (nType == 5 || nType == 10) {
                int nOffset = ReadUInt(ptr + 8, bLittleEndian) + index * 8;
                rational.Numerator = ReadUInt(pTIFFHeader + nOffset, bLittleEndian);
                rational.Denominator = ReadUInt(pTIFFHeader + nOffset + 4, bLittleEndian);
                if (rational.Numerator != 0 && rational.Denominator != 0) {
                    uint32 nModulo;
                    uint32 nA = (nType == 10) ? abs(static_cast<int>(rational.Numerator)) : rational.Numerator;
                    uint32 nB = (nType == 10) ? abs(static_cast<int>(rational.Denominator)) : rational.Denominator;
                    do {
                        nModulo = nA % nB;
                        nA = nB;
                        nB = nModulo;
                    } while (nB != 0);

                    if (nType == 10) {
                        rational.Numerator = static_cast<uint32>(static_cast<int>(rational.Numerator) / static_cast<int>(nA));
                        rational.Denominator = static_cast<uint32>(static_cast<int>(rational.Denominator) / static_cast<int>(nA));
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

int ReadRationalTag(Rational & rational, uint8* ptr, uint8* pTIFFHeader, bool bLittleEndian) {
    return ReadRationalTag(rational, ptr, pTIFFHeader, 0, bLittleEndian);
}

bool ReadSignedRationalTag(SignedRational & rational, uint8* ptr, uint8* pTIFFHeader, bool bLittleEndian) {
    return ReadRationalTag(reinterpret_cast<Rational &>(rational), ptr, pTIFFHeader, bLittleEndian);
}

double ReadDoubleTag(uint8* ptr, uint8* pTIFFHeader, int index, bool bLittleEndian) {
    Rational rational(0, 0);
    int nType = ReadRationalTag(rational, ptr, pTIFFHeader, index, bLittleEndian);
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

double ReadDoubleTag(uint8* ptr, uint8* pTIFFHeader, bool bLittleEndian) {
    return ReadDoubleTag(ptr, pTIFFHeader, 0, bLittleEndian);
}

} // namespace

bool CEXIFReader::ParseDateString(SYSTEMTIME & date, const QString& str) {
    int nYear, nMonth, nDay, nHour, nMin, nSec;
    if (sscanf(str.toLatin1().constData(), "%d:%d:%d %d:%d:%d", &nYear, &nMonth, &nDay, &nHour, &nMin, &nSec) == 6) {
        date.wYear = static_cast<WORD>(nYear);
        date.wMonth = static_cast<WORD>(nMonth);
        date.wDay = static_cast<WORD>(nDay);
        date.wHour = static_cast<WORD>(nHour);
        date.wMinute = static_cast<WORD>(nMin);
        date.wSecond = static_cast<WORD>(nSec);
        date.wMilliseconds = 0;
        date.wDayOfWeek = 0;
        return true;
    }
    return false;
}

CEXIFReader::CEXIFReader(void* pApp1Block, EImageFormat eImageFormat)
    : m_exposureTime{ 0, 0 },
    m_acqDate{ 0 },
    m_dateTime{ 0 }
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
    m_bHasJPEGCompressedThumbnail = false;
    m_nJPEGThumbStreamLen = 0;
    m_pLatitude = nullptr;
    m_pLongitude = nullptr;
    m_dAltitude = UNKNOWN_DOUBLE_VALUE;

    m_pApp1 = static_cast<uint8*>(pApp1Block);
    if (m_pApp1[0] != 0xFF || m_pApp1[1] != 0xE1) {
        return;
    }
    int nApp1Size = m_pApp1[2] * 256 + m_pApp1[3] + 2;

    uint8* pTIFFHeader = m_pApp1 + 10;
    bool bLittleEndian;
    if (*reinterpret_cast<short*>(pTIFFHeader) == 0x4949) {
        bLittleEndian = true;
    } else if (*reinterpret_cast<short*>(pTIFFHeader) == 0x4D4D) {
        bLittleEndian = false;
    } else {
        return;
    }
    m_bLittleEndian = bLittleEndian;

    uint8* pIFD0 = pTIFFHeader + ReadUInt(pTIFFHeader + 4, bLittleEndian);
    if (pIFD0 - m_pApp1 >= nApp1Size) {
        return;
    }

    uint16 nNumTags = ReadUShort(pIFD0, bLittleEndian);
    pIFD0 += 2;
    uint8* pLastIFD0 = pIFD0 + nNumTags * 12;
    if (pLastIFD0 - m_pApp1 + 4 >= nApp1Size) {
        return;
    }
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
    ReadStringTag(m_sModel, pTagModel, pTIFFHeader, bLittleEndian);

    uint8* pTagImageDesc = FindTag(pIFD0, pLastIFD0, 0x10E, bLittleEndian);
    ReadStringTag(m_sImageDescription, pTagImageDesc, pTIFFHeader, bLittleEndian, true);

    if (!m_sModel.isEmpty()) {
        QString sMake;
        uint8* pTagMake = FindTag(pIFD0, pLastIFD0, 0x10F, bLittleEndian);
        ReadStringTag(sMake, pTagMake, pTIFFHeader, bLittleEndian);
        if (!sMake.isEmpty()) {
            int nSpace = sMake.indexOf(QLatin1Char(' '));
            QString sMakeL(nSpace > 0 ? sMake.left(nSpace) : sMake);
            if (m_sModel.indexOf(sMakeL) == -1) {
                m_sModel = sMakeL + QStringLiteral(" ") + m_sModel;
            }
        }
    }

    uint8* pTagSoftware = FindTag(pIFD0, pLastIFD0, 0x0131, bLittleEndian);
    ReadStringTag(m_sSoftware, pTagSoftware, pTIFFHeader, bLittleEndian);

    uint8* pTagModDate = FindTag(pIFD0, pLastIFD0, 0x0132, bLittleEndian);
    QString sModDate;
    ReadStringTag(sModDate, pTagModDate, pTIFFHeader, bLittleEndian);
    ParseDateString(m_dateTime, sModDate);

    uint8* pTagEXIFIFD = FindTag(pIFD0, pLastIFD0, 0x8769, bLittleEndian);
    if (pTagEXIFIFD == nullptr) {
        return;
    }

    uint8* pTagGPSIFD = FindTag(pIFD0, pLastIFD0, 0x8825, bLittleEndian);
    if (pTagGPSIFD != nullptr) {
        ReadGPSData(pTIFFHeader, pTagGPSIFD, nApp1Size, bLittleEndian);
    }

    uint32 nOffsetEXIF = ReadLongTag(pTagEXIFIFD, bLittleEndian);
    if (nOffsetEXIF == 0) {
        return;
    }
    uint8* pEXIFIFD = pTIFFHeader + nOffsetEXIF;
    if (pEXIFIFD - m_pApp1 >= nApp1Size) {
        return;
    }
    nNumTags = ReadUShort(pEXIFIFD, bLittleEndian);
    pEXIFIFD += 2;
    uint8* pLastEXIF = pEXIFIFD + nNumTags * 12;
    if (pLastEXIF - m_pApp1 >= nApp1Size) {
        return;
    }

    uint8* pTagAcquisitionDate = FindTag(pEXIFIFD, pLastEXIF, 0x9003, bLittleEndian);
    QString sAcqDate;
    ReadStringTag(sAcqDate, pTagAcquisitionDate, pTIFFHeader, bLittleEndian);
    ParseDateString(m_acqDate, sAcqDate);

    uint8* pTagExposureTime = FindTag(pEXIFIFD, pLastEXIF, 0x829A, bLittleEndian);
    ReadRationalTag(m_exposureTime, pTagExposureTime, pTIFFHeader, bLittleEndian);

    uint8* pTagExposureBias = FindTag(pEXIFIFD, pLastEXIF, 0x9204, bLittleEndian);
    m_dExposureBias = ReadDoubleTag(pTagExposureBias, pTIFFHeader, bLittleEndian);

    uint8* pTagFlash = FindTag(pEXIFIFD, pLastEXIF, 0x9209, bLittleEndian);
    uint16 nFlash = ReadShortTag(pTagFlash, bLittleEndian);
    m_bFlashFired = (nFlash & 1) != 0;
    m_bFlashFlagPresent = pTagFlash != nullptr;

    uint8* pTagFocalLength = FindTag(pEXIFIFD, pLastEXIF, 0x920A, bLittleEndian);
    m_dFocalLength = ReadDoubleTag(pTagFocalLength, pTIFFHeader, bLittleEndian);

    uint8* pTagFNumber = FindTag(pEXIFIFD, pLastEXIF, 0x829D, bLittleEndian);
    m_dFNumber = ReadDoubleTag(pTagFNumber, pTIFFHeader, bLittleEndian);

    uint8* pTagISOSpeed = FindTag(pEXIFIFD, pLastEXIF, 0x8827, bLittleEndian);
    uint8* pTagISOSpeed2 = FindTag(pEXIFIFD, pLastEXIF, 0x8833, bLittleEndian);

    m_nISOSpeed = (pTagISOSpeed != nullptr) ? ReadShortTag(pTagISOSpeed, bLittleEndian) :
                      ReadLongTag(pTagISOSpeed2, bLittleEndian);

    uint8* pTagUserComment = FindTag(pEXIFIFD, pLastEXIF, 0x9286, bLittleEndian);
    ReadUserCommentTag(m_sUserComment, pTagUserComment, pTIFFHeader, bLittleEndian);
    if (m_sUserComment == QStringLiteral("User comments")) {
        m_sUserComment.clear();
    }

    if (nOffsetIFD1 != 0) {
        m_pIFD1 = pTIFFHeader + nOffsetIFD1;
        if (m_pIFD1 - m_pApp1 >= nApp1Size || m_pIFD1 - m_pApp1 < 0) {
            return;
        }
        nNumTags = ReadUShort(m_pIFD1, bLittleEndian);
        m_pIFD1 += 2;
        uint8* pLastIFD1 = m_pIFD1 + nNumTags * 12;
        if (pLastIFD1 - m_pApp1 >= nApp1Size) {
            return;
        }
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
                uint8* pSOI = pTIFFHeader + nOffsetSOI;
                const uint8* pSOF = reinterpret_cast<const uint8*>(Helpers::FindJPEGMarker(pSOI, nJPEGBytes, 0xC0));
                if (pSOF != nullptr) {
                    m_nThumbWidth = pSOF[7] * 256 + pSOF[8];
                    m_nThumbHeight = pSOF[5] * 256 + pSOF[6];
                    m_nJPEGThumbStreamLen = nJPEGBytes;
                    m_bHasJPEGCompressedThumbnail = true;
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

void CEXIFReader::ReadGPSData(uint8* pTIFFHeader, uint8* pTagGPSIFD, int nApp1Size, bool bLittleEndian) {
    uint32 nOffsetGPS = ReadLongTag(pTagGPSIFD, bLittleEndian);
    if (nOffsetGPS == 0) {
        return;
    }
    uint8* pGPSIFD = pTIFFHeader + nOffsetGPS;
    if (pGPSIFD - m_pApp1 >= nApp1Size) {
        return;
    }
    int nNumTags = ReadUShort(pGPSIFD, bLittleEndian);
    pGPSIFD += 2;
    uint8* pLastGPS = pGPSIFD + nNumTags * 12;
    if (pLastGPS - m_pApp1 >= nApp1Size) {
        return;
    }

    uint8* pTagLatitudeRef = FindTag(pGPSIFD, pLastGPS, 0x1, bLittleEndian);
    if (pTagLatitudeRef == nullptr)
        return;
    QString latitudeRef;
    ReadStringTag(latitudeRef, pTagLatitudeRef, pTIFFHeader, bLittleEndian, false, 2);

    uint8* pTagLatitude = FindTag(pGPSIFD, pLastGPS, 0x2, bLittleEndian);
    m_pLatitude = ReadGPSCoordinate(pTIFFHeader, pTagLatitude, latitudeRef, bLittleEndian);

    uint8* pTagLongitudeRef = FindTag(pGPSIFD, pLastGPS, 0x3, bLittleEndian);
    if (pTagLongitudeRef == nullptr)
        return;
    QString longitudeRef;
    ReadStringTag(longitudeRef, pTagLongitudeRef, pTIFFHeader, bLittleEndian, false, 2);

    uint8* pTagLongitude = FindTag(pGPSIFD, pLastGPS, 0x4, bLittleEndian);
    m_pLongitude = ReadGPSCoordinate(pTIFFHeader, pTagLongitude, longitudeRef, bLittleEndian);

    uint8* pTagAltitude = FindTag(pGPSIFD, pLastGPS, 0x6, bLittleEndian);
    if (pTagAltitude != nullptr) {
        m_dAltitude = ReadDoubleTag(pTagAltitude, pTIFFHeader, bLittleEndian);
        uint8* pTagAltitudeRef = FindTag(pGPSIFD, pLastGPS, 0x5, bLittleEndian);
        if (pTagAltitudeRef != nullptr && *(pTagAltitudeRef + 8) == 1) {
            m_dAltitude *= -1;
        }
    }
}

GPSCoordinate* CEXIFReader::ReadGPSCoordinate(uint8* pTIFFHeader, uint8* pTagLatOrLong, const QString& reference, bool bLittleEndian) {
    if (pTagLatOrLong == nullptr)
        return nullptr;
    double dDeg = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, 0, bLittleEndian);
    double dMin = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, 1, bLittleEndian);
    double dSec = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, 2, bLittleEndian);
    if (dDeg == UNKNOWN_DOUBLE_VALUE || dMin == UNKNOWN_DOUBLE_VALUE || dSec == UNKNOWN_DOUBLE_VALUE)
        return nullptr;

    return new GPSCoordinate(reference, dDeg, dMin, dSec);
}
#include "Helpers.h"
#include "JPEGImage.h"
#include "SettingsProvider.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <QString>
#include <QStringDecoder>
#include <QtEndian>


namespace Helpers {
	
    int ScaleWithDPI(int value)
    {
        return (int)(value * DPIScalingFactor + 0.5f);
    }
	
	LPCTSTR JPEGViewAppDataPath() {
		if (buffAppPath[0] == 0) {
			::SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffAppPath);
			_tcscat_s(buffAppPath, MAX_PATH + 32, _T("\\JPEGView\\"));
		}
		return buffAppPath;
	}

	void SetJPEGViewAppDataPath(LPCTSTR sPath) {
		_tcscpy_s(buffAppPath, MAX_PATH + 32, sPath);
	}

	CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, 
						bool bAllowZoomIn, bool bFillCrop, bool bLimitAR, EAutoZoomMode eAutoZoomMode, double & dZoom) {
		CSettingsProvider& sp = CSettingsProvider::This();
		int nBookModePageHeight = sp.BookModePageHeight();

		if (eAutoZoomMode == ZM_BookMode) {
			double dImageAR = (double)nWidth/nHeight;
			if (dImageAR >= 1.0) {                        // If it's wider than high, assume a double page
				dZoom = (double)nScreenWidth/nWidth;    //    ... and Fit to screen width
			} else {                                    // else, assume a single page
				double dAR1 = ((double)(nHeight*100) / (nBookModePageHeight * nScreenHeight));    // Zoom to user specified height
				double dAR2 = (double)nWidth/nScreenWidth;                                        //    ...but limit maximum image width to window width
				double dAR = std::max(dAR1, dAR2);
				dZoom = 1.0/dAR;
			}

		} else if (eAutoZoomMode == ZM_None) {
			dZoom = 1.0;
		} else {
			double dAR1 = (double)nWidth/nScreenWidth;
			double dAR2 = (double)nHeight/nScreenHeight;
			double dARMin = std::min(dAR1, dAR2);
			double dARMax = std::max(dAR1, dAR2);
			double dAR = bFillCrop ? dARMin : dARMax;

			if (dAR <= 1.0 && !bAllowZoomIn) {
				if (bFillCrop) dAR = dARMax;
				if (dAR <= 1.0) {
					dZoom = 1.0;
					return CSize(nWidth, nHeight);
				}
			}
			if (bFillCrop && bLimitAR) {
				if (((nWidth/dAR * nHeight/dAR) / (nScreenWidth * nScreenHeight)) > 1.5) {
					dAR = dARMax; // use fit to screen
				}
			}
			dZoom = 1.0/dAR;
			dZoom = std::min(ZoomMax, dZoom);
		}

		CSize size((int)(nWidth * dZoom + 0.5), (int)(nHeight * dZoom + 0.5));

		return size;
	}

	CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, EAutoZoomMode eAutoZoomMode, double & dZoom) {
		// Do not comment this out! newSize is unused, but GetImageRect() is needed to calculate dZoom!
		CSize newSize = GetImageRect(nWidth, nHeight, nScreenWidth, nScreenHeight,
			eAutoZoomMode == ZM_FitToScreen || eAutoZoomMode == ZM_FillScreen,
			eAutoZoomMode == ZM_FillScreenNoZoom || eAutoZoomMode == ZM_FillScreen,
			eAutoZoomMode == ZM_FillScreenNoZoom, eAutoZoomMode, dZoom);
		return CSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
	}

	// calculate CRT table
	void CalcCRCTable(unsigned int crc_table[256]) {
		 for (int n = 0; n < 256; n++) {
		   unsigned int c = (unsigned int) n;
		   for (int k = 0; k < 8; k++) {
			 if (c & 1)
			   c = 0xedb88320L ^ (c >> 1);
			 else
			   c = c >> 1;
		   }
		   crc_table[n] = c;
		 }
	}

	const void* FindJPEGMarker(const void* pJPEGStream, int nStreamLength, unsigned char nMarker) {
		const uint8_t* pStream = static_cast<const uint8_t*>(pJPEGStream);
		if (pStream == nullptr || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
			return nullptr; // not a JPEG
		}
		int nIndex = 2;
		do {
			if (pStream[nIndex] == 0xFF) {
				// block header found, skip padding bytes
				while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
				if (nIndex >= nStreamLength) break;

				if (pStream[nIndex] == 0 || pStream[nIndex] == nMarker) {
					break; // 0xFF 0x00 is part of pixel block, break
				} else {
					// it's a block marker, read length of block and skip the block
					nIndex++;
					if (nIndex + 1 < nStreamLength) {
						nIndex += pStream[nIndex] * 256 + pStream[nIndex + 1];
					} else {
						nIndex = nStreamLength;
					}
				}
			} else {
				break; // block with pixel data found
			}
		} while (nIndex < nStreamLength);

		if (nIndex < nStreamLength && (nMarker == 0 || (pStream[nIndex] == nMarker && pStream[nIndex - 1] == 0xFF))) {
			return &(pStream[nIndex - 1]); // place on marker start
		} else {
			return nullptr;
		}
	}

	void* FindEXIFBlock(void* pJPEGStream, int nStreamLength) {
		uint8* pEXIFBlock = (uint8*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xE1);
		if (pEXIFBlock != NULL && strncmp((const char*)(pEXIFBlock + 4), "Exif", 4) != 0) {
			return NULL;
		}
		return pEXIFBlock;
	}

	__int64 CalculateJPEGFileHash(void* pJPEGStream, int nStreamLength) {
		uint8* pStream = (uint8*) pJPEGStream;
        const void* pPixelStart = FindJPEGMarker(pJPEGStream, nStreamLength, 0);
		if (pPixelStart == NULL) {
			return 0;
		}
		int nIndex = (int)((uint8*)pPixelStart - (uint8*)pJPEGStream + 1);
		
		// take whole stream in case of inconsistency or if remaining part is too small
		if (nStreamLength - nIndex < 4) {
			nIndex = 0;
			assert(false);
		}

		// now we can calculate the hash over the compressed pixel data
		// do not look at every byte due to performance reasons
		const int nTotalLookups = 10000;
		int nIncrement = (nStreamLength - nIndex)/nTotalLookups;
		nIncrement = std::max(1, nIncrement);

		unsigned int crc_table[256];
		CalcCRCTable(crc_table);
		uint32 crcValue = 0xffffffff;
		unsigned int sumValue = 0;
		while (nIndex < nStreamLength) {
			sumValue += pStream[nIndex];
			crcValue = crc_table[(crcValue ^ pStream[nIndex]) & 0xff] ^ (crcValue >> 8);
			nIndex += nIncrement;
		}

		return ((__int64)crcValue << 32) + sumValue;
	}


// Versucht streng als validen UTF-8-String zu dekodieren (entspricht TryConvertFromUTF8)
QString TryConvertFromUTF8(const uint8_t* pComment, int nLengthInBytes) {
    auto utf8Decoder = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    
    // Dekodiert den Puffer als UTF-8
    QString result = utf8Decoder(QByteArrayView(reinterpret_cast<const char*>(pComment), nLengthInBytes));

    // Falls ungültige Byte-Sequenzen enthalten waren, schlägt die Validierung fehl
    if (utf8Decoder.hasError()) {
        return {};
    }

    return result;
}

QString GetJPEGComment(const void* pJPEGStream, int nStreamLength) {
    const uint8_t* pCommentSeg = static_cast<const uint8_t*>(FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE));
    if (!pCommentSeg) {
        return {};
    }
    
    pCommentSeg += 2;
    const int nCommentLen = pCommentSeg[0] * 256 + pCommentSeg[1] - 2;
    if (nCommentLen <= 0) {
        return {};
    }

    const uint8_t* pComment = &(pCommentSeg[2]);

    if (nCommentLen > 2 && (nCommentLen & 1) == 0) {
        // UTF-16 Little Endian (BOM 0xFF 0xFE)
        if (pComment[0] == 0xFF && pComment[1] == 0xFE) {
            return QString::fromUtf16(reinterpret_cast<const char16_t*>(&(pComment[2])), (nCommentLen - 2) / 2);
        } 
        // UTF-16 Big Endian (BOM 0xFE 0xFF)
        else if (pComment[0] == 0xFE && pComment[1] == 0xFF) {
            // Optional: Wenn Big-Endian doch unterstützt werden soll, dank Qt6 sehr einfach:
            // return QString::fromUcs2(reinterpret_cast<const char16_t*>(&(pComment[2])), (nCommentLen - 2) / 2, QSysInfo::BigEndian);
            return {};
        }
    }

    // 1. Versuch: UTF-8 Dekodierung
    const QString sConvertedFromUTF8 = TryConvertFromUTF8(pComment, nCommentLen);
    if (!sConvertedFromUTF8.isEmpty()) {
        return sConvertedFromUTF8.contains(QStringLiteral("Intel(R) JPEG Library")) ? QString() : sConvertedFromUTF8;
    }

    // 2. Fallback: ANSI / ASCII Plausibilitätsprüfung
    if (nCommentLen < 7) {
        return {};
    }

    int nGoodChars = 0;
    for (int i = 0; i < nCommentLen; ++i) {
        const uint8_t ch = pComment[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || 
            (ch >= '0' && ch <= '9') || ch == ' ' || ch == ',' || ch == '.') {
            nGoodChars++;
        }
    }

    // Filter für Intel JPEG Library
    const QByteArrayView commentView(reinterpret_cast<const char*>(pComment), nCommentLen);
    if (nCommentLen > 20 && commentView.contains("Intel(R) JPEG Library")) {
        return {};
    }

    // Latin-1 / ANSI Fallback, falls mehr als 80% der Zeichen lesbar sind
    return (nGoodChars > nCommentLen * 8 / 10) 
        ? QString::fromLatin1(reinterpret_cast<const char*>(pComment), nCommentLen) 
        : QString();
}

	CSize GetTotalBorderSize() {
		const int SM_CXP_ADDEDBORDER = 92;  // in MSDN this is SM_CXPADDEDBORDER, but for some reason it's not always available depending on configuration
		int nBorderWidth = (::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2;
		int nBorderHeight = (::GetSystemMetrics(SM_CYSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2 + ::GetSystemMetrics(SM_CYCAPTION);
		return CSize(nBorderWidth, nBorderHeight);
	}

	__int64 GetFileSize(HANDLE hFile) {
		__int64 fileSize = 0;
		::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
		return fileSize;
	}

	// Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
	int GetFrameIndex(CJPEGImage* pImage, int iOffset, bool bSwitchFrame, bool & switchImage) {
		bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
		bool isAnimation = pImage != NULL && pImage->IsAnimation();
		bool isContainer = pImage != NULL && pImage->IsContainer();
		int nFrameIndex = 0;
		switchImage = true;
		if (isMultiFrame) {
			switchImage = false;
			nFrameIndex = pImage->FrameIndex() + iOffset;
			if (isAnimation) {
				if (bSwitchFrame) {
					if (nFrameIndex < 0) {
						nFrameIndex = nFrameIndex + pImage->NumberOfFrames();
					} else if (nFrameIndex > pImage->NumberOfFrames() - 1) {
						nFrameIndex = nFrameIndex - pImage->NumberOfFrames();
					}
				} else {
					switchImage = true;
					nFrameIndex = 0;
				}
			} else if (isContainer) {
				if (nFrameIndex < 0) {
					nFrameIndex = nFrameIndex + pImage->NumberOfFrames();
				} else if (nFrameIndex > pImage->NumberOfFrames() - 1) {
					nFrameIndex = nFrameIndex - pImage->NumberOfFrames();
				}
			} else {
				if (nFrameIndex < 0 || nFrameIndex >= pImage->NumberOfFrames()) {
					nFrameIndex = 0;
					switchImage = true;
				}
			}
		}
		if (bSwitchFrame && pImage == NULL) {
			switchImage = false; // never switch image when error during animation playing
		}

		return nFrameIndex;
	}

// Based on: https://github.com/sdneon/jpegview.git
    void BlendAlpha(uint32* pImage32, int nWidth, int nHeight, Helpers::ETransparencyMode nTransparencyMode) {
        COLORREF nTransparency = CSettingsProvider::This().ColorTransparency();
        if (nTransparencyMode != Helpers::TP_CHECKERBOARD) {
            if (nTransparencyMode == Helpers::TP_BLEND_INVERSE) {
                nTransparency = ~(nTransparency & 0xffffff);
            }
            for (int i = 0; i < nWidth * nHeight; ++i) {
                *pImage32 = AlphaBlendBackground(*pImage32, nTransparency);
                pImage32++;
            }
        } else {
            for (int y = 0; y < nHeight; ++y) {
                for (int x = 0; x < nWidth; ++x) {
                    *pImage32 = AlphaBlendBackground(*pImage32, nTransparency, true, x, y);
                    pImage32++;
                }
            }
        }
    }

    uint32_t GetTimeMs() {
        using namespace std::chrono;
        auto now = steady_clock::now();
        return static_cast<uint32_t>(duration_cast<milliseconds>(now.time_since_epoch()).count());
    }

} // namespace Helpers

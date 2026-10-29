#ifndef EXIFREADER_H
#define EXIFREADER_H

#include "ImageProcessingTypes.h"
#include <QString>
#include <QDateTime>

// Signed rational number: numerator/denominator
class SignedRational {
public:
    SignedRational(int num, int denom) { Numerator = num; Denominator = denom; }
    int Numerator;
    int Denominator;
};

// Unsigned rational number: numerator/denominator
class Rational {
public:
    Rational(unsigned int num, unsigned int denom) { Numerator = num; Denominator = denom; }
    unsigned int Numerator;
    unsigned int Denominator;
};

class GPSCoordinate {
public:
    GPSCoordinate(const QString& reference, double degrees, double minutes, double seconds) {
        m_sReference = reference;
        if (minutes == 0.0 && seconds == 0.0) {
            minutes = 60 * abs(degrees - (int)degrees);
            degrees = (int)degrees;
        }
        if (seconds == 0.0) {
            seconds = 60 * abs(minutes - (int)minutes);
            minutes = (int)minutes;
        }
        Degrees = degrees;
        Minutes = minutes;
        Seconds = seconds;
    }
    QString GetReference() const { return m_sReference; }
    double Degrees;
    double Minutes;
    double Seconds;
private:
    QString m_sReference;
};

// Reads and parses the EXIF data of JPEG images
class CEXIFReader {
public:
    // The pApp1Block must point to the APP1 block of the EXIF data, including the APP1 block marker.
    // nBlockSize is the size of the buffer: nothing beyond it is read, whatever the data says.
    // The class does not take ownership of the memory (no copy made), thus the APP1 block must not be deleted
    // while the EXIF reader class is deleted.
    CEXIFReader(void* pApp1Block, int nBlockSize, EImageFormat eImageFormat);
    ~CEXIFReader(void);

    // Parse date string in the EXIF date/time format
    static bool ParseDateString(QDateTime & date, const QString& str);

public:
    QString GetCameraModel() const { return m_sModel; }
    QString GetUserComment() const { return m_sUserComment; }
    QString GetImageDescription() const { return m_sImageDescription; }
    QString GetSoftware() const { return m_sSoftware; }
    bool GetCameraModelPresent() const { return !m_sModel.isEmpty(); }
    bool GetSoftwarePresent() const { return !m_sSoftware.isEmpty(); }

    const QDateTime& GetAcquisitionTime() const { return m_acqDate; }
    bool GetAcquisitionTimePresent() const { return m_acqDate.isValid(); }

    const QDateTime& GetDateTime() const { return m_dateTime; }
    bool GetDateTimePresent() const { return m_dateTime.isValid(); }

    const Rational& GetExposureTime() const { return m_exposureTime; }
    bool GetExposureTimePresent() const { return m_exposureTime.Denominator != 0; }

    double GetExposureBias() const { return m_dExposureBias; }
    bool GetExposureBiasPresent() const { return m_dExposureBias != UNKNOWN_DOUBLE_VALUE; }

    bool GetFlashFired() const { return m_bFlashFired; }
    bool GetFlashFiredPresent() const { return m_bFlashFlagPresent; }

    double GetFocalLength() const { return m_dFocalLength; }
    bool GetFocalLengthPresent() const { return m_dFocalLength != UNKNOWN_DOUBLE_VALUE; }

    double GetFNumber() const { return m_dFNumber; }
    bool GetFNumberPresent() const { return m_dFNumber != UNKNOWN_DOUBLE_VALUE; }

    int GetISOSpeed() const { return m_nISOSpeed; }
    bool GetISOSpeedPresent() const { return m_nISOSpeed > 0; }

    int GetImageOrientation() const { return m_nImageOrientation; }
    bool ImageOrientationPresent() const { return m_nImageOrientation > 0; }

    int GetThumbnailWidth() const { return m_nThumbWidth; }
    int GetThumbnailHeight() const { return m_nThumbHeight; }

    bool IsGPSInformationPresent() const { return m_pLatitude != nullptr && m_pLongitude != nullptr; }
    bool IsGPSAltitudePresent() const { return m_dAltitude != UNKNOWN_DOUBLE_VALUE; }
    GPSCoordinate* GetGPSLatitude() const { return m_pLatitude; }
    GPSCoordinate* GetGPSLongitude() const { return m_pLongitude; }
    double GetGPSAltitude() const { return m_dAltitude; }

    static double UNKNOWN_DOUBLE_VALUE;

private:
    QString m_sModel;
    QString m_sUserComment;
    QString m_sImageDescription;
    QString m_sSoftware;
    QDateTime m_acqDate;
    QDateTime m_dateTime;
    Rational m_exposureTime;
    double m_dExposureBias;
    bool m_bFlashFired;
    bool m_bFlashFlagPresent;
    double m_dFocalLength;
    double m_dFNumber;
    int m_nISOSpeed;
    int m_nImageOrientation;
    int m_nThumbWidth;
    int m_nThumbHeight;
    GPSCoordinate* m_pLatitude;
    GPSCoordinate* m_pLongitude;
    double m_dAltitude;

    bool m_bLittleEndian;
    uint8* m_pApp1;
    uint8* m_pTagOrientation;
    uint8* m_pLastIFD0;
    uint8* m_pIFD1;
    uint8* m_pLastIFD1;

    // nTIFFSize: number of bytes of EXIF data from pTIFFHeader on (offsets in the data are checked against it)
    void ReadGPSData(uint8* pTIFFHeader, uint32 nTIFFSize, uint8* pTagGPSIFD, bool bLittleEndian);
    GPSCoordinate* ReadGPSCoordinate(uint8* pTIFFHeader, uint32 nTIFFSize, uint8* pTagLatOrLong, const QString& reference, bool bLittleEndian);
};

#endif // EXIFREADER_H
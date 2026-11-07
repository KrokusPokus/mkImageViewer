#include "Clipboard.h"

#include "BaseImage.h"
#include "GeometryTypes.h"
#include "Helpers.h"

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QString>
#include <QUrl>

void CClipboard::CopyImageToClipboard(CBaseImage* pImage, const QString& fileName) {
    if (pImage == nullptr || pImage->BitmapPixelsLastProcessed(true) == nullptr) {
        return;
    }

    DoCopy(pImage->BitmapWidth(), pImage->BitmapHeight(), pImage->BitmapPixelsLastProcessed(true), fileName);
}

void CClipboard::CopyFullImageToClipboard(CBaseImage* pImage, const CImageProcessingParams& procParams, EProcessingFlags eFlags, const QString& fileName) {
    if (pImage == nullptr) {
        return;
    }

    void* pBitmap = pImage->GetBitmap(pImage->OrigSize(), pImage->OrigSize(), DPoint(0, 0), procParams, eFlags);
    DoCopy(pImage->OrigWidth(), pImage->OrigHeight(), pBitmap, fileName);
}

void CClipboard::CopyPathToClipboard(CBaseImage* pImage, const QString& fileName) {
    if (pImage == nullptr || pImage->BitmapPixelsLastProcessed(true) == nullptr) {
        return;
    }
    DoCopyFileNameText(fileName);
}

void CClipboard::CutFileToClipboard(const QString& fileName) {
    if (fileName.isEmpty()) {
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return;
    }

    QMimeData* mimeData = new QMimeData();
    QUrl fileUrl = QUrl::fromLocalFile(fileName);

    // 1. Standard-Datei-URL Liste
    mimeData->setUrls({ fileUrl });

    // 2. KDE Cut-Marker
    mimeData->setData("application/x-kde-cutselection", "1");

    // 3. Freedesktop Standard (GNOME/Nautilus/Dolphin)
    // Format: "cut\nfile:///pfad/zur/datei.jpg\n"
    QByteArray gnomePayload = ("cut\n" + fileUrl.toString() + "\n").toUtf8();
    mimeData->setData("x-special/gnome-copied-files", gnomePayload);

    // Qt übernimmt die Speicherverwaltung von mimeData
    clipboard->setMimeData(mimeData);
}

void CClipboard::DoCopy(int nWidth, int nHeight, const void* pSourceImageBitmap32, const QString& fileName) {
    if (pSourceImageBitmap32 == nullptr || nWidth <= 0 || nHeight <= 0) {
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return;
    }

    // 1. QImage aus dem 32bpp-bitmap-Puffer (BGRA) erzeugen und KOPIEREN: Der Puffer gehört dem CBaseImage und wird
    // beim nächsten Zoom, Pan oder Bildwechsel freigegeben. Die Zwischenablage wandelt das Bild aber erst um, wenn
    // ein anderes Programm einfügt - ohne Kopie läse sie dann freigegebenen Speicher.
    // Format_RGB32 wie beim Zeichnen: Das Alpha-Byte des bitmap ist nicht in jedem Fall gesetzt (z. B. 32-Bit-BMP mit
    // Alpha 0), als ARGB wäre das Bild in der Zwischenablage dann durchsichtig.
    const QImage image = QImage(static_cast<const uchar*>(pSourceImageBitmap32), nWidth, nHeight, nWidth * 4, QImage::Format_RGB32).copy();
    if (image.isNull()) {
        return; // the copy could not be allocated
    }

    QMimeData* mimeData = new QMimeData();

    // Bild-Daten setzen (Qt erzeugt intern image/png / image/bmp etc.)
    mimeData->setImageData(image);

    // 2. Falls ein Dateiname vorhanden ist, auch als Datei-URL anhängen
    if (!fileName.isEmpty()) {
        mimeData->setUrls({ QUrl::fromLocalFile(fileName) });
    }

    clipboard->setMimeData(mimeData);
}

void CClipboard::DoCopyFileNameText(const QString& fileName) {
    if (fileName.isEmpty()) {
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return;
    }

    // Setzt sowohl Plain-Text als auch die Datei-URL in der Zwischenablage
    QMimeData* mimeData = new QMimeData();
    mimeData->setText(fileName);
    mimeData->setUrls({ QUrl::fromLocalFile(fileName) });

    clipboard->setMimeData(mimeData);
}

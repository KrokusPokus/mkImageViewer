#include "Clipboard.h"
#include "BaseImage.h"
#include "Helpers.h"

#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QImage>
#include <QUrl>
#include <QByteArray>
#include <QString>
#include <algorithm>

void CClipboard::CopyImageToClipboard(CBaseImage* pImage, const QString& fileName) {
    if (pImage == nullptr || pImage->DIBPixelsLastProcessed(true) == nullptr) {
        return;
    }

    DoCopy(pImage->DIBWidth(), pImage->DIBHeight(), pImage->DIBPixelsLastProcessed(true), fileName);
}

void CClipboard::CopyFullImageToClipboard(CBaseImage* pImage, const CImageProcessingParams& procParams, EProcessingFlags eFlags, const QString& fileName) {
    CopyFullImageToClipboard(pImage, procParams, eFlags, CRect(0, 0, pImage->OrigWidth(), pImage->OrigHeight()), fileName);
}

void CClipboard::CopyFullImageToClipboard(CBaseImage* pImage, const CImageProcessingParams& procParams, EProcessingFlags eFlags, CRect clipRect, const QString& fileName) {
    if (pImage == nullptr) {
        return;
    }

    clipRect.left = std::max<int>(0, clipRect.left);
    clipRect.top = std::max<int>(0, clipRect.top);
    clipRect.right = std::min<int>(pImage->OrigWidth(), clipRect.right);
    clipRect.bottom = std::min<int>(pImage->OrigHeight(), clipRect.bottom);

    void* pDIB = pImage->GetDIB(pImage->OrigSize(), clipRect.Size(), clipRect.TopLeft(), procParams, eFlags);
    DoCopy(clipRect.Width(), clipRect.Height(), pDIB, fileName);
}

void CClipboard::CopyPathToClipboard(CBaseImage* pImage, const QString& fileName) {
    if (pImage == nullptr || pImage->DIBPixelsLastProcessed(true) == nullptr) {
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

    // 1. Standard-Datei-URL Liste (Cross-Platform)
    mimeData->setUrls({ fileUrl });

    // 2. Windows-spezifischer Preferred DropEffect (Ausschneiden/Verschieben)
    // 0x02 = DROPEFFECT_MOVE
    QByteArray dropEffect(4, 0);
    dropEffect[0] = 0x02;
    mimeData->setData("Preferred DropEffect", dropEffect);

    // 3. Linux / KDE Cut-Marker
    mimeData->setData("application/x-kde-cutselection", "1");

    // 4. Linux / Freedesktop Standard (GNOME/Nautilus/Dolphin)
    // Format: "cut\nfile:///pfad/zur/datei.jpg\n"
    QByteArray gnomePayload = ("cut\n" + fileUrl.toString() + "\n").toUtf8();
    mimeData->setData("x-special/gnome-copied-files", gnomePayload);

    // Qt übernimmt die Speicherverwaltung von mimeData
    clipboard->setMimeData(mimeData);
}

void CClipboard::DoCopy(int nWidth, int nHeight, const void* pSourceImageDIB32, const QString& fileName) {
    if (pSourceImageDIB32 == nullptr || nWidth <= 0 || nHeight <= 0) {
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return;
    }

    // 1. QImage aus dem 32bpp-DIB-Puffer (BGRA) erzeugen und KOPIEREN: Der Puffer gehört dem CBaseImage und wird
    // beim nächsten Zoom, Pan oder Bildwechsel freigegeben. Die Zwischenablage wandelt das Bild aber erst um, wenn
    // ein anderes Programm einfügt - ohne Kopie läse sie dann freigegebenen Speicher.
    // Format_RGB32 wie beim Zeichnen: Das Alpha-Byte des DIB ist nicht in jedem Fall gesetzt (z. B. 32-Bit-BMP mit
    // Alpha 0), als ARGB wäre das Bild in der Zwischenablage dann durchsichtig.
    const QImage image = QImage(static_cast<const uchar*>(pSourceImageDIB32), nWidth, nHeight, nWidth * 4, QImage::Format_RGB32).copy();
    if (image.isNull()) {
        return; // the copy could not be allocated
    }

    QMimeData* mimeData = new QMimeData();

    // Bild-Daten setzen (Qt erzeugt intern CF_DIB / image/png / image/bmp etc.)
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

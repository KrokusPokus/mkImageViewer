#include "RenameDialog.h"

#include <QCollator>
#include <QDir>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QColor>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>
#include <algorithm>
#include <filesystem>

RenameDialog::RenameDialog(const QString& currentFilePath, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint),
      m_currentFileInfo(currentFilePath) {
    
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi();
}

QString RenameDialog::newFilePath() const {
    return m_newFilePath;
}

QColor RenameDialog::calculateFadedColor(int distance) const {
    QColor bgColor = palette().color(QPalette::Window);
    QColor textColor = palette().color(QPalette::WindowText);

    // Asymptotische Annäherung an die Hintergrundfarbe wie im AHK-Original (0.6 ^ abs(distance))
    double fadeFactor = qPow(0.6, std::abs(distance));

    int r = qRound(bgColor.red() + (textColor.red() - bgColor.red()) * fadeFactor);
    int g = qRound(bgColor.green() + (textColor.green() - bgColor.green()) * fadeFactor);
    int b = qRound(bgColor.blue() + (textColor.blue() - bgColor.blue()) * fadeFactor);

    return QColor(r, g, b);
}

void RenameDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(4);

    QDir dir = m_currentFileInfo.dir();
    QFileInfoList fileList = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);

    // Logische Sortierung der Dateien im Ordner (1, 2, ..., 10 statt 1, 10, 2)
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(fileList.begin(), fileList.end(), [&collator](const QFileInfo& a, const QFileInfo& b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    // Aktuellen Index ermitteln
    int targetIndex = -1;
    for (int i = 0; i < fileList.size(); ++i) {
        if (fileList[i].absoluteFilePath() == m_currentFileInfo.absoluteFilePath()) {
            targetIndex = i;
            break;
        }
    }

    if (targetIndex == -1) return;

    int startIndex = std::max(0, targetIndex - SIBLING_COUNT);
    int endIndex = std::min(static_cast<int>(fileList.size()) - 1, targetIndex + SIBLING_COUNT);

    for (int i = startIndex; i <= endIndex; ++i) {
        int distance = i - targetIndex;
        const QFileInfo& fi = fileList[i];
        QString baseName = fi.completeBaseName();
        QString ext = fi.suffix().isEmpty() ? "" : "." + fi.suffix();

        auto* rowLayout = new QHBoxLayout();
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        if (distance == 0) {
            // Aktuelle Datei: Editierbares Feld + Statische Endung
            m_nameEdit = new QLineEdit(baseName, this);
            m_nameEdit->setAlignment(Qt::AlignRight);
            m_nameEdit->selectAll();

            auto* extLabel = new QLabel(ext, this);
            extLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            rowLayout->addWidget(m_nameEdit);
            rowLayout->addWidget(extLabel);
            
            // Abstands-Feinjustierung analog zu y+11
            mainLayout->addSpacing(6);
            mainLayout->addLayout(rowLayout);
            mainLayout->addSpacing(6);
        } else {
            // Umlegende Dateien: Ausgeblendete Text-Labels
            auto* nameLabel = new QLabel(baseName, this);
            nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            auto* extLabel = new QLabel(ext, this);
            extLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

            QColor color = calculateFadedColor(distance);
            QString style = QString("color: %1;").arg(color.name());
            nameLabel->setStyleSheet(style);
            extLabel->setStyleSheet(style);

            rowLayout->addWidget(nameLabel);
            rowLayout->addWidget(extLabel);

            mainLayout->addLayout(rowLayout);
        }
    }

    adjustSize();
}

// Key-Handling für Enter (Speichern) und Escape (Abbrechen)
void RenameDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!m_nameEdit) return;

        QString newBaseName = m_nameEdit->text().trimmed();
        QString oldBaseName = m_currentFileInfo.completeBaseName();

        if (newBaseName.isEmpty() || newBaseName == oldBaseName) {
            reject();
            return;
        }

        QString ext = m_currentFileInfo.suffix().isEmpty() ? "" : "." + m_currentFileInfo.suffix();
        QString newFileName = newBaseName + ext;
        QDir dir = m_currentFileInfo.dir();
        m_newFilePath = dir.filePath(newFileName);

        // Dateiexistenzprüfung (wie im AHK-Original)
        if (QFile::exists(m_newFilePath)) {
            QMessageBox::warning(this, "Fehler", "Eine Datei mit diesem Namen existiert bereits.");
            return;
        }

        std::error_code ec;
        std::filesystem::rename(m_currentFileInfo.absoluteFilePath().toStdString(),
                                 m_newFilePath.toStdString(), ec);

        if (ec) {
            QMessageBox::critical(this, "Fehler", QString::fromStdString(ec.message()));
            return;
        }

        accept();
    } else if (event->key() == Qt::Key_Escape) {
        reject();
    } else {
        QDialog::keyPressEvent(event);
    }
}

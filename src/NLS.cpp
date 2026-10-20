#include "NLS.h"
#include "SettingsProvider.h"

#include <QFile>
#include <QTextStream>
#include <QLocale>
#include <QDir>

QHash<QString, QString> CNLS::sm_texts;
bool CNLS::sm_bTableRead = false;

QString CNLS::GetLocalizedFileName(const QString& sPath, const QString& sPrefixName, const QString& sExtension, const QString& sLanguageCode) {
    QString sNLSFile;
    QString languageCode = sLanguageCode;

    // Automatische Spracherkennung über Qt statt Windows-Thread-Locales
    if (languageCode.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        QString sysName = QLocale::system().name(); // z.B. "de_DE"
        QString langPart = sysName.section(QLatin1Char('_'), 0, 0);
        QString ctryPart = sysName.section(QLatin1Char('_'), 1, 1);

        QString basePath = sPath;
        if (!basePath.endsWith('/') && !basePath.endsWith('\\')) {
            basePath += QLatin1Char('/');
        }

        if (!langPart.isEmpty() && !ctryPart.isEmpty()) {
            sNLSFile = basePath + sPrefixName + QStringLiteral("_") + langPart + QLatin1Char('-') + ctryPart + QLatin1Char('.') + sExtension;
            if (!QFile::exists(sNLSFile)) {
                sNLSFile.clear();
            }
        }
        if (sNLSFile.isEmpty() && !langPart.isEmpty()) {
            sNLSFile = basePath + sPrefixName + QStringLiteral("_") + langPart + QLatin1Char('.') + sExtension;
            if (!QFile::exists(sNLSFile)) {
                sNLSFile.clear();
            }
        }
        languageCode = langPart;
    }

    if (sNLSFile.isEmpty()) {
        QString basePath = sPath;
        if (!basePath.endsWith('/') && !basePath.endsWith('\\')) {
            basePath += QLatin1Char('/');
        }
        sNLSFile = basePath + sPrefixName + QStringLiteral("_") + languageCode + QLatin1Char('.') + sExtension;
        if (!QFile::exists(sNLSFile)) {
            sNLSFile.clear();
        }
    }

    return sNLSFile;
}

QString CNLS::GetStringTableFileName(const QString& sLanguageCode) {
    return GetLocalizedFileName(CSettingsProvider::This().GetEXEPath(), QStringLiteral("strings"), QStringLiteral("txt"), sLanguageCode);
}

// Liest die String-Tabelle plattformunabhängig per QFile und QTextStream (UTF-8) ein
void CNLS::ReadStringTable(const QString& sFileName) {
    QFile file(sFileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    while (!in.atEnd()) {
        QString line = in.readLine();
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("//"))) {
            continue; // Kommentar- oder Leerzeilen überspringen
        }

        int tabIdx = line.indexOf(QChar('\t'));
        if (tabIdx != -1) {
            QString key = line.left(tabIdx).trimmed();
            QString translation = line.mid(tabIdx + 1).trimmed();
            if (!key.isEmpty() && !translation.isEmpty()) {
                sm_texts[key] = translation;
            }
        }
    }

    file.close();
    sm_bTableRead = true;
}

QString CNLS::GetString(const QString& sString) {
    if (!sm_bTableRead || sString.isEmpty()) {
        return sString; // Keine Übersetzung geladen, Original zurückgeben
    }

    auto iter = sm_texts.find(sString);
    if (iter == sm_texts.end()) {
        return sString; // Nicht gefunden, Original zurückgeben
    } else {
        return iter.value();
    }
}

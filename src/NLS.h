#ifndef NLS_H
#define NLS_H

#pragma once

#include <QString>
#include <QHash>

class CNLS {
public:
    static QString GetLocalizedFileName(const QString& sPath, const QString& sPrefixName, const QString& sExtension, const QString& sLanguageCode);
    static QString GetStringTableFileName(const QString& sLanguageCode);

    static void ReadStringTable(const QString& sFileName);
    static QString GetString(const QString& sString);

private:
    // Modernes, plattformunabhängiges Mapping von QString zu QString mittels QHash
    static QHash<QString, QString> sm_texts;
    static bool sm_bTableRead;
};

#endif // NLS_H

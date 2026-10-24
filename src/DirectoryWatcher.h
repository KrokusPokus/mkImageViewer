#ifndef DIRECTORYWATCHER_H
#define DIRECTORYWATCHER_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTimer>

class CDirectoryWatcher : public QObject {
    Q_OBJECT
public:
    explicit CDirectoryWatcher(QObject* pParent = nullptr);
    virtual ~CDirectoryWatcher();

    void SetCurrentFile(const QString& fileName);
    void SetCurrentDirectory(const QString& directoryName);
    void Abort();

signals:
    void DirectoryFileListChanged();
    void DisplayedFileChangedOnDisk();

private:
    QString m_sCurrentDirectory;
    QString m_sCurrentFile;

    bool m_bModificationTimeValid{false};
    QDateTime m_modificationTimeCurrentFile;
    bool m_aborted{false}; // Plattformübergreifend verfügbar

    static QDateTime GetLastModificationTime(const QString& fileName);

    QFileSystemWatcher* m_fileSystemWatcher{nullptr};
    QString m_watchedDirectory;
};

#endif // DIRECTORYWATCHER_H
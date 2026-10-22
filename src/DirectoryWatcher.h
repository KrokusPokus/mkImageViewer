#ifndef DIRECTORYWATCHER_H
#define DIRECTORYWATCHER_H

#include <QObject>
#include <QString>
#include <QDateTime>

#if defined(Q_OS_WIN)
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#else
#include <QFileSystemWatcher>
#include <QTimer>
#endif

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

#if defined(Q_OS_WIN)
    QMutex m_lock;
    QThread* m_workerThread{nullptr};
    QWaitCondition m_waitCondition;
    bool m_terminateRequested{false};
    bool m_newDirectoryRequested{false};

    void RunThread();
#else
    QFileSystemWatcher* m_fileSystemWatcher{nullptr};
    QString m_watchedDirectory;
#endif
};

#endif // DIRECTORYWATCHER_H
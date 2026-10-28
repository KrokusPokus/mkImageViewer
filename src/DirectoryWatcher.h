#ifndef DIRECTORYWATCHER_H
#define DIRECTORYWATCHER_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QTimer>

class CDirectoryWatcher : public QObject {
    Q_OBJECT

signals:
    void DirectoryFileListChanged();
    void DisplayedFileChangedOnDisk();

public:
    explicit CDirectoryWatcher(QObject* pParent = nullptr);
    virtual ~CDirectoryWatcher();

    void SetCurrentFile(const QString& fileName);
    void SetCurrentDirectory(const QString& directoryName);
    void Abort();
    void Suspend();
    void Resume();

    // RAII: Watcher fuer die Lebensdauer des Objekts pausieren
    class ScopedPause {
    public:
        explicit ScopedPause(CDirectoryWatcher& w) : m_w(w) { m_w.Suspend(); }
        ~ScopedPause() { m_w.Resume(); }
        ScopedPause(const ScopedPause&) = delete;
        ScopedPause& operator=(const ScopedPause&) = delete;
    private:
        CDirectoryWatcher& m_w;
    };

private:
    bool m_suspended = false;
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

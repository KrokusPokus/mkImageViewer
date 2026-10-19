#ifndef DIRECTORYWATCHER_H
#define DIRECTORYWATCHER_H

#include <QString>
#include <QDateTime>

class CDirectoryWatcher {
public:
    explicit CDirectoryWatcher(HWND hTargetWindow);
    virtual ~CDirectoryWatcher();

    void SetCurrentFile(const QString& fileName);
    void SetCurrentDirectory(const QString& directoryName);

    // First tries to terminate the thread by setting m_terminateEvent, when no reaction -> kills the thread
    void Abort();

private:
    CRITICAL_SECTION m_lock;
    HANDLE m_terminateEvent{ NULL };
    HANDLE m_newDirectoryEvent{ NULL };
    HANDLE m_hThread{ NULL };
    HWND m_hTargetWindow{ NULL };

    QString m_sCurrentDirectory;
    QString m_sCurrentFile;

    bool m_bModificationTimeValid{ false };
    QDateTime m_modificationTimeCurrentFile;

    static QDateTime GetLastModificationTime(const QString& fileName);
    static void __cdecl ThreadFunc(void* arg);
};

#endif // DIRECTORYWATCHER_H
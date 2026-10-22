#include "DirectoryWatcher.h"

#include <QFileInfo>
#include <QDir>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

/////////////////////////////////////////////////////////////////////////////////////////////
// Helpers
/////////////////////////////////////////////////////////////////////////////////////////////

QDateTime CDirectoryWatcher::GetLastModificationTime(const QString& fileName) {
    if (fileName.isEmpty()) {
        return {};
    }
    const QFileInfo fi(fileName);
    return fi.exists() ? fi.lastModified() : QDateTime{};
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Plattformübergreifender Konstruktor & Destruktor (mit Guards)
/////////////////////////////////////////////////////////////////////////////////////////////

#if defined(Q_OS_WIN)

CDirectoryWatcher::CDirectoryWatcher(QObject* pParent)
    : QObject(pParent)
{
    m_workerThread = QThread::create([this]() {
        this->RunThread();
    });
    m_workerThread->start();
}

CDirectoryWatcher::~CDirectoryWatcher() {
    Abort();
}

void CDirectoryWatcher::Abort() {
    {
        QMutexLocker locker(&m_lock);
        m_terminateRequested = true;
        m_aborted = true;
    }
    m_waitCondition.wakeAll();

    if (m_workerThread != nullptr) {
        m_workerThread->wait();
        delete m_workerThread;
        m_workerThread = nullptr;
    }
}

void CDirectoryWatcher::SetCurrentFile(const QString& fileName) {
    // I/O außerhalb des Locks holen, um Blockaden (z.B. bei Netzwerkfreigaben) zu vermeiden
    const QString clean = QDir::cleanPath(fileName);
    const QDateTime t = GetLastModificationTime(clean);

    QMutexLocker locker(&m_lock);
    if (m_aborted) return;
    m_sCurrentFile = clean;
    m_modificationTimeCurrentFile = t;
    m_bModificationTimeValid = t.isValid();
}

void CDirectoryWatcher::SetCurrentDirectory(const QString& directoryName) {
    const QString fullPath = QFileInfo(directoryName).absoluteFilePath();

    {
        QMutexLocker locker(&m_lock);
        if (m_aborted) return;
        m_sCurrentDirectory = fullPath;
        m_newDirectoryRequested = true;
    }
    m_waitCondition.wakeAll();
}

void CDirectoryWatcher::RunThread() {
    bool bSetupNewDirectory = true;
    HANDLE changeHandles[2]{ nullptr, nullptr };

    auto closeChangeHandles = [&changeHandles]() {
        if (changeHandles[0] != nullptr && changeHandles[0] != INVALID_HANDLE_VALUE) {
            ::FindCloseChangeNotification(changeHandles[0]);
            changeHandles[0] = nullptr;
        }
        if (changeHandles[1] != nullptr && changeHandles[1] != INVALID_HANDLE_VALUE) {
            ::FindCloseChangeNotification(changeHandles[1]);
            changeHandles[1] = nullptr;
        }
    };

    auto sleepInterruptible = [this](unsigned long ms) {
        QMutexLocker l(&m_lock);
        if (!m_terminateRequested && !m_newDirectoryRequested)
            m_waitCondition.wait(&m_lock, ms);
        return m_terminateRequested;
    };

    while (true) {
        {
            QMutexLocker locker(&m_lock);
            if (m_terminateRequested) {
                break;
            }
        }

        if (bSetupNewDirectory) {
            closeChangeHandles();

            QString dirToWatch;
            {
                QMutexLocker locker(&m_lock);
                dirToWatch = m_sCurrentDirectory;
                m_newDirectoryRequested = false;
            }

            if (!dirToWatch.isEmpty()) {
                const auto dirWStr = reinterpret_cast<LPCWSTR>(dirToWatch.utf16());
                changeHandles[0] = ::FindFirstChangeNotificationW(dirWStr, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
                changeHandles[1] = ::FindFirstChangeNotificationW(dirWStr, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
            }
            bSetupNewDirectory = false;
        }

        HANDLE activeHandles[2];
        int map[2];
        int activeCount = 0;

        if (changeHandles[0] != nullptr && changeHandles[0] != INVALID_HANDLE_VALUE) {
            map[activeCount] = 0;
            activeHandles[activeCount++] = changeHandles[0];
        }
        if (changeHandles[1] != nullptr && changeHandles[1] != INVALID_HANDLE_VALUE) {
            map[activeCount] = 1;
            activeHandles[activeCount++] = changeHandles[1];
        }

        DWORD waitStatus = WAIT_TIMEOUT;
        if (activeCount > 0) {
            waitStatus = ::WaitForMultipleObjects(activeCount, activeHandles, FALSE, 250);
        } else {
            if (sleepInterruptible(250)) break;
        }

        {
            QMutexLocker locker(&m_lock);
            if (m_newDirectoryRequested) {
                bSetupNewDirectory = true;
                continue;
            }
        }

        if (waitStatus == WAIT_FAILED) {
            bSetupNewDirectory = true;
            sleepInterruptible(250);
            continue;
        }

        const int which = (waitStatus >= WAIT_OBJECT_0 && waitStatus < WAIT_OBJECT_0 + (DWORD)activeCount)
                              ? map[waitStatus - WAIT_OBJECT_0] : -1;

        if (activeCount > 0 && which != -1) {
            if (which == 0) {
                if (::FindNextChangeNotification(changeHandles[0]) == FALSE) {
                    bSetupNewDirectory = true;
                }
                emit DirectoryFileListChanged();
                if (sleepInterruptible(500)) break;
            }
            else if (which == 1) {
                if (::FindNextChangeNotification(changeHandles[1]) == FALSE) {
                    bSetupNewDirectory = true;
                }
                if (sleepInterruptible(250)) break;

                QString file;
                {
                    QMutexLocker l(&m_lock);
                    file = m_sCurrentFile;
                }

                bool sendMessage = false;
                if (!file.isEmpty()) {
                    QDateTime newTime = GetLastModificationTime(file);
                    if (!newTime.isValid() && !sleepInterruptible(250)) {
                        newTime = GetLastModificationTime(file);
                    }

                    if (newTime.isValid()) {
                        QMutexLocker l(&m_lock);
                        // Gegen aktuellen Dateinamen & Wert prüfen, um veraltete Vergleiche zu verhindern
                        if (m_sCurrentFile == file && newTime != m_modificationTimeCurrentFile) {
                            m_modificationTimeCurrentFile = newTime;
                            sendMessage = true;
                        }
                    }
                }

                if (sendMessage) {
                    emit DisplayedFileChangedOnDisk();
                }
            }
        }
    }

    closeChangeHandles();
}

#else // Für Linux / macOS (Non-Windows)

CDirectoryWatcher::CDirectoryWatcher(QObject* pParent)
    : QObject(pParent)
{
    m_fileSystemWatcher = new QFileSystemWatcher(this);

    connect(m_fileSystemWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
        if (!m_aborted) {
            emit DirectoryFileListChanged();
        }
    });

    connect(m_fileSystemWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (m_aborted || path != m_sCurrentFile) {
            return;
        }

        // Atomares Speichern oder Löschen+Neuschreiben (Delete-and-Recreate) abfangen
        if (!m_fileSystemWatcher->files().contains(path)) {
            if (QFileInfo::exists(path)) {
                m_fileSystemWatcher->addPath(path);
            } else {
                // Verzögerter Retry für Tools, die Dateien kurzzeitig löschen und neu anlegen
                QTimer::singleShot(200, this, [this, path]() {
                    if (m_aborted || path != m_sCurrentFile) return;
                    if (QFileInfo::exists(path) && !m_fileSystemWatcher->files().contains(path)) {
                        m_fileSystemWatcher->addPath(path);
                        const QDateTime t = GetLastModificationTime(path);
                        if (m_bModificationTimeValid && t.isValid() && t != m_modificationTimeCurrentFile) {
                            m_modificationTimeCurrentFile = t;
                            emit DisplayedFileChangedOnDisk();
                        }
                    }
                });
            }
        }

        if (m_bModificationTimeValid) {
            QDateTime newTime = GetLastModificationTime(m_sCurrentFile);
            if (newTime.isValid() && newTime != m_modificationTimeCurrentFile) {
                m_modificationTimeCurrentFile = newTime;
                emit DisplayedFileChangedOnDisk();
            }
        }
    });
}

CDirectoryWatcher::~CDirectoryWatcher() {
    Abort();
}

void CDirectoryWatcher::Abort() {
    m_aborted = true;
    if (m_fileSystemWatcher) {
        QStringList paths = m_fileSystemWatcher->files();
        if (!paths.isEmpty()) {
            m_fileSystemWatcher->removePaths(paths);
        }
        QStringList dirs = m_fileSystemWatcher->directories();
        if (!dirs.isEmpty()) {
            m_fileSystemWatcher->removePaths(dirs);
        }
    }
}

void CDirectoryWatcher::SetCurrentFile(const QString& fileName) {
    if (m_aborted) return;
    QString cleanPath = QDir::cleanPath(fileName);

    if (!m_sCurrentFile.isEmpty() && m_fileSystemWatcher->files().contains(m_sCurrentFile)) {
        m_fileSystemWatcher->removePath(m_sCurrentFile);
    }

    m_sCurrentFile = cleanPath;
    m_modificationTimeCurrentFile = GetLastModificationTime(m_sCurrentFile);
    m_bModificationTimeValid = m_modificationTimeCurrentFile.isValid();

    if (!m_sCurrentFile.isEmpty()) {
        m_fileSystemWatcher->addPath(m_sCurrentFile);
    }
}

void CDirectoryWatcher::SetCurrentDirectory(const QString& directoryName) {
    if (m_aborted) return;
    const QString fullPath = QFileInfo(directoryName).absoluteFilePath();

    if (!m_watchedDirectory.isEmpty() && m_fileSystemWatcher->directories().contains(m_watchedDirectory)) {
        m_fileSystemWatcher->removePath(m_watchedDirectory);
    }

    m_watchedDirectory = fullPath;

    if (!m_watchedDirectory.isEmpty()) {
        m_fileSystemWatcher->addPath(m_watchedDirectory);
    }
}

#endif
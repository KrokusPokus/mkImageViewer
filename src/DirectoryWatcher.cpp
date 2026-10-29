#include "DirectoryWatcher.h"

#include <QFileInfo>
#include <QDir>

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

    // Waehrend einer Pause nur den Namen merken, Resume() setzt die Ueberwachung
    if (!m_sCurrentFile.isEmpty() && !m_suspended) {
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

    // Waehrend einer Pause nur den Namen merken, Resume() setzt die Ueberwachung
    if (!m_watchedDirectory.isEmpty() && !m_suspended) {
        m_fileSystemWatcher->addPath(m_watchedDirectory);
    }
}

void CDirectoryWatcher::Suspend() {
    if (m_aborted || m_suspended) return;
    m_suspended = true;
    // Entfernen statt "Signale ignorieren": ohne Watch erzeugt der Kernel gar keine Events,
    // auch keine, die erst nach dem Wiederaktivieren in der Queue ankaemen.
    const QStringList paths = m_fileSystemWatcher->files() + m_fileSystemWatcher->directories();
    if (!paths.isEmpty()) {
        m_fileSystemWatcher->removePaths(paths);
    }
}

void CDirectoryWatcher::Resume() {
    if (m_aborted || !m_suspended) return;
    m_suspended = false;
    // Neue Basiszeit, sonst wuerde die Aenderung waehrend der Pause als "Datei geaendert" gelten.
    m_modificationTimeCurrentFile = GetLastModificationTime(m_sCurrentFile);
    m_bModificationTimeValid = m_modificationTimeCurrentFile.isValid();
    if (!m_watchedDirectory.isEmpty()) m_fileSystemWatcher->addPath(m_watchedDirectory);
    if (!m_sCurrentFile.isEmpty())     m_fileSystemWatcher->addPath(m_sCurrentFile);
}

#include "DirectoryWatcher.h"
#include "MessageDef.h"

#include <QFileInfo>
#include <QDir>
#include <process.h>

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
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CDirectoryWatcher::CDirectoryWatcher(HWND hTargetWindow)
: m_hTargetWindow(hTargetWindow)
{
    ::InitializeCriticalSection(&m_lock);
    m_terminateEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_newDirectoryEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);

    m_hThread = reinterpret_cast<HANDLE>(_beginthread(ThreadFunc, 0, this));
}

CDirectoryWatcher::~CDirectoryWatcher() {
    Abort();
    ::DeleteCriticalSection(&m_lock);
    if (m_terminateEvent) ::CloseHandle(m_terminateEvent);
    if (m_newDirectoryEvent) ::CloseHandle(m_newDirectoryEvent);
}

void CDirectoryWatcher::Abort() {
    if (m_hThread != nullptr) {
        ::SetEvent(m_terminateEvent);
        if (WAIT_TIMEOUT == ::WaitForSingleObject(m_hThread, 100)) {
            ::TerminateThread(m_hThread, 1);
            ::WaitForSingleObject(m_hThread, 100);
        }
        m_hThread = nullptr;
    }
}

void CDirectoryWatcher::SetCurrentFile(const QString& fileName) {
    ::EnterCriticalSection(&m_lock);
    m_sCurrentFile = QDir::cleanPath(fileName);
    m_modificationTimeCurrentFile = GetLastModificationTime(m_sCurrentFile);
    m_bModificationTimeValid = m_modificationTimeCurrentFile.isValid();
    ::LeaveCriticalSection(&m_lock);
}

void CDirectoryWatcher::SetCurrentDirectory(const QString& directoryName) {
    // QFileInfo löst Pfade auf – kein MAX_PATH TCHAR-Puffer mehr nötig
    const QString fullPath = QFileInfo(directoryName).absoluteFilePath();

    ::EnterCriticalSection(&m_lock);
    m_sCurrentDirectory = fullPath;
    ::LeaveCriticalSection(&m_lock);

    ::SetEvent(m_newDirectoryEvent);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private Thread Function
/////////////////////////////////////////////////////////////////////////////////////////////

void CDirectoryWatcher::ThreadFunc(void* arg) {
    auto* thisPtr = static_cast<CDirectoryWatcher*>(arg);
    bool bTerminate = false;
    bool bSetupNewDirectory = true;
    HANDLE waitHandles[4]{ nullptr };

    do {
        waitHandles[0] = thisPtr->m_terminateEvent;
        waitHandles[1] = thisPtr->m_newDirectoryEvent;
        int numHandles = bSetupNewDirectory ? 2 : 4;

        ::EnterCriticalSection(&thisPtr->m_lock);
        if (bSetupNewDirectory && !thisPtr->m_sCurrentDirectory.isEmpty()) {
            // LPCWSTR-Cast an der Win32-Schnittstelle
            const auto dirWStr = reinterpret_cast<LPCWSTR>(thisPtr->m_sCurrentDirectory.utf16());

            waitHandles[2] = ::FindFirstChangeNotificationW(dirWStr, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
            if (waitHandles[2] != nullptr && waitHandles[2] != INVALID_HANDLE_VALUE) {
                numHandles++;
                waitHandles[3] = ::FindFirstChangeNotificationW(dirWStr, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
                if (waitHandles[3] != nullptr && waitHandles[3] != INVALID_HANDLE_VALUE) {
                    numHandles++;
                }
            }
        }
        ::LeaveCriticalSection(&thisPtr->m_lock);

        DWORD waitStatus = ::WaitForMultipleObjects(numHandles, waitHandles, FALSE, INFINITE);

        switch (waitStatus) {
            case WAIT_OBJECT_0:
                bTerminate = true;
                break;

            case WAIT_OBJECT_0 + 1:
                ::ResetEvent(thisPtr->m_newDirectoryEvent);
                bSetupNewDirectory = true;
                break;

            case WAIT_OBJECT_0 + 2:
                // file added or deleted in directory, send message to registered window
                bSetupNewDirectory = (::FindNextChangeNotification(waitHandles[2]) == FALSE);
                ::PostMessage(thisPtr->m_hTargetWindow, WM_ACTIVE_DIRECTORY_FILELIST_CHANGED, 0, 0);
                ::WaitForSingleObject(thisPtr->m_terminateEvent, 500); // don't flood the window with change notifications
                break;

            case WAIT_OBJECT_0 + 3: {
                // file written in directory, check if the displayed file has changed and send message to registered window if yes
                bSetupNewDirectory = (::FindNextChangeNotification(waitHandles[3]) == FALSE);
                ::WaitForSingleObject(thisPtr->m_terminateEvent, 250);

                bool sendMessage = false;

                ::EnterCriticalSection(&thisPtr->m_lock);
                if (thisPtr->m_bModificationTimeValid) {
                    QDateTime newTime = GetLastModificationTime(thisPtr->m_sCurrentFile);
                    if (!newTime.isValid()) {
                        ::LeaveCriticalSection(&thisPtr->m_lock);
                        ::WaitForSingleObject(thisPtr->m_terminateEvent, 250);
                        ::EnterCriticalSection(&thisPtr->m_lock);
                        newTime = GetLastModificationTime(thisPtr->m_sCurrentFile);
                    }

                    if (newTime.isValid() && newTime != thisPtr->m_modificationTimeCurrentFile) {
                        thisPtr->m_modificationTimeCurrentFile = newTime;
                        sendMessage = true;
                    }
                }
                ::LeaveCriticalSection(&thisPtr->m_lock);

                if (sendMessage) {
                    ::PostMessage(thisPtr->m_hTargetWindow, WM_DISPLAYED_FILE_CHANGED_ON_DISK, 0, 0);
                }
                break;
            }

            default:
                bTerminate = true;
                break;
        }

        if (bTerminate || bSetupNewDirectory) {
            if (waitHandles[2] != nullptr && waitHandles[2] != INVALID_HANDLE_VALUE) {
                ::FindCloseChangeNotification(waitHandles[2]);
                waitHandles[2] = nullptr;
            }
            if (waitHandles[3] != nullptr && waitHandles[3] != INVALID_HANDLE_VALUE) {
                ::FindCloseChangeNotification(waitHandles[3]);
                waitHandles[3] = nullptr;
            }
        }
    } while (!bTerminate);

    _endthread();
}
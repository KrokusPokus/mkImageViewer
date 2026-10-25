#include "resource.h"
#include "MainDlg.h"
#include "SettingsProvider.h"

#include <clocale>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QSystemSemaphore>
#include <QString>
#include <QDataStream>

// Eindeutige Namen für Shared Memory und Semaphor
const QString kShmKey = "mkImageViewer_Instance_Registry_v1";
const QString kSemKey = "mkImageViewer_Instance_Semaphore_v1";
constexpr int kIpcTimeoutMs = 250;

// Hilfsfunktion: Liest die Liste aktiver PIDs aus dem Shared Memory
QList<qint64> GetActivePids(QSharedMemory& shm, QSystemSemaphore& sem) {
    QList<qint64> pids;
    sem.acquire();
    if (shm.attach(QSharedMemory::ReadOnly)) {
        QByteArray buffer(static_cast<const char*>(shm.constData()), shm.size());
        QDataStream stream(&buffer, QIODevice::ReadOnly);
        stream >> pids;
        shm.detach();
    }
    sem.release();
    return pids;
}

// Hilfsfunktion: Fügt die eigene PID hinzu oder entfernt sie
void RegisterPid(qint64 myPid, bool registerInstance, QSharedMemory& shm, QSystemSemaphore& sem) {
    sem.acquire();
    QList<qint64> pids;

    if (shm.attach(QSharedMemory::ReadWrite)) {
        QByteArray buffer(static_cast<const char*>(shm.constData()), shm.size());
        QDataStream stream(&buffer, QIODevice::ReadOnly);
        stream >> pids;
        shm.detach();
    }

    if (registerInstance) {
        if (!pids.contains(myPid)) pids.append(myPid);
    } else {
        pids.removeAll(myPid);
    }

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << pids;

    if (shm.isAttached()) shm.detach();
    if (shm.create(buffer.size())) {
        memcpy(shm.data(), buffer.constData(), buffer.size());
    } else if (shm.attach(QSharedMemory::ReadWrite)) {
        shm.detach();
        shm.create(buffer.size());
        shm.attach(QSharedMemory::ReadWrite);
        memcpy(shm.data(), buffer.constData(), buffer.size());
        shm.detach();
    }
    sem.release();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(APP_TITLE);

    QImageReader::setAllocationLimit(0);
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");

    // Kommandozeilen-Parsing mit Qt6
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "Image file or directory to open.");
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    QString sStartupFile;
    if (!args.isEmpty()) {
        sStartupFile = QDir::cleanPath(args.first());
        QFileInfo fileInfo(sStartupFile);
        if (fileInfo.isDir()) {
            if (!sStartupFile.endsWith('/') && !sStartupFile.endsWith('\\')) {
                sStartupFile.append('/');
            }
        }
    }

    const qint64 myPid = QCoreApplication::applicationPid();
    QSystemSemaphore sem(kSemKey, 1, QSystemSemaphore::Open);
    QSharedMemory shm(kShmKey);

    // --- SCHRITT 1: Broadcast an alle laufenden Instanzen ---
    bool instanceAccepted = false;
    if (!sStartupFile.isEmpty()) {
        const QList<qint64> activePids = GetActivePids(shm, sem);
        qDebug() << "As Client: I can see activePids:" << activePids;
        for (qint64 pid : activePids) {
            if (pid == myPid) continue;

            QString pipeName = QString("mkImageViewer_Pipe_%1").arg(pid);
            QLocalSocket socket;
            socket.connectToServer(pipeName);

            if (socket.waitForConnected(kIpcTimeoutMs)) {
                // Frage diese Instanz, ob sie die Datei übernehmen will
                socket.write(sStartupFile.toUtf8());
                socket.waitForBytesWritten(kIpcTimeoutMs);

                if (socket.waitForReadyRead(kIpcTimeoutMs)) {
                    if (socket.readAll() == "ACK") {
                        instanceAccepted = true;
                        socket.disconnectFromServer();
                        break; // Eine Instanz hat es übernommen!
                    }
                }
                socket.disconnectFromServer();
            } else {
                // Instanz antwortet nicht mehr -> Automatisch aus der Registry werfen (Crash-Cleanup)
                RegisterPid(pid, false, shm, sem);
            }
        }
    }

    // Wenn eine andere Instanz die Datei übernommen hat, beenden wir uns
    if (instanceAccepted) {
        qDebug() << "As Client: Server has accepted the file. Client will exit.";
        return 0;
    }

    // --- SCHRITT 2: Keine Instanz wollte sie -> Wir werden selbst zur aktiven Instanz ---
    RegisterPid(myPid, true, shm, sem);

    // Eigenen lokalen Server aufspannen (mit unserer PID im Namen)
    QString myPipeName = QString("mkImageViewer_Pipe_%1").arg(myPid);
    QLocalServer::removeServer(myPipeName); // Sicherheishalber aufräumen
    QLocalServer localServer;
    localServer.listen(myPipeName);

    int nRet = 0;
    try {
        CMainDlg dlgMain(sStartupFile);

        QObject::connect(&localServer, &QLocalServer::newConnection, &dlgMain, [&localServer, &dlgMain]() {
            auto* clientSocket = localServer.nextPendingConnection();
            if (!clientSocket) return;

            clientSocket->waitForReadyRead(kIpcTimeoutMs);
            QString sCopyDataFile = QString::fromUtf8(clientSocket->readAll());
            qDebug() << "As Server: Client sent path to evaluate:" << sCopyDataFile;
            if (dlgMain.ShouldAcceptFile(sCopyDataFile)) {
                qDebug() << "As Server: I accept that file. The client should exit.";
                clientSocket->write("ACK");
            } else {
                qDebug() << "As Server: I won't accept that file. The client may open it.";
                clientSocket->write("NACK");
            }
            clientSocket->waitForBytesWritten(kIpcTimeoutMs);
            clientSocket->deleteLater();
        });

        dlgMain.show();
        nRet = app.exec();

        if (CSettingsProvider::This().StickyWindowSize() && !dlgMain.IsFullScreenMode() && !dlgMain.IsMaximizedMode()) {
            CSettingsProvider::This().SaveStickyWindowRect(dlgMain.WindowRectOnClose());
        }
    }
    catch (...) {
        // Exception Handling
    }

    // Sauberer Exit: Eigene PID aus dem Shared Memory austragen
    RegisterPid(myPid, false, shm, sem);
    return nRet;
}
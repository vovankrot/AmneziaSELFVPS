#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QFile>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <sys/un.h>
#include <libproc.h>
#endif

namespace amnezia {
inline QString localPeerProcessIdentity(QLocalSocket *socket)
{
    if (!socket) return {};
#ifdef Q_OS_WIN
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &pid)) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!ok) return {};
    return QString("%1:%2").arg(pid).arg((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime);
#elif defined(Q_OS_LINUX)
    struct ucred peer{}; socklen_t length = sizeof(peer);
    if (getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0) return {};
    QFile stat(QString("/proc/%1/stat").arg(peer.pid));
    if (!stat.open(QIODevice::ReadOnly)) return {};
    const auto data = stat.readAll();
    const auto fields = data.mid(data.lastIndexOf(')') + 2).trimmed().split(' ');
    if (fields.size() <= 19) return {};
    return QString("%1:%2").arg(peer.pid).arg(QString::fromLatin1(fields[19]));
#elif defined(Q_OS_MACOS)
    pid_t pid = 0; socklen_t length = sizeof(pid);
    if (getsockopt(socket->socketDescriptor(), SOL_LOCAL, LOCAL_PEERPID, &pid, &length) != 0) return {};
    struct proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return {};
    return QString("%1:%2:%3").arg(pid).arg(info.pbi_start_tvsec).arg(info.pbi_start_tvusec);
#else
    return {};
#endif
}
inline bool isTrustedLocalPeer(QLocalSocket *socket)
{
    if (!socket) return false;
    QString image;
#ifdef Q_OS_WIN
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &pid)) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768];
    DWORD length = 32768;
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!queried) return false;
    image = QString::fromWCharArray(path, length);
#elif defined(Q_OS_LINUX)
    struct PeerCredentials { pid_t pid; uid_t uid; gid_t gid; } peer{};
    socklen_t length = sizeof(peer);
    if (getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0
            || length != sizeof(peer)) return false;
    image = QFileInfo(QString("/proc/%1/exe").arg(peer.pid)).symLinkTarget();
#elif defined(Q_OS_MACOS)
    pid_t pid = 0;
    socklen_t length = sizeof(pid);
    if (getsockopt(socket->socketDescriptor(), SOL_LOCAL, LOCAL_PEERPID, &pid, &length) != 0) return false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int size = proc_pidpath(pid, path, sizeof(path));
    if (size <= 0) return false;
    image = QString::fromUtf8(path);
#else
    return false;
#endif
    const QString actual = QFileInfo(image).canonicalFilePath();
    if (actual.isEmpty()) return false;
    const QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QStringList candidates{directory.filePath("AmneziaVPN.exe")};
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    const QStringList candidates{directory.filePath("AmneziaVPN"),
                                 directory.filePath("../client/bin/AmneziaVPN"),
                                 directory.filePath("../../client/bin/AmneziaVPN")};
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    for (const auto &candidate : candidates) {
        const QString expected = QFileInfo(candidate).canonicalFilePath();
        if (!expected.isEmpty() && actual.compare(expected, sensitivity) == 0) return true;
    }
    return false;
}

inline bool authorizeLocalPeer(QLocalSocket *socket)
{
    if (isTrustedLocalPeer(socket)) return true;
    qWarning() << "IPC: rejected client outside the installed application";
    if (socket) {
        socket->abort();
        socket->deleteLater();
    }
    return false;
}
}

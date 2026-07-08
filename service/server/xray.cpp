#include "xray.h"
#include "core/networkUtilities.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QCoreApplication>
#include <amnezia_xray.h>
#include <qdebug.h>

#ifdef Q_OS_DARWIN
    #include <arpa/inet.h>
    #include <cerrno>
    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <netinet/ip.h>
    #include <sys/socket.h>
#endif
#ifdef Q_OS_WIN
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif
#ifdef Q_OS_LINUX
    #include <sys/socket.h>
#endif

bool Xray::startXray(const QString &cfg)
{
    qDebug() << "Xray::startXray()";

    // Stop any previous xray instance to avoid "already running" errors
    stopXray();

    refreshDefaultInterface();

    try {
        if (auto err = amnezia_xray_setsockcallback(ctxSockCallback, this); err != nullptr) {
            qDebug() << "[xray] sockopt failed: " << err;
            amnezia_xray_free(err);
            return false;
        }

        amnezia_xray_setloghandler(ctxLogHandler, this);

        QByteArray bytes = cfg.toUtf8();
        if (auto err = amnezia_xray_configure(bytes.data()); err != nullptr) {
            qDebug() << "[xray] configuration failed: " << err;
            amnezia_xray_free(err);
            return false;
        }

        if (auto err = amnezia_xray_start(); err != nullptr) {
            qDebug() << "[xray] failed to start: " << err;
            amnezia_xray_free(err);
            return false;
        }
    } catch (const std::exception &ex) {
        qCritical() << "[xray] C++ exception in startXray:" << ex.what();
        return false;
    } catch (...) {
        qCritical() << "[xray] Unknown exception in startXray";
        return false;
    }

    return true;
}

bool Xray::stopXray()
{
    qDebug() << "Xray::stopXray()";
    if (auto err = amnezia_xray_stop(); err != nullptr) {
        qDebug() << "[xray] failed to stop: " << err;
        amnezia_xray_free(err);
        return false;
    }

    return true;
}

void Xray::logHandler(char* str)
{
    QMetaObject::invokeMethod(qApp, [str = QString::fromUtf8(str)] {
        qDebug() << "[xray]" << str;
    }, Qt::QueuedConnection);
}

void Xray::sockCallback(uintptr_t fd)
{
#ifdef Q_OS_MAC
    int idx = m_defaultIfaceIdx.load(std::memory_order_relaxed);
    if (idx > 0) {
        setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx));
        setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &idx, sizeof(idx));
    }
#endif
#ifdef Q_OS_WIN
    // Identify the socket type. mKCP rides on UDP; reality/vision/direct ride on TCP.
    int sotype = 0;
    int slen = sizeof(sotype);
    getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&sotype), &slen);
    const bool isUdp = (sotype == SOCK_DGRAM);

    DWORD idx = static_cast<DWORD>(m_defaultIfaceIdx.load(std::memory_order_relaxed));
    int rc6 = -1, rc4 = -1, e6 = 0, e4 = 0;

    // IP_UNICAST_IF pins the dial to the physical interface so it does not recurse
    // into the VPN tunnel. TCP dials need this. mKCP (UDP), however, reaches the
    // server through the explicit /32 host route via the physical gateway, and
    // pinning the UDP socket with IP_UNICAST_IF was stopping the kcp packets from
    // ever leaving the box (0 packets reached the server). So skip UDP. by vovankrot
    if (idx > 0 && !isUdp) {
        DWORD idx6 = idx;
        rc6 = setsockopt(static_cast<SOCKET>(fd), IPPROTO_IPV6, IPV6_UNICAST_IF, reinterpret_cast<char *>(&idx6), sizeof(idx6));
        if (rc6 != 0) e6 = WSAGetLastError();
        DWORD idx4 = htonl(idx); // IP_UNICAST_IF expects index in network byte order
        rc4 = setsockopt(static_cast<SOCKET>(fd), IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char *>(&idx4), sizeof(idx4));
        if (rc4 != 0) e4 = WSAGetLastError();
    }

    QString diag = QString("[xray] sockCallback fd=%1 type=%2 ifaceIdx=%3 bound=%4 rc6=%5 e6=%6 rc4=%7 e4=%8")
                       .arg(QString::number(static_cast<qulonglong>(fd)),
                            isUdp ? QStringLiteral("UDP") : QStringLiteral("TCP"),
                            QString::number(idx),
                            (idx > 0 && !isUdp) ? QStringLiteral("yes") : QStringLiteral("no"),
                            QString::number(rc6), QString::number(e6),
                            QString::number(rc4), QString::number(e4));
    QMetaObject::invokeMethod(qApp, [diag] { qDebug() << diag; }, Qt::QueuedConnection);
#endif
#ifdef Q_OS_LINUX
    {
        QMutexLocker locker(&m_ifaceMutex);
        if (!m_defaultIfaceName.isEmpty()) {
            setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, m_defaultIfaceName.data(), m_defaultIfaceName.size());
        }
    }
#endif
}

void Xray::refreshDefaultInterface()
{
    auto defaultIface = NetworkUtilities::getGatewayAndIface().second;
    if (!defaultIface.isValid()) {
        qWarning() << "[xray] No valid default network interface found";
    }
#ifdef Q_OS_LINUX
    {
        QMutexLocker locker(&m_ifaceMutex);
        m_defaultIfaceName = defaultIface.name().toUtf8();
    }
    qDebug() << "[xray] refreshDefaultInterface: name =" << defaultIface.name();
#else
    int newIdx = defaultIface.isValid() ? defaultIface.index() : 0;
    int oldIdx = m_defaultIfaceIdx.exchange(newIdx, std::memory_order_relaxed);
    if (oldIdx != newIdx) {
        qDebug() << "[xray] refreshDefaultInterface: iface index" << oldIdx << "->" << newIdx
                 << "(" << defaultIface.humanReadableName() << ")";
    }
#endif
}

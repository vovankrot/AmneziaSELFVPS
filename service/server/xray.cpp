#include "xray.h"
#include "core/networkUtilities.h"

#include <QDebug>
#include <QElapsedTimer>
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
    #include "windowsSocketBinding.h"
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
        // amnezia_xray_configure() calls xray-core's core.New(coreConfig), which
        // synchronously initializes every configured service (DNS, GeoIP/GeoSite
        // routing tables, etc.) -- and amnezia_xray_start() calls server.Start(),
        // which brings up the actual listeners/outbound handlers. Both are opaque
        // calls into the closed-source-to-us amnezia_xray.dll with no internal
        // logging of their own before the "core: Xray ... started" message, so a
        // hang inside either one is invisible without bracketing timestamps like
        // these. Added 2026-08-05 chasing an intermittent 100+s hang on connect
        // with zero log output anywhere. by vovankrot
        QElapsedTimer xrayStageTimer;
        xrayStageTimer.start();
        qDebug() << "[xray] calling amnezia_xray_configure()...";
        auto configureErr = amnezia_xray_configure(bytes.data());
        qDebug() << "[xray] amnezia_xray_configure() returned after" << xrayStageTimer.elapsed() << "ms";
        if (configureErr != nullptr) {
            qDebug() << "[xray] configuration failed: " << configureErr;
            amnezia_xray_free(configureErr);
            return false;
        }

        xrayStageTimer.restart();
        qDebug() << "[xray] calling amnezia_xray_start()...";
        auto startErr = amnezia_xray_start();
        qDebug() << "[xray] amnezia_xray_start() returned after" << xrayStageTimer.elapsed() << "ms";
        if (startErr != nullptr) {
            qDebug() << "[xray] failed to start: " << startErr;
            amnezia_xray_free(startErr);
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
    const auto result = WindowsSocketBinding::bindTcp(static_cast<SOCKET>(fd),
        static_cast<DWORD>(m_defaultIfaceIdx.load(std::memory_order_relaxed)),
        getsockopt, setsockopt, WSAGetLastError);
    if (result.status == WindowsSocketBinding::Status::Failed) {
        const QString diagnostic = QStringLiteral("[xray] physical interface binding failed: family=%1 error=%2")
            .arg(result.family).arg(result.error);
        QMetaObject::invokeMethod(qApp, [diagnostic] { qWarning() << diagnostic; }, Qt::QueuedConnection);
    }
    // A skipped mKCP/UDP binding is intentional, not a socket error. Successful
    // sockets also need no per-socket queued log message on the service thread.

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

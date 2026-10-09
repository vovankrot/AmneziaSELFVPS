#pragma once

#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QHostAddress>
#include <functional>

namespace TunnelDataProbe {
enum class Failure { None, InvalidTarget, Tls, Response, Timeout, Path };
struct Result { bool success = false; Failure failure = Failure::None; qint64 elapsedMs = 0; };

// Run on a worker thread. A SOCKS CONNECT or TLS handshake alone is not an
// application response. No account credentials or response body are requested.
namespace detail {
inline Result run(const QString &host, const QString &tcpTarget, quint16 port,
                  const QNetworkProxy &proxy, int timeoutMs, const QSslConfiguration &ssl,
                  const std::function<bool(QSslSocket &)> &prepareSocket = {})
{
    QElapsedTimer timer;
    timer.start();
    const auto done = [&](Failure failure) {
        return Result{failure == Failure::None, failure, timer.elapsed()};
    };
    static const QRegularExpression hostname(R"(^[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?$)");
    if (!hostname.match(host).hasMatch() || host.size() > 253 || !port || timeoutMs <= 0)
        return done(Failure::InvalidTarget);
    const auto remaining = [&] { return qMax(0, timeoutMs - int(timer.elapsed())); };
    QSslSocket socket;
    socket.setProxy(proxy);
    socket.setSslConfiguration(ssl);
    socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (prepareSocket && !prepareSocket(socket)) return done(Failure::Path);
    // TLS identity/SNI and HTTP Host remain the hostname even when the socket
    // (including SOCKS CONNECT) is pinned to a numeric CDN address.
    socket.connectToHostEncrypted(tcpTarget, port, host);
    if (!socket.waitForEncrypted(remaining()))
        return done(remaining() == 0 || socket.error() == QAbstractSocket::SocketTimeoutError
                    ? Failure::Timeout : Failure::Tls);
    const QByteArray target = host.toLatin1() + (port == 443 ? QByteArray() : ':' + QByteArray::number(port));
    socket.write("HEAD / HTTP/1.1\r\nHost: " + target + "\r\nConnection: close\r\n\r\n");
    QByteArray response;
    while (remaining() > 0) {
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(remaining())) break;
        response += socket.read(8192 - response.size());
        const int newline = response.indexOf("\r\n");
        if (newline >= 0) {
            static const QRegularExpression status(R"(^HTTP/1\.[01] [1-5]\d\d(?: [^\r\n]*)?$)");
            return done(status.match(QString::fromLatin1(response.left(newline))).hasMatch()
                        ? Failure::None : Failure::Response);
        }
        if (response.size() == 8192) return done(Failure::Response);
    }
    return done(remaining() == 0 || socket.error() == QAbstractSocket::SocketTimeoutError
                ? Failure::Timeout : Failure::Response);
}
}

inline Result run(const QString &host, quint16 port, const QNetworkProxy &proxy, int timeoutMs,
                  const QSslConfiguration &ssl = QSslConfiguration::defaultConfiguration())
{
    return detail::run(host, host, port, proxy, timeoutMs, ssl);
}

// Run both controls against the SAME address from one DNS snapshot. The direct
// caller must pin the physical interface in prepareSocket; NoProxy alone only
// disables a proxy and does not bypass OS VPN routes. Failure to pin the path
// is an inconclusive Path result, never evidence of a CDN timeout.
inline Result runAtAddress(const QString &host, const QHostAddress &address, quint16 port,
                           const QNetworkProxy &proxy, int timeoutMs,
                           const std::function<bool(QSslSocket &)> &prepareSocket = {},
                           const QSslConfiguration &ssl = QSslConfiguration::defaultConfiguration())
{
    if (address.isNull() || (address.protocol() != QAbstractSocket::IPv4Protocol
                            && address.protocol() != QAbstractSocket::IPv6Protocol))
        return {false, Failure::InvalidTarget, 0};
    if (proxy.type() == QNetworkProxy::NoProxy && !prepareSocket)
        return {false, Failure::Path, 0};
    if (proxy.type() != QNetworkProxy::NoProxy && proxy.type() != QNetworkProxy::Socks5Proxy)
        return {false, Failure::Path, 0};
    return detail::run(host, address.toString(), port, proxy, timeoutMs, ssl, prepareSocket);
}
}

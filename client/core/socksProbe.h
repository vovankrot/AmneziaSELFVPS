#pragma once
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>

namespace SocksProbe {
inline bool connect(const QString &targetHost, quint16 targetPort, int timeoutMs, quint16 socksPort,
                    const QString &user = {}, const QString &password = {})
{
    const QByteArray host = QUrl::toAce(targetHost), username = user.toUtf8(), secret = password.toUtf8();
    const bool authenticated = !user.isEmpty() || !password.isEmpty();
    if (host.isEmpty() || host.size() > 255 || !targetPort || !socksPort || timeoutMs <= 0
        || (authenticated && (username.isEmpty() || secret.isEmpty() || username.size() > 255 || secret.size() > 255))) return false;
    QElapsedTimer timer; timer.start();
    auto budget = [&] { return qMax(0, timeoutMs - int(timer.elapsed())); };
    while (budget() > 0 && !QThread::currentThread()->isInterruptionRequested()) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, socksPort);
        auto read = [&](int size) {
            while (socket.bytesAvailable() < size && budget() > 0) {
                socket.waitForReadyRead(qMin(budget(), 100));
                if (socket.state() != QAbstractSocket::ConnectedState) return QByteArray();
            }
            return socket.bytesAvailable() >= size ? socket.read(size) : QByteArray();
        };
        auto send = [&](const QByteArray &bytes) {
            if (budget() <= 0 || socket.write(bytes) != bytes.size()) return false;
            return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(qMin(budget(), 500));
        };
        bool success = false;
        if (socket.waitForConnected(qMin(budget(), 700))
            && send(authenticated ? QByteArray::fromHex("050102") : QByteArray::fromHex("050100"))
            && read(2) == (authenticated ? QByteArray::fromHex("0502") : QByteArray::fromHex("0500"))) {
            bool authorized = !authenticated;
            if (authenticated) {
                QByteArray auth; auth += char(1); auth += char(username.size()); auth += username;
                auth += char(secret.size()); auth += secret;
                authorized = send(auth) && read(2) == QByteArray::fromHex("0100");
            }
            QByteArray request = QByteArray::fromHex("05010003");
            request += char(host.size()); request += host;
            request += char(targetPort >> 8); request += char(targetPort & 255);
            if (authorized && send(request)) {
                const QByteArray reply = read(4);
                if (reply.size() == 4 && reply.left(3) == QByteArray::fromHex("050000")) {
                    int length = quint8(reply[3]) == 1 ? 4 : (quint8(reply[3]) == 4 ? 16 : -1);
                    if (quint8(reply[3]) == 3) {
                        const auto domainLength = read(1);
                        if (domainLength.size() == 1) length = quint8(domainLength[0]);
                    }
                    success = length > 0 && read(length + 2).size() == length + 2;
                }
            }
        }
        socket.abort();
        if (success) return true;
        if (budget() > 0) QThread::msleep(qMin(budget(), 100));
    }
    return false;
}
}

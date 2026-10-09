#pragma once
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>

inline bool serviceHealthProbe(const QString &name = QStringLiteral("\\\\.\\pipe\\amneziavpn"))
{
    QLocalSocket socket;
    QElapsedTimer deadline; deadline.start();
    socket.connectToServer(name);
    if (!socket.waitForConnected(2000)) return false;
    socket.write("{\"type\":\"logs\"}\n");
    if (!socket.waitForBytesWritten(2000)) return false;
    QByteArray reply;
    while (deadline.elapsed() < 4000 && reply.size() < 4096) {
        if (socket.bytesAvailable() == 0
            && !socket.waitForReadyRead(int(4000-deadline.elapsed()))) return false;
        reply += socket.readAll();
        const auto newline = reply.indexOf('\n');
        if (newline >= 0)
            return QJsonDocument::fromJson(reply.left(newline)).object().value("type").toString() == "logs";
    }
    return false;
}

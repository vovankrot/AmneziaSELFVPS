#pragma once
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <cstring>

namespace SshSessionPolicy {
inline int effectivePort(int port) { return port == 0 ? 22 : (port > 0 && port <= 65535 ? port : -1); }
inline QByteArray identity(const QString &host, const QString &user, int port, const QString &secret) {
    return QCryptographicHash::hash(QJsonDocument(QJsonArray{host, user, effectivePort(port), secret}).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}
inline bool copyPassphrase(const QString &value, char *buffer, size_t length) {
    if (!buffer || !length) return false;
    buffer[0] = '\0';
    const auto bytes = value.toUtf8();
    if (size_t(bytes.size()) >= length) return false;
    std::memcpy(buffer, bytes.constData(), size_t(bytes.size()));
    buffer[bytes.size()] = '\0';
    return true;
}
}

#pragma once

#include <QHostAddress>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace ListValidation {
constexpr qint64 maxDownloadBytes = 32 * 1024 * 1024;

inline void limitDownload(QNetworkReply *reply)
{
    reply->setReadBufferSize(maxDownloadBytes + 1);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply]() {
        if (reply->bytesAvailable() > maxDownloadBytes) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply]() {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maxDownloadBytes)
            reply->abort();
    });
}

inline bool normalize(const QByteArray &input, bool cidrs, QByteArray &output, int &count)
{
    output.clear();
    count = 0;
    if (input.isEmpty() || input.size() > maxDownloadBytes) return false;
    QSet<QByteArray> seen;
    const QRegularExpression ipv4(QStringLiteral("^(0|[1-9][0-9]{0,2})(\\.(0|[1-9][0-9]{0,2})){3}$"));
    const QRegularExpression label(QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    for (const auto &raw : input.split('\n')) {
        QByteArray value = raw.trimmed();
        if (value.isEmpty() || value.startsWith('#') || value.startsWith(';')) continue;
        if (cidrs) {
            const auto parts = value.split('/');
            bool ok = false;
            const int prefix = parts.size() == 2 ? parts[1].toInt(&ok) : -1;
            const QHostAddress address(QString::fromUtf8(parts.value(0)));
            if (!ok || !QRegularExpression("^(0|[1-9][0-9]?)$").match(QString::fromLatin1(parts.value(1))).hasMatch()
                    || prefix < 0 || prefix > 32 || address.protocol() != QAbstractSocket::IPv4Protocol
                    || !ipv4.match(QString::fromUtf8(parts[0])).hasMatch()) return false;
            const quint32 mask = prefix == 0 ? 0 : 0xffffffffu << (32 - prefix);
            value = QHostAddress(address.toIPv4Address() & mask).toString().toUtf8()
                    + '/' + QByteArray::number(prefix);
        } else {
            if (value.startsWith("*.")) value.remove(0, 2);
            const QString domain = QString::fromUtf8(value);
            if (domain.toUtf8() != value) return false;
            value = QUrl::toAce(domain).toLower();
            if (value.endsWith('.')) value.chop(1);
            if (value.isEmpty() || value.size() > 253 || !value.contains('.')) return false;
            for (const auto &part : value.split('.'))
                if (!label.match(QString::fromLatin1(part)).hasMatch()) return false;
        }
        if (!seen.contains(value)) {
            seen.insert(value);
            output += value + '\n';
            ++count;
        }
    }
    return count > 0;
}

inline bool save(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
}

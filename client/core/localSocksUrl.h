#pragma once
#include <QUrl>

namespace LocalSocksUrl {
inline QString make(quint16 port, const QString &user = {}, const QString &password = {})
{
    QUrl url;
    url.setScheme(QStringLiteral("socks5"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(port);
    if (!user.isEmpty() && !password.isEmpty()) {
        url.setUserName(user, QUrl::DecodedMode);
        url.setPassword(password, QUrl::DecodedMode);
    }
    return QString::fromLatin1(url.toEncoded());
}
}

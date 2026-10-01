#pragma once
#include <QString>
#include <QRegularExpression>
namespace LogRedaction {
inline QString hideProxyCredentials(QString text) {
    static const QRegularExpression credentials(
        QStringLiteral(R"(((?:socks5h?|socks|https?)://)[^\s/@]+(?::[^\s/@]*)?@)"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(credentials, QStringLiteral("\\1[REDACTED]@"));
    return text;
}
}

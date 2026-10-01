#pragma once
#include <QHostAddress>
#include <QList>

namespace SplitTunnelAddress {
inline QHostAddress ipv4(const QList<QHostAddress> &addresses)
{
    for (const auto &ip : addresses) {
        if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isNull() && !ip.isLoopback()
            && !ip.isMulticast() && !ip.isInSubnet(QHostAddress("169.254.0.0"), 16)) return ip;
    }
    return {};
}
inline QHostAddress ipv6(const QList<QHostAddress> &addresses)
{
    // A scoped link-local address is not an Internet source address. Prefer
    // global unicast; keep ULA for installations with IPv6 through a LAN router.
    for (const auto &ip : addresses)
        if (ip.protocol() == QAbstractSocket::IPv6Protocol && ip.isInSubnet(QHostAddress("2000::"), 3)) return ip;
    for (const auto &ip : addresses)
        if (ip.protocol() == QAbstractSocket::IPv6Protocol && ip.isInSubnet(QHostAddress("fc00::"), 7)) return ip;
    return {};
}
}

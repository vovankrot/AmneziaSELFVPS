#pragma once
#include <QString>
#include <QRegularExpression>

namespace ProtocolInstallHealth {
enum class Kind { WireGuard, AwgLegacy, Awg, Xray, Hysteria2, AnyTls };
inline QString quote(QString value) {
    value.replace("'", "'\\''");
    return "'" + value + "'";
}
inline QString probe(const QString &container, Kind kind, int port) {
    if (!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]*$").match(container).hasMatch()
        || port < 1 || port > 65535) return {};
    QString command;
    if (kind == Kind::WireGuard || kind == Kind::AwgLegacy || kind == Kind::Awg) {
        const QString tool=kind == Kind::Awg ? "awg" : "wg";
        const QString iface=kind == Kind::Awg ? "awg0" : "wg0";
        command=QString("ip link show dev %1 >/dev/null 2>&1 && test \"$(%2 show %1 listen-port)\" = %3")
            .arg(iface,tool).arg(port);
    } else {
        const QString process=kind == Kind::Xray ? "xray" : kind == Kind::Hysteria2 ? "hysteria" : "anytls-server";
        command=QString("pidof %1 >/dev/null && for file in /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6; do "
            "if test -r \"$file\" && awk '$2 ~ /:%2$/ && ($4 == \"0A\" || $4 == \"07\") { found=1 } "
            "END { exit !found }' \"$file\"; then exit 0; fi; done; exit 1")
            .arg(process,QString::number(port,16).toUpper().rightJustified(4,'0'));
    }
    return QString("sudo docker inspect -f '{{.State.Running}}' %1 2>/dev/null | grep -qx true && "
                   "sudo docker exec %1 sh -c %2").arg(container,quote(command));
}
}

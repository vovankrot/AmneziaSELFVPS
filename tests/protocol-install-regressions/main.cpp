#include "client/core/protocolInstallHealth.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc,argv);
    using namespace ProtocolInstallHealth;
    if (argc == 4) {
        const QString name=QString::fromLocal8Bit(argv[1]);
        const auto kind=static_cast<Kind>(QString::fromLocal8Bit(argv[2]).toInt());
        const QString script=probe(name,kind,QString::fromLocal8Bit(argv[3]).toInt());
        if (script.isEmpty()) return 1;
        std::fputs(script.toUtf8().constData(),stdout);
        return 0;
    }
    if (!probe("bad;name",Kind::Awg,443).isEmpty()) return 1;
    if (!probe("test",Kind::Awg,0).isEmpty()) return 2;
    if (!probe("test",Kind::Awg,65536).isEmpty()) return 3;
    if (quote("a'b") != "'a'\\''b'") return 4;
    if (!probe("test",Kind::Awg,42001).contains("awg show awg0 listen-port")) return 5;
    if (!probe("test",Kind::AwgLegacy,42001).contains("wg show wg0 listen-port")) return 6;
    for (auto kind : {Kind::Xray,Kind::Hysteria2,Kind::AnyTls}) {
        const auto script=probe("test",kind,42002);
        if (!script.contains(":A412$") || !script.contains("pidof ")
            || !script.contains("/proc/net/tcp6") || !script.contains("/proc/net/udp6")) return 7;
    }
    std::puts("Protocol installation probe validation passed");
    return 0;
}

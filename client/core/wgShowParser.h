#pragma once
#include <QString>
#include <vector>
#include <utility>

template<class Peer> std::vector<Peer> parseWgShow(const QString &text)
{
    std::vector<Peer> peers;
    Peer current{};
    bool active = false;
    for (const auto &raw : text.split('\n')) {
        const QString line = raw.trimmed();
        const int separator = line.indexOf(':');
        if (separator < 0) continue;
        const QString key = line.left(separator);
        const QString value = line.mid(separator + 1).trimmed();
        if (key == "peer") {
            if (active) peers.push_back(current);
            current = Peer{};
            current.clientId = value;
            active = !value.isEmpty();
        } else if (key == "interface") {
            if (active) peers.push_back(current);
            current = Peer{};
            active = false;
        } else if (active) {
            if (key == "latest handshake") {
                current.latestHandshake = value;
                for (const auto &unit : {std::pair{"day", "d"}, {"hour", "h"}, {"minute", "m"}, {"second", "s"}}) {
                    current.latestHandshake.replace(QString(" %1s").arg(unit.first), unit.second);
                    current.latestHandshake.replace(QString(" %1").arg(unit.first), unit.second);
                }
            } else if (key == "allowed ips") current.allowedIps = value;
            else if (key == "endpoint") current.endpoint = value;
            else if (key == "transfer") {
                for (QString part : value.split(',')) {
                    part = part.trimmed();
                    if (part.endsWith(" received")) {
                        part.chop(9);
                        current.dataSent = part;
                    } else if (part.endsWith(" sent")) {
                        part.chop(5);
                        current.dataReceived = part;
                    }
                }
            }
        }
    }
    if (active) peers.push_back(current);
    return peers;
}

#pragma once

#include <QJsonObject>
#include <QSharedPointer>
#include "rep_ipc_interface_replica.h"
#include "core/asyncIpcSequence.h"
#include "core/networkUtilities.h"

namespace SocksRoutingSetup {
struct Parameters {
    QString device, localAddress, vpnAddress, vpnGateway, serverAddress, externalGateway;
    QList<QHostAddress> dns;
    QJsonObject peerConfig;
    bool allSites = false;
    bool killSwitch = false;
    bool appSplit = false;
};

inline std::vector<AsyncIpcSequence::Step> steps(const QSharedPointer<IpcInterfaceReplica> &iface,
                                               const Parameters &p,
                                               std::function<int()> vpnIndex = {},
                                               std::function<int(const QHostAddress &)> externalIndexLookup = {})
{
    using Step = AsyncIpcSequence::Step;
    const auto count = [](int expected) {
        return [expected](const QVariant &value) { return value.toInt() == expected; };
    };
    if (!vpnIndex) vpnIndex = [p] {
        for (const auto &iface : QNetworkInterface::allInterfaces()) {
            if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
            for (const auto &entry : iface.addressEntries())
                if (entry.ip() == QHostAddress(p.vpnAddress)) return iface.index();
        }
        return -1;
    };
    std::vector<Step> result;
    result.push_back({"createTun", [iface, p] { return iface->createTun(p.device, p.localAddress); }});
    result.push_back({"updateResolvers", [iface, p] { return iface->updateResolvers(p.device, p.dns); }});
    // Capture the physical path before adding any catch-all TUN routes.
#ifdef Q_OS_WIN
    const int externalIndex = externalIndexLookup
        ? externalIndexLookup(QHostAddress(p.serverAddress))
        : NetworkUtilities::AdapterIndexTo(QHostAddress(p.serverAddress));
#endif
    if (p.killSwitch && !p.appSplit) {
        result.push_back({"enableKillSwitch", [iface, p, vpnIndex] {
#ifdef Q_OS_WIN
            const int index = vpnIndex();
            if (index <= 0) return QRemoteObjectPendingReply<bool>(
                QRemoteObjectPendingCall::fromCompletedCall(false));
#else
            const int index = 0;
#endif
            auto config = p.peerConfig;
            config.insert("vpnServer", p.serverAddress);
            return iface->enableKillSwitch(config, index);
        }});
    }
    if (p.allSites) {
        // Missing/failed server exclusion is fatal: adding catch-all routes
        // without it could route the VPN's own connection back into the TUN.
        result.push_back({"excludeServer", [iface, p] {
            if (!NetworkUtilities::checkIPv4Format(p.serverAddress)
                || !NetworkUtilities::checkIPv4Format(p.externalGateway))
                return QRemoteObjectPendingReply<int>(QRemoteObjectPendingCall::fromCompletedCall(-1));
            return iface->routeAddList(p.externalGateway, {p.serverAddress + "/32"});
        }, count(1)});
        const QStringList subnets {"1.0.0.0/8", "2.0.0.0/7", "4.0.0.0/6", "8.0.0.0/5",
                                   "16.0.0.0/4", "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/1"};
        result.push_back({"routeAddList", [iface, p, subnets] {
            return iface->routeAddList(p.vpnGateway, subnets);
        }, count(subnets.size())});
    }
#ifdef Q_OS_WIN
    if (!p.appSplit)
#endif
        result.push_back({"StopRoutingIpv6", [iface] { return iface->StopRoutingIpv6(); }});
#ifdef Q_OS_WIN
    if (p.killSwitch || p.appSplit) {
        result.push_back({"enablePeerTraffic", [iface, p, externalIndex, vpnIndex] {
            const int index = vpnIndex();
            if (externalIndex <= 0 || index <= 0 || externalIndex == index)
                return QRemoteObjectPendingReply<bool>(QRemoteObjectPendingCall::fromCompletedCall(false));
            auto config = p.peerConfig;
            config.insert("inetAdapterIndex", externalIndex);
            config.insert("vpnAdapterIndex", index);
            config.insert("vpnGateway", p.vpnGateway);
            config.insert("vpnServer", p.serverAddress);
            return iface->enablePeerTraffic(config);
        }, [](const QVariant &value) { return value.toBool(); }, 15000});
    }
    if (p.appSplit)
        result.push_back({"StartRoutingIpv6", [iface] { return iface->StartRoutingIpv6(); }});
#endif
    return result;
}
}

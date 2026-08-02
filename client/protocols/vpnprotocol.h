#ifndef VPNPROTOCOL_H
#define VPNPROTOCOL_H

#include <QObject>
#include <QString>
#include <QJsonObject>

#include "core/defs.h"
#include "containers/containers_defs.h"

using namespace amnezia;

class QTimer;

//todo change name
namespace Vpn
{
    Q_NAMESPACE
    enum ConnectionState {
        Unknown,
        Disconnected,
        Preparing,
        Connecting,
        Connected,
        Disconnecting,
        Reconnecting,
        Error
    };
    Q_ENUM_NS(ConnectionState)

    static void declareQmlVpnConnectionStateEnum() {
        qmlRegisterUncreatableMetaObject(
            Vpn::staticMetaObject,
            "ConnectionState",
            1, 0,
            "ConnectionState",
            "Error: only enums"
            );
    }
}

class VpnProtocol : public QObject
{
    Q_OBJECT

public:
    explicit VpnProtocol(const QJsonObject& configuration, QObject* parent = nullptr);
    virtual ~VpnProtocol() override = default;

    static QString textConnectionState(Vpn::ConnectionState connectionState);

    virtual ErrorCode prepare() { return ErrorCode::NoError; }

    virtual bool isConnected() const;
    virtual bool isDisconnected() const;
    virtual ErrorCode start() = 0;
    virtual void stop() = 0;

    Vpn::ConnectionState connectionState() const;
    ErrorCode lastError() const;
    QString textConnectionState() const;
    void setLastError(ErrorCode lastError);

    QString routeGateway() const;
    QString vpnGateway() const;
    QString vpnLocalAddress() const;

    static VpnProtocol* factory(amnezia::DockerContainer container, const QJsonObject &configuration);

signals:
    void bytesChanged(quint64 receivedBytes, quint64 sentBytes);
    void connectionStateChanged(Vpn::ConnectionState state);
    void timeoutTimerEvent();
    void protocolError(amnezia::ErrorCode e);
    void tunnelAddressesUpdated(const QString& gateway, const QString& localAddress);
    // Raised by a protocol when it decides the current session is dead even though
    // its state is still Connected -- e.g. post-Connect healthcheck failure.
    // Wired to VpnConnection::reconnectToVpn() so a stuck "ghost connected" tunnel
    // gets rebuilt without user intervention. by vovankrot
    void reconnectRequested();

    // The tunnel reached Connected but never carried a single received byte.
    // Deliberately NOT wired to a reconnect: the usual cause is a config that no
    // longer matches the server (port changed, obfuscation added), and retrying
    // forever hides that instead of surfacing it. by vovankrot
    void tunnelCarriesNoTraffic();

public slots:
    virtual void onTimeout(); // todo: remove?

    void setBytesChanged(quint64 receivedBytes, quint64 sentBytes);
    void setConnectionState(Vpn::ConnectionState state);

protected:
    void startTimeoutTimer();
    void stopTimeoutTimer();

    Vpn::ConnectionState m_connectionState;

    QString m_routeGateway;
    QString m_vpnLocalAddress;
    QString m_vpnGateway;

    QJsonObject m_rawConfig;

private:
    void startSilenceWatchdog();
    void stopSilenceWatchdog();

    QTimer* m_timeoutTimer;
    ErrorCode m_lastError;
    quint64 m_receivedBytes;
    quint64 m_sentBytes;

    // "Connected but nothing comes back" watchdog. Armed when the protocol reports
    // Connected, disarmed by the first received byte. Catches the whole class of
    // failures where the tunnel builds fine and then goes nowhere -- a stale client
    // config pointing at a port the server no longer listens on being the one that
    // cost the most time to find. by vovankrot
    QTimer* m_silenceTimer = nullptr;
    quint64 m_bytesAtConnect = 0;
    bool m_silenceReported = false;
    // Set by the first setBytesChanged() of a session. Protocols that never report
    // counters (desktop XRay) must not be judged by a counter that cannot move.
    bool m_bytesEverReported = false;
};

#endif // VPNPROTOCOL_H

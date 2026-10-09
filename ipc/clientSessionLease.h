#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QTimer>
#include <QDebug>
#include <functional>

// Service-local ownership across all authenticated channels. A new process
// cannot acquire shared network resources while an old owner is recovering.
class ClientSessionLease : public QObject {
public:
    using Recovery = std::function<void(std::function<void(bool)>)>;
    using IdleCheck = std::function<bool()>;
    ClientSessionLease(QObject *parent, Recovery recovery, int graceMs = 30000, IdleCheck idleCheck = {})
        : QObject(parent), m_recovery(std::move(recovery)), m_idleCheck(std::move(idleCheck)), m_graceMs(graceMs) {}
    bool attach(QLocalSocket *socket, const QString &identity) {
        if (!socket || identity.isEmpty() || m_recovering || (!m_identity.isEmpty() && m_identity != identity)) return false;
        m_identity = identity; ++m_peers; ++m_generation;
        connect(socket, &QLocalSocket::disconnected, this, [this, identity] {
            if (m_identity != identity) return;
            m_peers = qMax(0, m_peers - 1);
            if (m_peers) return;
            // Health/status probes have not acquired any resources. Holding
            // their PID for 30 seconds blocks the GUI launched by the installer.
            // Only the service's resource inventory can authorize fast release.
            if (m_idleCheck && m_idleCheck()) {
                m_identity.clear(); ++m_generation;
                qInfo() << "IPC idle client disconnected; ownership released immediately";
                return;
            }
            qInfo() << "IPC owner disconnected; retaining resources for recovery graceMs=" << m_graceMs;
            const auto generation = ++m_generation;
            QTimer::singleShot(m_graceMs, this, [this, generation] {
                if (generation != m_generation || m_peers || m_identity.isEmpty()) return;
                recover();
            });
        });
        return true;
    }
    QString identity() const { return m_identity; }
    bool available() const { return !m_identity.isEmpty() && m_peers > 0 && !m_recovering; }
private:
    void recover() {
        m_recovering = true;
        m_recovery([this](bool success) {
            if (success) { m_identity.clear(); m_recovering = false; ++m_generation; }
            else QTimer::singleShot(m_graceMs, this, [this] { recover(); });
        });
    }
    Recovery m_recovery;
    IdleCheck m_idleCheck;
    QString m_identity;
    int m_peers = 0, m_graceMs;
    quint64 m_generation = 0;
    bool m_recovering = false;
};

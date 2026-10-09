#pragma once

#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCallWatcher>
#include <QPointer>
#include <QTimer>
#include <functional>
#include "rep_ipc_interface_replica.h"
#include "rep_ipc_process_interface_replica.h"

// Wait for a replica without nesting an event loop or blocking the connection
// thread. Cancellation destroys the waiter and suppresses its callback.
class AsyncReplicaReady : public QObject
{
public:
    using Completed = std::function<void(bool)>;
    AsyncReplicaReady(QObject *parent, QRemoteObjectReplica *replica,
                      int timeoutMs, Completed completed)
        : QObject(parent), m_replica(replica), m_completed(std::move(completed))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { finish(false); });
        if (replica) {
            connect(replica, &QRemoteObjectReplica::stateChanged, this,
                    [this] { if (m_replica && m_replica->isReplicaValid()) finish(true); });
            connect(replica, &QObject::destroyed, this, [this] { finish(false); });
        }
        m_timeout.start(qMax(1, timeoutMs));
        QTimer::singleShot(0, this, [this] {
            if (!m_replica) finish(false);
            else if (m_replica->isReplicaValid()) finish(true);
        });
    }
    void cancel() { m_done = true; m_timeout.stop(); deleteLater(); }
private:
    void finish(bool ready)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop();
        const auto completed = m_completed;
        deleteLater();
        completed(ready);
    }
    QPointer<QRemoteObjectReplica> m_replica;
    Completed m_completed;
    QTimer m_timeout;
    bool m_done = false;
};

class AsyncProcessRequest : public QObject
{
public:
    using Replica = QSharedPointer<IpcProcessInterfaceReplica>;
    using Completed = std::function<void(Replica)>;
    using Endpoint = std::function<QUrl(int)>;
    AsyncProcessRequest(QObject *parent, QSharedPointer<IpcInterfaceReplica> service,
                        Endpoint endpoint, Completed completed, int timeoutMs = 10000)
        : QObject(parent), m_service(std::move(service)), m_endpoint(std::move(endpoint)),
          m_completed(std::move(completed))
    {
        // One total deadline includes service discovery, RPC and process discovery.
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { finish({}); });
        m_timeout.start(qMax(1, timeoutMs));
        m_ready = new AsyncReplicaReady(this, m_service.data(), timeoutMs,
            [this](bool ready) {
                m_ready = nullptr;
                if (m_done) return;
                if (!ready) { finish({}); return; }
                auto *watcher = new QRemoteObjectPendingCallWatcher(m_service->createPrivilegedProcess(), this);
                connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
                        [this, watcher] { created(watcher); });
                if (watcher->isFinished()) created(watcher);
            });
    }
    void cancel()
    {
        m_done = true;
        m_timeout.stop();
        if (m_ready) m_ready->cancel();
        m_process.reset();
        deleteLater();
    }
private:
    void created(QRemoteObjectPendingCallWatcher *watcher)
    {
        if (m_done || watcher->property("consumed").toBool()) return;
        watcher->setProperty("consumed", true);
        watcher->deleteLater();
        if (watcher->error() != QRemoteObjectPendingCall::NoError) { finish({}); return; }
        const int id = watcher->returnValue().toInt();
        if (id < 0) { finish({}); return; }
        auto *node = new QRemoteObjectNode;
        if (!node->connectToNode(m_endpoint(id))) { node->deleteLater(); finish({}); return; }
        m_process = Replica(node->acquire<IpcProcessInterfaceReplica>(),
            [node](IpcProcessInterfaceReplica *replica) { delete replica; node->deleteLater(); });
        m_ready = new AsyncReplicaReady(this, m_process.data(), qMax(1, m_timeout.remainingTime()),
            [this](bool ready) {
                m_ready = nullptr;
                if (!m_done) finish(ready ? m_process : Replica{});
            });
    }
    void finish(Replica process)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop();
        if (m_ready) { m_ready->cancel(); m_ready = nullptr; }
        const auto completed = m_completed;
        m_process.reset();
        deleteLater();
        completed(std::move(process));
    }
    QSharedPointer<IpcInterfaceReplica> m_service;
    Endpoint m_endpoint;
    Completed m_completed;
    Replica m_process;
    QPointer<AsyncReplicaReady> m_ready;
    QTimer m_timeout;
    bool m_done = false;
};

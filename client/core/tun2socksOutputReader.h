#pragma once

#include <QPointer>
#include <QElapsedTimer>
#include <QRemoteObjectPendingCallWatcher>
#include <functional>
#include "rep_ipc_process_interface_replica.h"
#include "core/tun2socksOutput.h"

// Keep draining for the entire process lifetime, without waiting synchronously
// on the UI thread. Coalesce notifications while an IPC read is in flight.
class Tun2SocksOutputReader : public QObject
{
public:
    Tun2SocksOutputReader(IpcProcessInterfaceReplica *process, QObject *parent,
                         std::function<bool()> active, std::function<void()> ready)
        : QObject(parent), m_process(process), m_active(std::move(active)), m_ready(std::move(ready))
    {
        m_window.start();
        connect(process, &IpcProcessInterfaceReplica::readyReadStandardOutput, this, [this] {
            m_pending = true;
            drain();
        }, Qt::QueuedConnection);
        // Also cover output produced before signal registration.
        m_pending = true;
        drain();
    }
private:
    void drain()
    {
        if (m_reading || !m_pending || !m_process || !m_active()) return;
        m_pending = false;
        m_reading = true;
        auto *watcher = new QRemoteObjectPendingCallWatcher(m_process->readAllStandardOutput(), this);
        connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
                [this, watcher] { finish(watcher); });
        // An in-process/already completed reply may precede connect().
        if (watcher->isFinished()) finish(watcher);
    }
    void finish(QRemoteObjectPendingCallWatcher *watcher)
    {
        if (watcher->property("consumed").toBool()) return;
        watcher->setProperty("consumed", true);
        watcher->deleteLater();
        m_reading = false;
        if (!m_process || !m_active()) return;
        if (watcher->error() != QRemoteObjectPendingCall::NoError) {
            qWarning() << "Failed to read tun2socks output over IPC";
            return;
        }
        const auto output = m_output.feed(watcher->returnValue().toByteArray());
        if (m_window.elapsed() >= 60000) { m_window.restart(); m_logged = 0; }
        for (const auto &line : output.diagnostics) {
            if (m_logged++ < 32) qWarning().noquote() << "[tun2socks]:" << line;
        }
        QPointer<Tun2SocksOutputReader> guard(this);
        if (output.ready) m_ready();
        if (guard) drain();
    }
    QPointer<IpcProcessInterfaceReplica> m_process;
    std::function<bool()> m_active;
    std::function<void()> m_ready;
    Tun2SocksOutput m_output;
    QElapsedTimer m_window;
    int m_logged = 0;
    bool m_reading = false;
    bool m_pending = false;
};

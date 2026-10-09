#pragma once
#include <QObject>
#include <QProcess>
#include <functional>
#include "rep_ipc_process_interface_replica.h"

// A zero exit code still means the data path is gone if this session is active.
// Explicit shutdown and stale-process notifications are filtered by the owner.
class Tun2SocksProcessObserver : public QObject
{
public:
    Tun2SocksProcessObserver(IpcProcessInterfaceReplica *process, QObject *parent,
                            std::function<bool()> active, std::function<void()> failed)
        : QObject(parent), m_active(std::move(active)), m_failed(std::move(failed))
    {
        connect(process, &IpcProcessInterfaceReplica::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                if (m_reported || !m_active()) return;
                qWarning() << "tun2socks exited during an active session, code" << code << "status" << status;
                report();
            }, Qt::QueuedConnection);
        connect(process, &IpcProcessInterfaceReplica::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || m_reported || !m_active()) return;
                qWarning() << "tun2socks could not start";
                report();
            }, Qt::QueuedConnection);
    }
private:
    void report() { m_reported = true; m_failed(); }
    std::function<bool()> m_active;
    std::function<void()> m_failed;
    bool m_reported = false;
};

#pragma once
#include <QPointer>
#include <QProcess>
#include <QElapsedTimer>
#include <QSet>
#include <algorithm>
#include "core/asyncProcessRequest.h"
#include "core/asyncIpcSequence.h"

// Drain the exact helper objects before touching their adapter/backend. No
// blocking process waits; an unconfirmed exit prevents adapter deletion.
class AsyncTunnelStop : public QObject {
public:
    using Completed = std::function<void(bool, QString)>;
    AsyncTunnelStop(QObject *parent, AsyncProcessRequest::Replica process,
                    QProcess *local, QSharedPointer<IpcInterfaceReplica> service,
                    QString device, bool xray, QSet<QString> &completedSteps, Completed completed, int exitTimeoutMs = 5000)
        : QObject(parent), m_process(std::move(process)), m_local(local),
          m_service(std::move(service)), m_device(std::move(device)), m_xray(xray), m_completed(std::move(completed)), m_completedSteps(completedSteps), m_exitTimeoutMs(exitTimeoutMs)
    {
        m_elapsed.start();
        if (m_process && !m_completedSteps.contains("helperExit")) { m_process->blockSignals(true); m_process->kill(); }
        if (m_local && m_local->state() != QProcess::NotRunning) { m_local->blockSignals(true); m_local->kill(); }
        QTimer::singleShot(0, this, [this] { poll(); });
    }
private:
    void poll() {
        if (m_elapsed.elapsed() >= m_exitTimeoutMs) { finish(false, "helperExit"); return; }
        if (m_local && m_local->state() != QProcess::NotRunning) {
            QTimer::singleShot(50, this, [this] { poll(); }); return;
        }
        if (!m_process || m_completedSteps.contains("helperExit")) { cleanup(); return; }
        auto *watcher = new QRemoteObjectPendingCallWatcher(m_process->waitForFinished(0), this);
        auto *deadline = new QTimer(watcher);
        deadline->setSingleShot(true);
        auto consume = [this, watcher, deadline](bool timedOut) {
            if (watcher->property("consumed").toBool()) return;
            watcher->setProperty("consumed", true); deadline->stop(); watcher->deleteLater();
            if (timedOut || watcher->error() != QRemoteObjectPendingCall::NoError) { finish(false, "helperExitReply"); return; }
            if (!watcher->returnValue().toBool()) { QTimer::singleShot(50, this, [this] { poll(); }); return; }
            // Close only an already-exited process, so QProcess::close cannot wait.
            m_completedSteps.insert("helperExit"); m_process->close(); cleanup();
        };
        connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this, [consume] { consume(false); });
        connect(deadline, &QTimer::timeout, this, [consume] { consume(true); });
        deadline->start(qMax(1, m_exitTimeoutMs - int(m_elapsed.elapsed())));
        if (watcher->isFinished()) consume(false);
    }
    void cleanup() {
        if (!m_service || !m_service->isReplicaValid()) { finish(false, "serviceUnavailable"); return; }
        using Step = AsyncIpcSequence::Step;
        const auto service = m_service;
        const auto device = m_device;
        std::vector<Step> steps {
            {"disableKillSwitch", [service] { return service->disableKillSwitch(); }, {}, 2000},
            {"StartRoutingIpv6", [service] { return service->StartRoutingIpv6(); }, {}, 2000},
            {"restoreResolvers", [service] { return service->restoreResolvers(); }, {}, 2000},
            {"deleteTun", [service, device] { return service->deleteTun(device); }, {}, 2000}
        };
        for (auto &step : steps) step.accepts = [](const QVariant &v) { return v.toBool(); };
        if (m_xray) steps.push_back({"xrayStop", [service] { return service->xrayStop(); }, [](const QVariant &v) { return v.toBool(); }, 2000});
        steps.erase(std::remove_if(steps.begin(), steps.end(), [this](const Step &step) { return m_completedSteps.contains(step.name); }), steps.end());
        for (auto &step : steps) {
            const auto name = step.name;
            step.accepts = [this, name](const QVariant &value) {
                if (!value.toBool()) return false;
                m_completedSteps.insert(name); return true;
            };
        }
        new AsyncIpcSequence(this, std::move(steps), [this](auto result, const QString &step) {
            finish(result == AsyncIpcSequence::Result::Success, step);
        });
    }
    void finish(bool success, const QString &step) {
        if (m_done) return;
        m_done = true; const auto completed = m_completed;
        deleteLater(); completed(success, step);
    }
    AsyncProcessRequest::Replica m_process;
    QPointer<QProcess> m_local;
    QSharedPointer<IpcInterfaceReplica> m_service;
    QString m_device;
    bool m_xray = false, m_done = false;
    Completed m_completed;
    QElapsedTimer m_elapsed;
    QSet<QString> &m_completedSteps;
    int m_exitTimeoutMs;
};

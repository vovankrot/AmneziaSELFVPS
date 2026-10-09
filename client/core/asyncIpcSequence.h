#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QRemoteObjectPendingCallWatcher>
#include <functional>
#include <vector>

// Serial IPC operations, one request at a time. A cancellation stops later
// requests; it cannot undo an operation already executing in the service.
class AsyncIpcSequence : public QObject
{
public:
    enum class Result { Success, Rejected, IpcError, Timeout };
    struct Step {
        QString name;
        std::function<QRemoteObjectPendingCall()> request;
        std::function<bool(const QVariant &)> accepts = [](const QVariant &value) { return value.toBool(); };
        int timeoutMs = 10000;
    };
    using Completed = std::function<void(Result, const QString &)>;
    AsyncIpcSequence(QObject *parent, std::vector<Step> steps, Completed completed)
        : QObject(parent), m_steps(std::move(steps)), m_completed(std::move(completed))
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { finish(Result::Timeout); });
        QTimer::singleShot(0, this, [this] { next(); });
    }
    void cancel() { m_done = true; m_timeout.stop(); deleteLater(); }
private:
    void next()
    {
        if (m_done) return;
        if (m_index == m_steps.size()) { finish(Result::Success); return; }
        const auto &step = m_steps[m_index];
        auto *watcher = new QRemoteObjectPendingCallWatcher(step.request(), this);
        m_timeout.start(step.timeoutMs);
        connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
                [this, watcher] { reply(watcher); });
        if (watcher->isFinished()) reply(watcher);
    }
    void reply(QRemoteObjectPendingCallWatcher *watcher)
    {
        if (m_done || watcher->property("consumed").toBool()) return;
        watcher->setProperty("consumed", true);
        watcher->deleteLater();
        m_timeout.stop();
        if (watcher->error() != QRemoteObjectPendingCall::NoError) { finish(Result::IpcError); return; }
        if (!m_steps[m_index].accepts(watcher->returnValue())) { finish(Result::Rejected); return; }
        ++m_index;
        // Yield between operations, including already completed local calls.
        QTimer::singleShot(0, this, [this] { next(); });
    }
    void finish(Result result)
    {
        if (m_done) return;
        m_done = true;
        m_timeout.stop();
        const QString name = m_index < m_steps.size() ? m_steps[m_index].name : QString();
        const auto completed = m_completed;
        deleteLater();
        completed(result, name);
    }
    std::vector<Step> m_steps;
    Completed m_completed;
    QTimer m_timeout;
    std::size_t m_index = 0;
    bool m_done = false;
};

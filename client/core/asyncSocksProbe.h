#pragma once
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <functional>
#include "core/socksProbe.h"

class AsyncSocksProbe : public QObject
{
public:
    using Completed = std::function<void(bool)>;
    AsyncSocksProbe(QObject *parent, QString host, quint16 port, int timeoutMs,
                    quint16 socksPort, QString user, QString password, Completed completed)
        : QObject(parent), m_canceled(std::make_shared<std::atomic_bool>(false))
    {
        auto *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, completed = std::move(completed)] {
                if (m_canceled->load()) return;
                const bool ok = watcher->result();
                m_canceled->store(true);
                deleteLater();
                completed(ok);
            });
        const auto canceled = m_canceled;
        watcher->setFuture(QtConcurrent::run(
            [host = std::move(host), port, timeoutMs, socksPort,
             user = std::move(user), password = std::move(password), canceled] {
                return SocksProbe::connect(host, port, timeoutMs, socksPort, user, password, canceled);
            }));
    }
    ~AsyncSocksProbe() override { m_canceled->store(true); }
    void cancel() { m_canceled->store(true); deleteLater(); }
private:
    std::shared_ptr<std::atomic_bool> m_canceled;
};

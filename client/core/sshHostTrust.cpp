#include "sshHostTrust.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

struct SshHostTrust::Request {
    QString endpoint, fingerprint;
    QMutex mutex;
    QWaitCondition condition;
    bool done = false, accepted = false;
};
SshHostTrust *SshHostTrust::s_instance = nullptr;
SshHostTrust::SshHostTrust(QObject *parent) : QObject(parent) { s_instance = this; }
SshHostTrust::~SshHostTrust() { answer(false); s_instance = nullptr; }

static QString trustKey(const QString &endpoint)
{
    return "SshTrust/" + QString::fromLatin1(QCryptographicHash::hash(endpoint.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool SshHostTrust::forget(const QString &host, int port)
{
    if (s_instance && s_instance->m_pending) return false;
    QSettings settings;
    settings.remove(trustKey(host.trimmed().toLower() + ":" + QString::number(port)));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool SshHostTrust::verify(const QString &host, int port, const QString &fingerprint, bool &changed)
{
    changed = false;
    const QString endpoint = host.trimmed().toLower() + ":" + QString::number(port);
    QSettings settings;
    const QString stored = settings.value(trustKey(endpoint)).toString();
    if (!stored.isEmpty()) {
        changed = stored != fingerprint;
        return !changed;
    }
    if (!s_instance || fingerprint.isEmpty() || QCoreApplication::closingDown()) return false;
    auto request = std::make_shared<Request>();
    request->endpoint = endpoint;
    request->fingerprint = fingerprint;
    QMetaObject::invokeMethod(s_instance, [request] { s_instance->showRequest(request); }, Qt::QueuedConnection);
    constexpr int timeoutMs = 120000;
    QElapsedTimer deadline;
    deadline.start();
    if (QThread::currentThread() == s_instance->thread()) {
        // Some installation entry points run synchronously on the GUI thread.
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            QMutexLocker lock(&request->mutex);
            if (request->done || deadline.elapsed() >= timeoutMs) loop.quit();
        });
        poll.start(50);
        loop.exec();
    } else {
        QMutexLocker lock(&request->mutex);
        while (!request->done && deadline.elapsed() < timeoutMs)
            request->condition.wait(&request->mutex, timeoutMs - deadline.elapsed());
    }
    QMutexLocker lock(&request->mutex);
    if (!request->done) {
        request->done = true;
        QMetaObject::invokeMethod(s_instance, [request] {
            if (s_instance->m_pending == request) {
                s_instance->m_pending.reset();
                emit s_instance->confirmationExpired();
            }
        }, Qt::QueuedConnection);
    }
    return request->accepted;
}

void SshHostTrust::showRequest(const std::shared_ptr<Request> &request)
{
    QMutexLocker lock(&request->mutex);
    if (request->done) return;
    // A second concurrent unknown server must not overwrite the visible prompt.
    if (m_pending) { request->done = true; request->condition.wakeAll(); return; }
    QSettings settings;
    if (settings.value(trustKey(request->endpoint)).toString() == request->fingerprint) {
        request->accepted = request->done = true;
        request->condition.wakeAll();
        return;
    }
    m_pending = request;
    lock.unlock();
    emit confirmationRequested(request->endpoint, request->fingerprint);
}

void SshHostTrust::answer(bool accepted)
{
    if (!m_pending) return;
    auto request = m_pending;
    m_pending.reset();
    QMutexLocker lock(&request->mutex);
    if (request->done) return;
    if (accepted) {
        QSettings settings;
        // Never silently overwrite an existing pin, including concurrent requests.
        const QString stored = settings.value(trustKey(request->endpoint)).toString();
        if (stored.isEmpty() || stored == request->fingerprint) {
            settings.setValue(trustKey(request->endpoint), request->fingerprint);
            settings.sync();
            request->accepted = settings.status() == QSettings::NoError;
        }
    }
    request->done = true;
    request->condition.wakeAll();
}

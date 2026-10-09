#pragma once

#include <QHash>
#include <QHostInfo>
#include <QHostAddress>
#include <QVector>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <functional>

// A pre-connect DNS snapshot. Does not install routes or alter saved settings.
// All callbacks run on the owner's thread; cancel suppresses late completion.
class AsyncSiteDnsRefresh : public QObject {
public:
    struct Entry { QString hostname; QStringList savedIps; };
    struct Result {
        QHash<QString, QStringList> addresses;
        int unresolved = 0;
        bool timedOut = false;
    };
    using Reply = std::function<void(const QHostInfo &)>;
    using Lookup = std::function<int(const QString &, QObject *, Reply)>;
    using Abort = std::function<void(int)>;
    using Completed = std::function<void(Result)>;

    static QString hostname(QString text) {
        text = text.trimmed();
        if (text.contains('*') || text.contains('/')) {
            if (!text.contains("://") || text.contains('*')) return {};
            const QUrl url(text);
            if (url.scheme() != "http" && url.scheme() != "https") return {};
            text = url.host();
        }
        if (text.endsWith('.')) text.chop(1);
        text = QString::fromLatin1(QUrl::toAce(text.toLower()));
        static const QRegularExpression valid(
            R"(^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*$)");
        return text.size() <= 253 && QHostAddress(text).isNull() && valid.match(text).hasMatch()
            ? text : QString();
    }

    AsyncSiteDnsRefresh(QObject *owner, const QVector<Entry> &entries, Completed completed,
                        int timeoutMs = 10000, int concurrency = 16,
                        Lookup lookup = {}, Abort abort = {})
        : QObject(owner), m_completed(std::move(completed)), m_limit(qBound(1, concurrency, 16)),
          m_lookup(std::move(lookup)), m_abort(std::move(abort)) {
        if (!m_lookup) m_lookup = [](const QString &host, QObject *context, Reply reply) {
            return QHostInfo::lookupHost(host, context, std::move(reply));
        };
        if (!m_abort) m_abort = [](int id) { QHostInfo::abortHostLookup(id); };
        for (const auto &entry : entries) {
            const QString host = hostname(entry.hostname);
            if (host.isEmpty()) continue;
            if (!m_result.addresses.contains(host)) {
                m_hosts.append(host);
                m_result.addresses.insert(host, {});
            }
            auto &saved = m_result.addresses[host];
            for (const auto &ip : entry.savedIps) {
                const QHostAddress address(ip);
                if (address.protocol() == QAbstractSocket::IPv4Protocol
                    && !saved.contains(address.toString())) saved.append(address.toString());
            }
        }
        m_deadline.setSingleShot(true);
        m_deadline.setParent(this);
        connect(&m_deadline, &QTimer::timeout, this, [this] { finish(true); });
        // Never invoke the completion inside the constructor.
        QTimer::singleShot(0, this, [this, timeoutMs] {
            if (m_done) return;
            m_deadline.start(qMax(1, timeoutMs));
            pump();
        });
    }

    ~AsyncSiteDnsRefresh() override { cancelLookups(); }
    void cancel() {
        if (m_done) return;
        m_done = true;
        m_completed = {};
        m_deadline.stop();
        cancelLookups();
        deleteLater();
    }

private:
    void cancelLookups() {
        const auto ids = m_ids;
        m_ids.clear();
        for (int id : ids) if (id >= 0) m_abort(id);
    }
    void pump() {
        if (m_done) return;
        while (m_ids.size() < m_limit && m_next < m_hosts.size()) {
            const QString host = m_hosts.at(m_next++);
            m_ids.insert(host, -1);
            const QPointer<AsyncSiteDnsRefresh> guard(this);
            const int id = m_lookup(host, this, [guard, host](const QHostInfo &info) {
                if (!guard || guard->m_done || !guard->m_ids.contains(host)) return;
                guard->m_ids.remove(host);
                QStringList fresh;
                if (info.error() == QHostInfo::NoError) {
                    for (const auto &address : info.addresses()) {
                        if (address.protocol() == QAbstractSocket::IPv4Protocol
                            && !fresh.contains(address.toString())) fresh.append(address.toString());
                    }
                }
                if (!fresh.isEmpty()) guard->m_result.addresses[host] = fresh;
                else ++guard->m_result.unresolved;
                guard->pump();
            });
            if (m_ids.contains(host)) m_ids[host] = id;
        }
        if (m_next == m_hosts.size() && m_ids.isEmpty()) finish(false);
    }
    void finish(bool timedOut) {
        if (m_done) return;
        m_done = true;
        m_deadline.stop();
        if (timedOut) m_result.unresolved += m_ids.size() + m_hosts.size() - m_next;
        m_result.timedOut = timedOut;
        cancelLookups();
        auto completed = std::move(m_completed);
        deleteLater();
        if (completed) completed(std::move(m_result));
    }
    bool m_done = false;
    Completed m_completed;
    int m_limit, m_next = 0;
    Lookup m_lookup;
    Abort m_abort;
    QStringList m_hosts;
    QHash<QString, int> m_ids;
    QTimer m_deadline;
    Result m_result;
};

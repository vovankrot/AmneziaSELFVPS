#pragma once

#include <QObject>
#include <QString>
#include <memory>

// Created on the GUI thread. Requests from SSH workers are bounded and resolved
// by the user before any authentication data is sent to the server.
class SshHostTrust : public QObject
{
    Q_OBJECT
public:
    explicit SshHostTrust(QObject *parent = nullptr);
    ~SshHostTrust() override;
    static bool verify(const QString &host, int port, const QString &fingerprint, bool &changed);
    static bool forget(const QString &host, int port);
    Q_INVOKABLE void answer(bool accepted);
signals:
    void confirmationRequested(const QString &endpoint, const QString &fingerprint);
    void confirmationExpired();
private:
    struct Request;
    void showRequest(const std::shared_ptr<Request> &request);
    std::shared_ptr<Request> m_pending;
    static SshHostTrust *s_instance;
};

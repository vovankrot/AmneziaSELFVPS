#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QTimer>
#include <memory>
#include "releaseManifest.h"

class AppUpdater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY changed)
    Q_PROPERTY(QString repository READ repository WRITE setRepository NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(bool supported READ supported CONSTANT)
public:
    explicit AppUpdater(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    QString status() const { return m_status; }
    QString availableVersion() const { return m_release.version; }
    QString releaseNotes() const { return m_release.notes; }
    QString repository() const;
    void setRepository(const QString &repo);
    bool automatic() const;
    void setAutomatic(bool value);
    bool busy() const { return !m_reply.isNull(); }
    bool ready() const { return !m_readyPath.isEmpty(); }
    int progress() const { return m_progress; }
    bool supported() const;
    Q_INVOKABLE void check(bool manual = true);
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void install();
    Q_INVOKABLE void openRelease();
signals:
    void changed();
    void updateAvailable();
private:
    friend class UpdateTests;
    void fail(const QString &message);
    QNetworkRequest request(const QUrl &url) const;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_periodic;
    SelfVpsRelease::Release m_release;
    QString m_status, m_readyPath, m_downloadRoot;
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    QByteArray m_metadata;
    qint64 m_received = 0;
    int m_progress = 0;
    bool m_failed = false;
};

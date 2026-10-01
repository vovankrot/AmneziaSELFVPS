#include "blocklistUpdater.h"
#include "settings.h"
#include "listValidation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QDebug>

BlocklistUpdater::BlocklistUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent)
    : QObject(parent), m_nam(nam), m_settings(std::move(settings))
{
    m_timer.setInterval(std::chrono::hours(24));
    connect(&m_timer, &QTimer::timeout, this, &BlocklistUpdater::checkAndUpdate);
}

QString BlocklistUpdater::localCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/rkn_blocked_domains.txt");
}

QString BlocklistUpdater::domainsFilePath(const std::shared_ptr<Settings> &settings)
{
    const QString cached = localCachePath();
    if (QFileInfo::exists(cached) && QFileInfo(cached).size() > 0) {
        return cached;
    }
    return QString(); // no fallback — feature is additive
}

void BlocklistUpdater::startPeriodicUpdates()
{
    checkAndUpdate();
    m_timer.start();
}

void BlocklistUpdater::checkAndUpdate()
{
    if (!m_settings->isAutoBypassRknEnabled()) {
        return;
    }

    const QDateTime lastUpdate = m_settings->rknBlocklistLastUpdate();
    if (lastUpdate.isValid() && lastUpdate.daysTo(QDateTime::currentDateTime()) < UpdateIntervalDays) {
        if (QFileInfo::exists(localCachePath())) {
            qDebug() << "BlocklistUpdater: cache is fresh, last update" << lastUpdate;
            return;
        }
    }
    download();
}

void BlocklistUpdater::download()
{
    if (m_updating) return;
    m_updating = true;
    qDebug() << "BlocklistUpdater: downloading RKN blocked domains from" << DownloadUrl;

    QNetworkRequest request(QUrl(QString::fromLatin1(DownloadUrl)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = m_nam->get(request);
    ListValidation::limitDownload(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleReply(reply);
    });
}

void BlocklistUpdater::handleReply(QNetworkReply *reply)
{
    m_updating = false;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "BlocklistUpdater: download failed:" << reply->errorString();
        emit updateFinished(false, 0);
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        qWarning() << "BlocklistUpdater: received empty response";
        emit updateFinished(false, 0);
        return;
    }

    QByteArray normalized;
    int validCount = 0;
    if (!ListValidation::normalize(data, false, normalized, validCount)) {
        qWarning() << "BlocklistUpdater: invalid or oversized domain list";
        emit updateFinished(false, 0);
        return;
    }

    if (validCount < 50) {
        qWarning() << "BlocklistUpdater: suspiciously few domains:" << validCount;
        emit updateFinished(false, 0);
        return;
    }

    const QString cachePath = localCachePath();
    if (!ListValidation::save(cachePath, normalized)) {
        qWarning() << "BlocklistUpdater: atomic cache replacement failed";
        emit updateFinished(false, 0);
        return;
    }

    m_settings->setRknBlocklistLastUpdate(QDateTime::currentDateTime());

    qInfo() << "BlocklistUpdater: saved" << validCount << "blocked domains to" << cachePath;
    emit updateFinished(true, validCount);
}

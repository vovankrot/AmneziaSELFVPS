#include "geoipUpdater.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QDebug>

GeoipUpdater::GeoipUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent)
    : QObject(parent), m_nam(nam), m_settings(std::move(settings))
{
    m_timer.setInterval(std::chrono::hours(24));
    connect(&m_timer, &QTimer::timeout, this, &GeoipUpdater::checkAndUpdate);
}

QString GeoipUpdater::localCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/geoip_ru_cidrs.txt");
}

QString GeoipUpdater::cidrFilePath(const std::shared_ptr<Settings> &settings)
{
    const QString cached = localCachePath();
    if (QFileInfo::exists(cached) && QFileInfo(cached).size() > 0) {
        return cached;
    }
    return QStringLiteral(":/resources/geoip_ru_cidrs.txt");
}

void GeoipUpdater::startPeriodicUpdates()
{
    checkAndUpdate();
    m_timer.start();
}

void GeoipUpdater::checkAndUpdate()
{
    const QDateTime lastUpdate = m_settings->geoIpLastUpdate();
    if (lastUpdate.isValid() && lastUpdate.daysTo(QDateTime::currentDateTime()) < UpdateIntervalDays) {
        if (QFileInfo::exists(localCachePath())) {
            qDebug() << "GeoipUpdater: cache is fresh, last update" << lastUpdate;
            return;
        }
    }
    download();
}

void GeoipUpdater::download()
{
    qDebug() << "GeoipUpdater: downloading fresh RU CIDRs from" << DownloadUrl;

    QNetworkRequest request(QUrl(QString::fromLatin1(DownloadUrl)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleReply(reply);
    });
}

void GeoipUpdater::handleReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "GeoipUpdater: download failed:" << reply->errorString();
        emit updateFinished(false, 0);
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        qWarning() << "GeoipUpdater: received empty response";
        emit updateFinished(false, 0);
        return;
    }

    // Validate: each non-empty line must look like a CIDR (contain '/' and '.')
    const QList<QByteArray> lines = data.split('\n');
    int validCount = 0;
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        if (!trimmed.contains('.') || !trimmed.contains('/')) {
            qWarning() << "GeoipUpdater: invalid CIDR line, aborting:" << trimmed.left(80);
            emit updateFinished(false, 0);
            return;
        }
        ++validCount;
    }

    if (validCount < 100) {
        qWarning() << "GeoipUpdater: suspiciously few CIDRs:" << validCount;
        emit updateFinished(false, 0);
        return;
    }

    // Write atomically: write to temp, then rename
    const QString cachePath = localCachePath();
    const QString tempPath = cachePath + QStringLiteral(".tmp");

    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "GeoipUpdater: cannot write temp file:" << tempFile.errorString();
        emit updateFinished(false, 0);
        return;
    }
    tempFile.write(data);
    tempFile.close();

    // Remove old cache, rename temp → cache
    QFile::remove(cachePath);
    if (!QFile::rename(tempPath, cachePath)) {
        qWarning() << "GeoipUpdater: rename failed";
        QFile::remove(tempPath);
        emit updateFinished(false, 0);
        return;
    }

    m_settings->setGeoIpLastUpdate(QDateTime::currentDateTime());

    qInfo() << "GeoipUpdater: saved" << validCount << "RU CIDRs to" << cachePath;
    emit updateFinished(true, validCount);
}

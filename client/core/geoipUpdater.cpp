#include "geoipUpdater.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDebug>

GeoipUpdater::GeoipUpdater(QNetworkAccessManager *nam, std::shared_ptr<Settings> settings, QObject *parent)
    : QObject(parent), m_nam(nam), m_settings(std::move(settings))
{
    connect(&m_timer, &QTimer::timeout, this, &GeoipUpdater::checkAndUpdate);
    applyTimerInterval();
}

QString GeoipUpdater::localCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/geoip_ru_cidrs.txt");
}

QString GeoipUpdater::cidrFilePath(const std::shared_ptr<Settings> &settings)
{
    Q_UNUSED(settings)
    const QString cached = localCachePath();
    if (QFileInfo::exists(cached) && QFileInfo(cached).size() > 0) {
        return cached;
    }
    return QStringLiteral(":/resources/geoip_ru_cidrs.txt");
}

// ---------------------------------------------------------------- settings ----

QString GeoipUpdater::sourceUrl() const
{
    return m_settings->geoIpSourceUrl();
}

void GeoipUpdater::setSourceUrl(const QString &url)
{
    if (url.trimmed() == m_settings->geoIpSourceUrl()) {
        return;
    }
    m_settings->setGeoIpSourceUrl(url);
    emit statusChanged();
}

int GeoipUpdater::intervalHours() const
{
    return m_settings->geoIpUpdateIntervalHours();
}

void GeoipUpdater::setIntervalHours(int hours)
{
    if (hours == m_settings->geoIpUpdateIntervalHours()) {
        return;
    }
    m_settings->setGeoIpUpdateIntervalHours(hours);
    applyTimerInterval();
    emit statusChanged();
}

void GeoipUpdater::applyTimerInterval()
{
    // Wake up at most once per configured interval, but never rarer than once a day
    // so a long interval still re-checks after the machine has been off.
    const int hours = qBound(1, m_settings->geoIpUpdateIntervalHours(), 24 * 30);
    m_timer.setInterval(std::chrono::hours(qMin(hours, 24)));
}

// ----------------------------------------------------------------- status ----

QString GeoipUpdater::lastUpdateText() const
{
    const QDateTime dt = m_settings->geoIpLastUpdate();
    if (!dt.isValid()) {
        return tr("never");
    }
    return QLocale().toString(dt, QLocale::ShortFormat);
}

int GeoipUpdater::cidrCount() const
{
    return m_settings->geoIpLastCount();
}

QString GeoipUpdater::listPath() const
{
    return cidrFilePath(m_settings);
}

bool GeoipUpdater::usingBundledList() const
{
    return listPath().startsWith(QLatin1Char(':'));
}

// ---------------------------------------------------------------- updating ----

void GeoipUpdater::startPeriodicUpdates()
{
    checkAndUpdate();
    m_timer.start();
}

void GeoipUpdater::updateNow()
{
    download();
}

void GeoipUpdater::resetSourceToDefault()
{
    setSourceUrl(QString::fromLatin1(Settings::defaultGeoIpSourceUrl));
}

void GeoipUpdater::checkAndUpdate()
{
    const QDateTime lastUpdate = m_settings->geoIpLastUpdate();
    const int intervalH = m_settings->geoIpUpdateIntervalHours();
    if (lastUpdate.isValid() && lastUpdate.secsTo(QDateTime::currentDateTime()) < qint64(intervalH) * 3600) {
        if (QFileInfo::exists(localCachePath())) {
            qDebug() << "GeoipUpdater: cache is fresh, last update" << lastUpdate << "interval(h)" << intervalH;
            return;
        }
    }
    download();
}

void GeoipUpdater::download()
{
    if (m_updating) {
        return;
    }

    const QString url = m_settings->geoIpSourceUrl();
    qDebug() << "GeoipUpdater: downloading CIDR list from" << url;

    m_updating = true;
    m_lastError.clear();
    emit statusChanged();

    QNetworkRequest request { QUrl(url) };
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

    auto fail = [this](const QString &why, int count = 0) {
        m_updating = false;
        m_lastError = why;
        qWarning() << "GeoipUpdater:" << why;
        emit statusChanged();
        emit updateFinished(false, count);
    };

    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("download failed: %1").arg(reply->errorString()));
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        fail(tr("received empty response"));
        return;
    }

    // Normalise rather than store verbatim: vpnconnection.cpp turns EVERY non-empty line of
    // this file into a route, so a header comment (the RIR-derived lists start with a few
    // "# Country: ..." lines) would be injected as a bogus route. Keep only real CIDRs, drop
    // blanks and comments, and reject anything else -- that last part is what stops an HTML
    // error page from being saved as a "list". by vovankrot
    static const QRegularExpression cidrRe(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}/\\d{1,2}$"));

    QByteArray normalized;
    normalized.reserve(data.size());
    int validCount = 0;

    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith(';')) {
            continue;
        }
        if (!cidrRe.match(QString::fromUtf8(trimmed)).hasMatch()) {
            fail(tr("invalid CIDR line: %1").arg(QString::fromUtf8(trimmed.left(60))));
            return;
        }
        normalized.append(trimmed);
        normalized.append('\n');
        ++validCount;
    }

    if (validCount < 100) {
        fail(tr("suspiciously few CIDRs: %1").arg(validCount));
        return;
    }

    // Write atomically: write to temp, then rename
    const QString cachePath = localCachePath();
    const QString tempPath = cachePath + QStringLiteral(".tmp");

    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("cannot write cache: %1").arg(tempFile.errorString()));
        return;
    }
    tempFile.write(normalized);
    tempFile.close();

    QFile::remove(cachePath);
    if (!QFile::rename(tempPath, cachePath)) {
        QFile::remove(tempPath);
        fail(tr("cannot replace cache file"));
        return;
    }

    m_settings->setGeoIpLastUpdate(QDateTime::currentDateTime());
    m_settings->setGeoIpLastCount(validCount);

    m_updating = false;
    m_lastError.clear();

    qInfo() << "GeoipUpdater: saved" << validCount << "CIDRs to" << cachePath;
    emit statusChanged();
    emit updateFinished(true, validCount);
}

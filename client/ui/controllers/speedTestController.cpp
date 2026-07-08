#include "speedTestController.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTextStream>

#include "containers/containers_defs.h"
#include "logger.h"
#include "ui/models/servers_model.h"

namespace {
Logger logger("SpeedTestController");

constexpr int kTransferTimeoutMs = 45000;
constexpr int kLatencyAttempts = 3;
constexpr int kLatencyTimeoutMs = 4000;
constexpr int kDownloadPhaseDurationMs = 10000;
constexpr int kUploadPhaseDurationMs = 10000;
constexpr int kProgressUpdateIntervalMs = 200;
constexpr int kRequestProbeTimeoutMs = 2500;
constexpr int kParallelDownloadRequests = 3;
constexpr int kParallelUploadRequests = 3;
constexpr int kUploadPayloadSizeBytes = 16 * 1024 * 1024;
constexpr qint64 kDownloadProbeMinBytes = 256 * 1024;
constexpr quint16 kFallbackServerPort = 443;

const QString kMoscowProbeHost = QStringLiteral("ya.ru");

quint16 portFromJsonValue(const QJsonValue &value, quint16 fallback = 0)
{
    bool ok = false;

    const QString portString = value.toString().trimmed();
    if (!portString.isEmpty()) {
        const quint16 parsedPort = portString.toUShort(&ok);
        if (ok && parsedPort > 0) {
            return parsedPort;
        }
    }

    const int intPort = value.toInt(0);
    if (intPort > 0 && intPort <= 65535) {
        return static_cast<quint16>(intPort);
    }

    return fallback;
}

QJsonObject defaultServerConfig(ServersModel *serversModel)
{
    if (!serversModel) {
        return {};
    }

    const int defaultServerIndex = serversModel->getDefaultServerIndex();
    if (defaultServerIndex < 0 || defaultServerIndex >= serversModel->getServersCount()) {
        return {};
    }

    return serversModel->getServerConfig(defaultServerIndex);
}

QJsonObject defaultContainerConfig(const QJsonObject &serverConfig, DockerContainer *defaultContainerOut = nullptr)
{
    const DockerContainer defaultContainer =
        ContainerProps::containerFromString(serverConfig.value(config_key::defaultContainer).toString());

    if (defaultContainerOut) {
        *defaultContainerOut = defaultContainer;
    }

    const QJsonArray containers = serverConfig.value(config_key::containers).toArray();
    for (const QJsonValue &containerValue : containers) {
        const QJsonObject containerConfig = containerValue.toObject();
        if (containerConfig.value(config_key::container).toString()
            == ContainerProps::containerToString(defaultContainer)) {
            return containerConfig;
        }
    }

    return {};
}

QJsonObject defaultProtocolConfig(const QJsonObject &serverConfig, Proto *defaultProtocolOut = nullptr)
{
    DockerContainer defaultContainer = DockerContainer::None;
    const QJsonObject containerConfig = defaultContainerConfig(serverConfig, &defaultContainer);

    if (containerConfig.isEmpty()) {
        if (defaultProtocolOut) {
            *defaultProtocolOut = Proto::Any;
        }
        return {};
    }

    const Proto defaultProtocol = ContainerProps::defaultProtocol(defaultContainer);
    if (defaultProtocolOut) {
        *defaultProtocolOut = defaultProtocol;
    }

    return containerConfig.value(ContainerProps::containerTypeToProtocolString(defaultContainer)).toObject();
}

QJsonObject protocolLastConfig(const QJsonObject &protocolConfig)
{
    return QJsonDocument::fromJson(protocolConfig.value(config_key::last_config).toString().toUtf8()).object();
}

QString configuredXrayEndpointHost(const QJsonObject &protocolConfig)
{
    const QJsonArray outbounds = protocolLastConfig(protocolConfig).value(QStringLiteral("outbounds")).toArray();
    for (const QJsonValue &outboundValue : outbounds) {
        const QJsonObject outbound = outboundValue.toObject();
        if (outbound.value(QStringLiteral("tag")).toString() != QStringLiteral("proxy")) {
            continue;
        }

        const QJsonArray vnext = outbound.value(QStringLiteral("settings")).toObject()
            .value(QStringLiteral("vnext")).toArray();
        if (vnext.isEmpty()) {
            continue;
        }

        const QString host = vnext.first().toObject().value(QStringLiteral("address")).toString().trimmed();
        if (!host.isEmpty()) {
            return host;
        }
    }

    return {};
}

quint16 configuredXrayEndpointPort(const QJsonObject &protocolConfig)
{
    const QJsonArray outbounds = protocolLastConfig(protocolConfig).value(QStringLiteral("outbounds")).toArray();
    for (const QJsonValue &outboundValue : outbounds) {
        const QJsonObject outbound = outboundValue.toObject();
        if (outbound.value(QStringLiteral("tag")).toString() != QStringLiteral("proxy")) {
            continue;
        }

        const QJsonArray vnext = outbound.value(QStringLiteral("settings")).toObject()
            .value(QStringLiteral("vnext")).toArray();
        if (vnext.isEmpty()) {
            continue;
        }

        const quint16 port = portFromJsonValue(vnext.first().toObject().value(QStringLiteral("port")));
        if (port > 0) {
            return port;
        }
    }

    return 0;
}

double bytesToMbps(qint64 bytesTransferred, qint64 elapsedMs)
{
    if (bytesTransferred <= 0 || elapsedMs <= 0) {
        return 0.0;
    }

    return (bytesTransferred * 8.0) / (elapsedMs * 1000.0);
}

void appendSpeedTestLog(const QString &message)
{
    static const QString logPath = QDir::tempPath() + "/amnezia_speedtest.log";
    QFile file(logPath);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz")
               << " " << message << "\n";
    }
}

void logSpeedTestInfo(const QString &message)
{
    logger.info() << message;
    appendSpeedTestLog(message);
}

void logSpeedTestWarning(const QString &message)
{
    logger.warning() << message;
    appendSpeedTestLog(message);
}

QString takeRandomUrl(QStringList &urls,
                      const QStringList &sourceUrls,
                      bool allowRefill,
                      const QString &excludedUrl = QString())
{
    if (urls.isEmpty() && allowRefill) {
        urls = sourceUrls;
    }

    if (urls.isEmpty()) {
        return {};
    }

    QStringList candidates = urls;
    if (!excludedUrl.isEmpty()) {
        candidates.removeAll(excludedUrl);
        if (candidates.isEmpty()) {
            candidates = urls;
        }
    }

    const int index = QRandomGenerator::global()->bounded(candidates.size());
    const QString selectedUrl = candidates.at(index);
    urls.removeOne(selectedUrl);
    return selectedUrl;
}

QString httpStatusSuffix(QNetworkReply *reply)
{
    if (!reply) {
        return QString();
    }

    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status.isValid()) {
        return QString();
    }

    return QStringLiteral(" (HTTP %1)").arg(status.toInt());
}

// HTTP HEAD-based latency probe.  Measures real round-trip time through the
// full network path including any VPN tunnel, unlike raw TCP connect which
// can be short-circuited by tun2socks and show ~4 ms.  A completed TLS
// handshake also counts as success, so VPN endpoints that accept TLS but do
// not behave like a normal HTTPS server still produce a usable RTT.
class HttpLatencyProbe final : public QObject
{
public:
    using Callback = std::function<void(int, bool)>;

    HttpLatencyProbe(QNetworkAccessManager *nam, QUrl url, int attempts, int timeoutMs,
                     Callback callback, QObject *parent = nullptr)
        : QObject(parent)
        , m_nam(nam)
        , m_url(std::move(url))
        , m_attempts((std::max)(1, attempts))
        , m_timeoutMs((std::max)(1, timeoutMs))
        , m_callback(std::move(callback))
    {
        m_overallTimer.setSingleShot(true);
        connect(&m_overallTimer, &QTimer::timeout, this, [this]() {
            abortCurrentReply();
            finish(m_successfulAttempts > 0);
        });
    }

    void start()
    {
        if (!m_nam || m_url.isEmpty()) {
            finish(false);
            return;
        }
        m_overallTimer.start(m_timeoutMs);
        startAttempt();
    }

private:
    void startAttempt()
    {
        QNetworkRequest request(m_url);
        request.setTransferTimeout(m_timeoutMs);
        // Disable caching so every probe is a real network trip
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);

        m_attemptElapsed.restart();
        m_currentReply = m_nam->head(request);

        // Ignore SSL certificate errors: we only need the round-trip time,
        // not certificate validation (REALITY servers present a camouflage
        // cert that won't match the raw IP address we connect to).
        m_currentReply->ignoreSslErrors();

        connect(m_currentReply, &QNetworkReply::encrypted, this, [this]() {
            completeAttempt(true);
        });

        connect(m_currentReply, &QNetworkReply::finished, this, [this]() {
            completeAttempt(m_currentReply->error() == QNetworkReply::NoError);
        });
    }

    void completeAttempt(bool success)
    {
        const qint64 elapsed = m_attemptElapsed.elapsed();
        abortCurrentReply();

        if (success) {
            // Keep the minimum RTT across attempts (most representative of
            // pure network latency, excluding server processing variance).
            if (m_bestLatencyMs < 0 || elapsed < m_bestLatencyMs) {
                m_bestLatencyMs = elapsed;
            }
            ++m_successfulAttempts;
        }

        ++m_completedAttempts;
        if (m_completedAttempts < m_attempts && m_overallTimer.isActive()) {
            startAttempt();
            return;
        }

        finish(m_successfulAttempts > 0);
    }

    void abortCurrentReply()
    {
        if (!m_currentReply) {
            return;
        }
        m_currentReply->disconnect(this);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    void finish(bool success)
    {
        m_overallTimer.stop();
        abortCurrentReply();

        const int latencyMs = success ? static_cast<int>(m_bestLatencyMs) : -1;
        if (m_callback) {
            m_callback(latencyMs, success);
        }
        deleteLater();
    }

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_url;
    int m_attempts = 1;
    int m_timeoutMs = 0;
    Callback m_callback;
    QNetworkReply *m_currentReply = nullptr;
    QTimer m_overallTimer;
    QElapsedTimer m_attemptElapsed;
    qint64 m_bestLatencyMs = -1;
    int m_successfulAttempts = 0;
    int m_completedAttempts = 0;
};
}

const QStringList SpeedTestController::s_downloadUrls = {
    QStringLiteral("https://proof.ovh.net/files/100Mb.dat"),
    QStringLiteral("https://cachefly.cachefly.net/100mb.test"),
    QStringLiteral("https://download.thinkbroadband.com/100MB.zip")
};

const QStringList SpeedTestController::s_uploadUrls = {
    QStringLiteral("https://speed.cloudflare.com/__up")
};

SpeedTestController::SpeedTestController(ServersModel *serversModel, QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_serversModel(serversModel)
{
    m_phaseLimitTimer.setSingleShot(true);
    connect(&m_phaseLimitTimer, &QTimer::timeout, this, &SpeedTestController::onPhaseTimeout);
}

SpeedTestController::~SpeedTestController()
{
    m_phaseLimitTimer.stop();
    clearActiveReplies();
}

bool SpeedTestController::isRunning() const { return m_isRunning; }
double SpeedTestController::downloadSpeed() const { return m_downloadSpeed; }
double SpeedTestController::uploadSpeed() const { return m_uploadSpeed; }
QString SpeedTestController::statusText() const { return m_statusText; }
QString SpeedTestController::serverPingText() const { return m_serverPingText; }
QString SpeedTestController::moscowPingText() const { return m_moscowPingText; }

void SpeedTestController::runSpeedTest()
{
    if (m_isRunning) {
        return;
    }

    m_pendingDownloadUrls = s_downloadUrls;
    m_pendingUploadUrls = s_uploadUrls;
    m_uploadPayload.clear();
    m_phase = Phase::Latency;
    m_phaseStopRequested = false;

    setDownloadSpeed(0.0);
    setUploadSpeed(0.0);
    setServerPingText(QStringLiteral("..."));
    setMoscowPingText(QStringLiteral("..."));
    setIsRunning(true);
    setStatusText(tr("Measuring latency..."));

    logSpeedTestInfo(QStringLiteral("Starting speed test with %1s download and %2s upload phases")
                     .arg(kDownloadPhaseDurationMs / 1000)
                     .arg(kUploadPhaseDurationMs / 1000));

    startLatencyMeasurements();
}

void SpeedTestController::startLatencyMeasurements()
{
    m_pendingLatencyProbes = 0;

    const auto handleProbeFinished = [this](const QString &targetName, bool isMoscow, int latencyMs, bool success) {
        const QString latencyText = success ? QStringLiteral("%1 ms").arg(latencyMs) : QStringLiteral("n/a");
        if (isMoscow) {
            setMoscowPingText(latencyText);
        } else {
            setServerPingText(latencyText);
        }

        if (success) {
            logSpeedTestInfo(QStringLiteral("Latency probe to %1 finished: %2 ms")
                             .arg(targetName)
                             .arg(latencyMs));
        } else {
            logSpeedTestWarning(QStringLiteral("Latency probe to %1 failed")
                                .arg(targetName));
        }

        --m_pendingLatencyProbes;
        if (m_pendingLatencyProbes == 0) {
            startDownloadTest();
        }
    };

    const QString serverHost = currentServerHost();
    const quint16 serverPort = currentServerPort();
    if (!serverHost.isEmpty()) {
        ++m_pendingLatencyProbes;
        const QUrl serverUrl = QUrl(QStringLiteral("https://%1:%2/").arg(serverHost).arg(serverPort));
        logSpeedTestInfo(QStringLiteral("Starting server latency probe (HTTP HEAD) against %1")
                         .arg(serverUrl.toString()));
        auto *serverProbe = new HttpLatencyProbe(
            m_nam,
            serverUrl,
            kLatencyAttempts,
            kLatencyTimeoutMs,
            [handleProbeFinished, serverHost, serverPort](int latencyMs, bool success) {
                handleProbeFinished(QStringLiteral("%1:%2").arg(serverHost).arg(serverPort), false, latencyMs, success);
            },
            this);
        serverProbe->start();
    } else {
        setServerPingText(QStringLiteral("n/a"));
    }

    ++m_pendingLatencyProbes;
    const QUrl moscowUrl = QUrl(QStringLiteral("https://%1/").arg(kMoscowProbeHost));
    logSpeedTestInfo(QStringLiteral("Starting Moscow latency probe (HTTP HEAD) against %1")
                     .arg(moscowUrl.toString()));
    auto *moscowProbe = new HttpLatencyProbe(
        m_nam,
        moscowUrl,
        kLatencyAttempts,
        kLatencyTimeoutMs,
        [handleProbeFinished](int latencyMs, bool success) {
            handleProbeFinished(kMoscowProbeHost, true, latencyMs, success);
        },
        this);
    moscowProbe->start();

    if (m_pendingLatencyProbes == 0) {
        startDownloadTest();
    }
}

void SpeedTestController::startDownloadTest()
{
    if (m_pendingDownloadUrls.isEmpty()) {
        finishWithError(tr("Download test failed"), tr("No download endpoints available"));
        return;
    }

    m_phase = Phase::Download;
    resetPhaseTransferState();
    setStatusText(tr("Testing download..."));
    m_phaseLimitTimer.start(kDownloadPhaseDurationMs);

    logSpeedTestInfo(QStringLiteral("Starting download phase for %1 seconds with %2 parallel streams")
                     .arg(kDownloadPhaseDurationMs / 1000)
                     .arg(kParallelDownloadRequests));

    ensureDownloadConcurrency(true);

    if (m_activeReplies.isEmpty()) {
        finishWithError(tr("Download test failed"), tr("No download endpoints available"));
    }
}

void SpeedTestController::startUploadTest()
{
    if (m_pendingUploadUrls.isEmpty()) {
        setUploadSpeed(0.0);
        setStatusText(tr("Done (upload test failed)"));
        setIsRunning(false);
        m_phase = Phase::Idle;
        logSpeedTestWarning(QStringLiteral("Upload speed test aborted: no upload endpoints left to try"));
        emit speedTestFinished();
        return;
    }

    if (m_uploadPayload.size() != kUploadPayloadSizeBytes) {
        m_uploadPayload.resize(kUploadPayloadSizeBytes);
        for (int index = 0; index + static_cast<int>(sizeof(quint32)) <= m_uploadPayload.size(); index += static_cast<int>(sizeof(quint32))) {
            const quint32 value = QRandomGenerator::global()->generate();
            std::memcpy(m_uploadPayload.data() + index, &value, sizeof(value));
        }
    }

    const int uploadConcurrency = (std::max)(1, (std::min)(kParallelUploadRequests, static_cast<int>(s_uploadUrls.size())));

    m_phase = Phase::Upload;
    resetPhaseTransferState();
    setStatusText(tr("Testing upload..."));
    m_phaseLimitTimer.start(kUploadPhaseDurationMs);

    logSpeedTestInfo(QStringLiteral("Starting upload phase for %1 seconds with %2 parallel streams and %3 MiB payloads")
                     .arg(kUploadPhaseDurationMs / 1000)
                     .arg(uploadConcurrency)
                     .arg(kUploadPayloadSizeBytes / (1024 * 1024)));

    ensureUploadConcurrency(true);

    if (m_activeReplies.isEmpty()) {
        setUploadSpeed(0.0);
        setStatusText(tr("Done (upload test failed)"));
        setIsRunning(false);
        m_phase = Phase::Idle;
        logSpeedTestWarning(QStringLiteral("Upload speed test aborted: no upload endpoints left to try"));
        emit speedTestFinished();
    }
}

bool SpeedTestController::startDownloadRequest(bool allowRefill, const QString &excludedUrl)
{
    const QString downloadUrl = takeRandomUrl(m_pendingDownloadUrls, s_downloadUrls, allowRefill, excludedUrl);
    if (downloadUrl.isEmpty()) {
        return false;
    }

    QNetworkRequest request { QUrl(downloadUrl) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("User-Agent", "Mozilla/5.0");
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = m_nam->get(request);
    QTimer *probeTimer = new QTimer(this);
    probeTimer->setSingleShot(true);

    ActiveReplyState state;
    state.probeTimer = probeTimer;
    m_activeReplies.insert(reply, state);

    connect(probeTimer, &QTimer::timeout, this, [this, reply]() {
        auto it = m_activeReplies.find(reply);
        if (it == m_activeReplies.end() || !reply || m_phase != Phase::Download || m_phaseStopRequested) {
            return;
        }

        if (it->bytesTransferred >= kDownloadProbeMinBytes) {
            return;
        }

        it->switchRequested = true;
        reply->abort();
    });
    probeTimer->start(kRequestProbeTimeoutMs);

    logSpeedTestInfo(QStringLiteral("Running download request against %1")
                     .arg(downloadUrl));

    connect(reply, &QNetworkReply::downloadProgress,
            this, &SpeedTestController::onDownloadProgress);
    connect(reply, &QNetworkReply::finished,
            this, &SpeedTestController::onDownloadFinished);

    return true;
}

bool SpeedTestController::startUploadRequest(bool allowRefill, const QString &excludedUrl)
{
    const QString uploadUrl = takeRandomUrl(m_pendingUploadUrls, s_uploadUrls, allowRefill, excludedUrl);
    if (uploadUrl.isEmpty()) {
        return false;
    }

    QNetworkRequest request { QUrl(uploadUrl) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("User-Agent", "Mozilla/5.0");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = m_nam->post(request, m_uploadPayload);
    m_activeReplies.insert(reply, ActiveReplyState {});

    logSpeedTestInfo(QStringLiteral("Running upload request against %1")
                     .arg(uploadUrl));

    connect(reply, &QNetworkReply::uploadProgress,
            this, &SpeedTestController::onUploadProgress);
    connect(reply, &QNetworkReply::finished,
            this, &SpeedTestController::onUploadFinished);

    return true;
}

void SpeedTestController::ensureDownloadConcurrency(bool allowRefill, const QString &excludedUrl)
{
    QString excludedCandidate = excludedUrl;
    while (m_phase == Phase::Download
           && !m_phaseStopRequested
           && m_activeReplies.size() < kParallelDownloadRequests
           && startDownloadRequest(allowRefill, excludedCandidate)) {
        excludedCandidate.clear();
        allowRefill = true;
    }
}

void SpeedTestController::ensureUploadConcurrency(bool allowRefill, const QString &excludedUrl)
{
    const int uploadConcurrency = (std::max)(1, (std::min)(kParallelUploadRequests, static_cast<int>(s_uploadUrls.size())));
    QString excludedCandidate = excludedUrl;
    while (m_phase == Phase::Upload
           && !m_phaseStopRequested
           && m_activeReplies.size() < uploadConcurrency
           && startUploadRequest(allowRefill, excludedCandidate)) {
        excludedCandidate.clear();
        allowRefill = true;
    }
}

void SpeedTestController::onDownloadProgress(qint64 bytesReceived, qint64 /*bytesTotal*/)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    auto it = m_activeReplies.find(reply);
    if (it == m_activeReplies.end()) {
        return;
    }

    it->bytesTransferred = bytesReceived;

    if (it->probeTimer && it->probeTimer->isActive() && bytesReceived >= kDownloadProbeMinBytes) {
        it->probeTimer->stop();
    }

    const qint64 elapsed = m_timer.elapsed();
    if (elapsed - m_lastTimestamp < kProgressUpdateIntervalMs) {
        return;
    }

    const double mbps = bytesToMbps(m_phaseAccumulatedBytes + currentInFlightBytes(), elapsed);
    if (mbps > 0.0) {
        setDownloadSpeed(mbps);
    }

    m_lastTimestamp = elapsed;
}

void SpeedTestController::onUploadProgress(qint64 bytesSent, qint64 /*bytesTotal*/)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    auto it = m_activeReplies.find(reply);
    if (it == m_activeReplies.end()) {
        return;
    }

    it->bytesTransferred = bytesSent;

    const qint64 elapsed = m_timer.elapsed();
    if (elapsed - m_lastTimestamp < kProgressUpdateIntervalMs) {
        return;
    }

    const double mbps = bytesToMbps(m_phaseAccumulatedBytes + currentInFlightBytes(), elapsed);
    if (mbps > 0.0) {
        setUploadSpeed(mbps);
    }

    m_lastTimestamp = elapsed;
}

void SpeedTestController::onDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    auto it = m_activeReplies.find(reply);
    if (!reply || it == m_activeReplies.end()) {
        return;
    }

    const ActiveReplyState state = it.value();
    m_activeReplies.erase(it);

    const QString finishedUrl = reply->url().toString();
    const QString errorMsg = reply->errorString() + httpStatusSuffix(reply);
    const bool hasError = reply->error() != QNetworkReply::NoError;
    const bool timedStop = m_phaseStopRequested;
    const bool switchingMirror = state.switchRequested;
    const qint64 bufferedBytes = reply->bytesAvailable();
    const qint64 drainedBytes = reply->readAll().size();
    const qint64 requestBytes = (std::max)((std::max)(state.bytesTransferred, bufferedBytes), drainedBytes);

    stopAndDeleteProbeTimer(state.probeTimer);
    m_phaseAccumulatedBytes += requestBytes;

    reply->deleteLater();

    if (timedStop || m_timer.elapsed() >= kDownloadPhaseDurationMs) {
        if (m_activeReplies.isEmpty()) {
            finalizeDownloadPhase();
        }
        return;
    }

    if (switchingMirror) {
        logSpeedTestWarning(QStringLiteral("Download request against %1 was too slow (%2 bytes in %3 ms). Switching mirror.")
                            .arg(finishedUrl)
                            .arg(requestBytes)
                            .arg(kRequestProbeTimeoutMs));

        ensureDownloadConcurrency(true, finishedUrl);
        if (m_activeReplies.isEmpty()) {
            finishWithError(tr("Download test failed"), tr("All download endpoints were too slow"));
        }
        return;
    }

    if (hasError) {
        if (!m_phaseStopRequested) {
            setStatusText(tr("Retrying download..."));
            logSpeedTestWarning(QStringLiteral("Download request failed for %1: %2. Retrying with another endpoint.")
                                .arg(finishedUrl, errorMsg));
            ensureDownloadConcurrency(true, finishedUrl);

            if (!m_activeReplies.isEmpty()) {
                return;
            }
        }

        logSpeedTestWarning(QStringLiteral("Download phase failed for %1: %2")
                            .arg(finishedUrl, errorMsg));
        finishWithError(tr("Download test failed"), errorMsg);
        return;
    }

    logSpeedTestInfo(QStringLiteral("Download request finished against %1 with %2 bytes collected so far")
                     .arg(finishedUrl)
                     .arg(m_phaseAccumulatedBytes));

    ensureDownloadConcurrency(true);
}

void SpeedTestController::onUploadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    auto it = m_activeReplies.find(reply);
    if (!reply || it == m_activeReplies.end()) {
        return;
    }

    const ActiveReplyState state = it.value();
    m_activeReplies.erase(it);

    const QString finishedUrl = reply->url().toString();
    const QString errorMsg = reply->errorString() + httpStatusSuffix(reply);
    const bool hasError = reply->error() != QNetworkReply::NoError;
    const bool timedStop = m_phaseStopRequested;
    const qint64 requestBytes = hasError
                                ? state.bytesTransferred
                                : (std::max<qint64>)(state.bytesTransferred, static_cast<qint64>(m_uploadPayload.size()));

    stopAndDeleteProbeTimer(state.probeTimer);
    m_phaseAccumulatedBytes += requestBytes;

    reply->deleteLater();

    if (timedStop || m_timer.elapsed() >= kUploadPhaseDurationMs) {
        if (m_activeReplies.isEmpty()) {
            finalizeUploadPhase();
        }
        return;
    }

    if (hasError) {
        if (!m_phaseStopRequested) {
            setStatusText(tr("Retrying upload..."));
            logSpeedTestWarning(QStringLiteral("Upload request failed for %1: %2. Retrying with another endpoint.")
                                .arg(finishedUrl, errorMsg));
            ensureUploadConcurrency(true, finishedUrl);

            if (!m_activeReplies.isEmpty()) {
                return;
            }
        }

        setUploadSpeed(0.0);
        setStatusText(tr("Done (upload test failed)"));
        setIsRunning(false);
        m_phase = Phase::Idle;
        m_phaseStopRequested = false;
        logSpeedTestWarning(QStringLiteral("Upload phase failed for %1: %2")
                            .arg(finishedUrl, errorMsg));
        emit speedTestFinished();
        return;
    }

    logSpeedTestInfo(QStringLiteral("Upload request finished against %1 with %2 bytes sent so far")
                     .arg(finishedUrl)
                     .arg(m_phaseAccumulatedBytes));

    ensureUploadConcurrency(true);
}

void SpeedTestController::onPhaseTimeout()
{
    if (!m_isRunning) {
        return;
    }

    const Phase phaseBeingStopped = m_phase;

    if (m_activeReplies.isEmpty()) {
        if (phaseBeingStopped == Phase::Download) {
            finalizeDownloadPhase();
        } else if (phaseBeingStopped == Phase::Upload) {
            finalizeUploadPhase();
        }
        return;
    }

    m_phaseStopRequested = true;

    // Snapshot reply pointers before aborting.  QNetworkReply::abort() can
    // synchronously emit finished() -> onDownload/UploadFinished() -> erase()
    // from m_activeReplies, which invalidates any live QHash iterator.
    const QList<QNetworkReply *> pendingReplies = m_activeReplies.keys();

    for (auto it = m_activeReplies.begin(); it != m_activeReplies.end(); ++it) {
        if (it->probeTimer) {
            it->probeTimer->stop();
        }
    }

    for (QNetworkReply *reply : pendingReplies) {
        if (reply) {
            reply->abort();
        }
    }

    logSpeedTestInfo(QStringLiteral("Stopping %1 phase after reaching target duration")
                     .arg(phaseBeingStopped == Phase::Download ? QStringLiteral("download") : QStringLiteral("upload")));
}

void SpeedTestController::finalizeDownloadPhase()
{
    m_phaseLimitTimer.stop();
    m_phaseStopRequested = false;

    const double mbps = currentPhaseAverageMbps();
    if (mbps <= 0.0) {
        logSpeedTestWarning(QStringLiteral("Download phase finished without measurable data"));
        finishWithError(tr("Download test failed"), tr("Download test failed"));
        return;
    }

    setDownloadSpeed(mbps);
    logSpeedTestInfo(QStringLiteral("Download phase finished: %1 Mbps average over %2 ms and %3 bytes")
                     .arg(QString::number(mbps, 'f', 1))
                     .arg(m_timer.elapsed())
                     .arg(m_phaseAccumulatedBytes));

    startUploadTest();
}

void SpeedTestController::finalizeUploadPhase()
{
    m_phaseLimitTimer.stop();
    m_phaseStopRequested = false;

    const double mbps = currentPhaseAverageMbps();
    if (mbps > 0.0) {
        setUploadSpeed(mbps);
        setStatusText(tr("Done"));
        logSpeedTestInfo(QStringLiteral("Upload phase finished: %1 Mbps average over %2 ms and %3 bytes")
                         .arg(QString::number(mbps, 'f', 1))
                         .arg(m_timer.elapsed())
                         .arg(m_phaseAccumulatedBytes));
    } else {
        setUploadSpeed(0.0);
        setStatusText(tr("Done (upload test failed)"));
        logSpeedTestWarning(QStringLiteral("Upload phase finished without measurable data"));
    }

    setIsRunning(false);
    m_phase = Phase::Idle;
    emit speedTestFinished();
}

void SpeedTestController::finishWithError(const QString &status, const QString &error)
{
    m_phaseLimitTimer.stop();
    m_phase = Phase::Idle;
    m_phaseStopRequested = false;

    clearActiveReplies();

    setStatusText(status);
    setIsRunning(false);

    logSpeedTestWarning(QStringLiteral("Speed test failed: %1")
                        .arg(error));
    emit speedTestFailed(error);
}

void SpeedTestController::resetPhaseTransferState()
{
    m_timer.start();
    m_lastTimestamp = 0;
    m_phaseAccumulatedBytes = 0;
    m_phaseStopRequested = false;
    clearActiveReplies();
}

double SpeedTestController::currentPhaseAverageMbps() const
{
    return bytesToMbps(m_phaseAccumulatedBytes + currentInFlightBytes(), m_timer.elapsed());
}

qint64 SpeedTestController::currentInFlightBytes() const
{
    qint64 totalBytes = 0;
    for (auto it = m_activeReplies.cbegin(); it != m_activeReplies.cend(); ++it) {
        totalBytes += it->bytesTransferred;
    }

    return totalBytes;
}

QString SpeedTestController::currentServerHost() const
{
    if (!m_serversModel) {
        return QString();
    }

    const QJsonObject serverConfig = defaultServerConfig(m_serversModel);
    Proto defaultProtocol = Proto::Any;
    const QJsonObject protocolConfig = defaultProtocolConfig(serverConfig, &defaultProtocol);

    if (defaultProtocol == Proto::Xray || defaultProtocol == Proto::SSXray) {
        const QString configuredHost = configuredXrayEndpointHost(protocolConfig);
        if (!configuredHost.isEmpty()) {
            return configuredHost;
        }
    }

    const int defaultServerIndex = m_serversModel->getDefaultServerIndex();
    if (defaultServerIndex < 0 || defaultServerIndex >= m_serversModel->getServersCount()) {
        return QString();
    }

    const ServerCredentials credentials = m_serversModel->getServerCredentials(defaultServerIndex);
    if (!credentials.hostName.isEmpty()) {
        return credentials.hostName;
    }

    return m_serversModel->getDefaultServerData(QStringLiteral("hostName")).toString();
}

quint16 SpeedTestController::currentServerPort() const
{
    if (!m_serversModel) {
        return kFallbackServerPort;
    }

    const QJsonObject serverConfig = defaultServerConfig(m_serversModel);
    Proto defaultProtocol = Proto::Any;
    const QJsonObject protocolConfig = defaultProtocolConfig(serverConfig, &defaultProtocol);

    if (defaultProtocol == Proto::Xray || defaultProtocol == Proto::SSXray) {
        const quint16 configuredPort = configuredXrayEndpointPort(protocolConfig);
        if (configuredPort > 0) {
            return configuredPort;
        }
    }

    const quint16 protocolPort = portFromJsonValue(protocolConfig.value(config_key::port));
    if (protocolPort > 0) {
        return protocolPort;
    }

    const int defaultServerIndex = m_serversModel->getDefaultServerIndex();
    if (defaultServerIndex < 0 || defaultServerIndex >= m_serversModel->getServersCount()) {
        return kFallbackServerPort;
    }

    const ServerCredentials credentials = m_serversModel->getServerCredentials(defaultServerIndex);
    if (credentials.port > 0) {
        return static_cast<quint16>(credentials.port);
    }

    return kFallbackServerPort;
}

void SpeedTestController::stopAndDeleteProbeTimer(QTimer *probeTimer)
{
    if (!probeTimer) {
        return;
    }

    probeTimer->stop();
    probeTimer->deleteLater();
}

void SpeedTestController::clearActiveReplies()
{
    for (auto it = m_activeReplies.begin(); it != m_activeReplies.end(); ++it) {
        stopAndDeleteProbeTimer(it->probeTimer);

        QNetworkReply *reply = it.key();
        if (!reply) {
            continue;
        }

        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }

    m_activeReplies.clear();
}

void SpeedTestController::setIsRunning(bool running)
{
    if (m_isRunning != running) {
        m_isRunning = running;
        emit isRunningChanged();
    }
}

void SpeedTestController::setDownloadSpeed(double mbps)
{
    m_downloadSpeed = mbps;
    emit downloadSpeedChanged();
}

void SpeedTestController::setUploadSpeed(double mbps)
{
    m_uploadSpeed = mbps;
    emit uploadSpeedChanged();
}

void SpeedTestController::setStatusText(const QString &text)
{
    if (m_statusText != text) {
        m_statusText = text;
        emit statusTextChanged();
    }
}

void SpeedTestController::setServerPingText(const QString &text)
{
    if (m_serverPingText != text) {
        m_serverPingText = text;
        emit serverPingTextChanged();
    }
}

void SpeedTestController::setMoscowPingText(const QString &text)
{
    if (m_moscowPingText != text) {
        m_moscowPingText = text;
        emit moscowPingTextChanged();
    }
}

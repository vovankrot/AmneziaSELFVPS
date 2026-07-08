#include "serverMonitorController.h"

#include <QtConcurrent>

#include "core/controllers/serverController.h"

ServerMonitorController::ServerMonitorController(const QSharedPointer<ServersModel> &serversModel,
                                                 const std::shared_ptr<Settings> &settings,
                                                 QObject *parent)
    : QObject(parent)
    , m_serversModel(serversModel)
    , m_settings(settings)
{
    connect(&m_timer, &QTimer::timeout, this, &ServerMonitorController::pollMetrics);
}

ServerMonitorController::~ServerMonitorController()
{
    stopMonitoring();
}

bool ServerMonitorController::isPolling() const { return m_isPolling; }
QVariantList ServerMonitorController::cpuHistory() const
{
    QVariantList out;
    for (double v : m_cpuHistory) out.append(v);
    return out;
}
QVariantList ServerMonitorController::ramHistory() const
{
    QVariantList out;
    for (double v : m_ramHistory) out.append(v);
    return out;
}
QVariantList ServerMonitorController::netRxHistory() const
{
    QVariantList out;
    for (double v : m_netRxHistory) out.append(v);
    return out;
}
QVariantList ServerMonitorController::netTxHistory() const
{
    QVariantList out;
    for (double v : m_netTxHistory) out.append(v);
    return out;
}
double ServerMonitorController::cpuPercent() const { return m_cpuPercent; }
double ServerMonitorController::ramUsedMb() const { return m_ramUsedMb; }
double ServerMonitorController::ramTotalMb() const { return m_ramTotalMb; }
double ServerMonitorController::netRxKBs() const { return m_netRxKBs; }
double ServerMonitorController::netTxKBs() const { return m_netTxKBs; }
int ServerMonitorController::diskPercent() const { return m_diskPercent; }
QString ServerMonitorController::uptime() const { return m_uptime; }
int ServerMonitorController::dockerContainers() const { return m_dockerContainers; }
QString ServerMonitorController::errorString() const { return m_errorString; }

void ServerMonitorController::startMonitoring()
{
    if (m_isPolling)
        return;

    m_cpuHistory.clear();
    m_ramHistory.clear();
    m_netRxHistory.clear();
    m_netTxHistory.clear();
    m_prevRxBytes = -1;
    m_prevTxBytes = -1;
    m_prevCpuTotal = -1;
    m_prevCpuIdle = -1;
    m_errorString.clear();

    m_isPolling = true;
    emit isPollingChanged();

    pollMetrics();
    m_timer.start(kPollIntervalMs);
}

void ServerMonitorController::stopMonitoring()
{
    if (!m_isPolling)
        return;

    m_timer.stop();
    m_isPolling = false;
    emit isPollingChanged();
}

void ServerMonitorController::pollMetrics()
{
    // Skip if previous poll is still running
    if (m_pollInFlight)
        return;
    m_pollInFlight = true;

    // Run SSH command in background thread to avoid blocking UI
    auto serversModel = m_serversModel;
    auto settings = m_settings;

    QtConcurrent::run([this, serversModel, settings]() {
        const auto credentials = serversModel->getProcessedServerCredentials();
        if (credentials.hostName.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                m_errorString = tr("No server selected");
                emit errorStringChanged();
            });
            return;
        }

        ServerController serverController(settings);

        // Single compound command — minimal SSH overhead
        // Line 1: /proc/stat cpu line
        // Line 2+: /proc/meminfo
        // Then: /proc/net/dev
        // Then: /proc/uptime
        // Then: disk usage %
        // Then: docker container count
        const QString cmd =
            "echo '---CPUSTAT---'; head -1 /proc/stat; "
            "echo '---MEMINFO---'; grep -E '^(MemTotal|MemAvailable):' /proc/meminfo; "
            "echo '---NETDEV---'; cat /proc/net/dev; "
            "echo '---UPTIME---'; cat /proc/uptime; "
            "echo '---DISK---'; df / --output=pcent 2>/dev/null | tail -1; "
            "echo '---DOCKER---'; docker ps -q 2>/dev/null | wc -l";

        QString fullOutput;
        auto cbRead = [&fullOutput](const QString &line, libssh::Client &) -> ErrorCode {
            fullOutput += line + "\n";
            return ErrorCode::NoError;
        };

        ErrorCode err = serverController.runScript(credentials, cmd, cbRead, cbRead);

        QMetaObject::invokeMethod(this, [this, err, fullOutput]() {
            m_pollInFlight = false;
            if (err != ErrorCode::NoError) {
                m_errorString = tr("SSH connection failed");
                emit errorStringChanged();
                return;
            }
            m_errorString.clear();
            emit errorStringChanged();
            parseOutput(fullOutput);
        });
    });
}

void ServerMonitorController::parseOutput(const QString &output)
{
    const QStringList lines = output.split('\n');

    enum Section { None, CpuStat, MemInfo, NetDev, Uptime, Disk, Docker };
    Section section = None;

    qint64 cpuTotal = 0, cpuIdle = 0;
    double memTotalKb = 0, memAvailKb = 0;
    qint64 rxBytes = 0, txBytes = 0;

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;

        if (trimmed == "---CPUSTAT---") { section = CpuStat; continue; }
        if (trimmed == "---MEMINFO---") { section = MemInfo; continue; }
        if (trimmed == "---NETDEV---") { section = NetDev; continue; }
        if (trimmed == "---UPTIME---") { section = Uptime; continue; }
        if (trimmed == "---DISK---") { section = Disk; continue; }
        if (trimmed == "---DOCKER---") { section = Docker; continue; }

        switch (section) {
        case CpuStat: {
            // cpu  user nice system idle iowait irq softirq steal
            if (trimmed.startsWith("cpu ")) {
                QStringList parts = trimmed.split(QRegularExpression("\\s+"));
                if (parts.size() >= 5) {
                    for (int i = 1; i < parts.size(); ++i)
                        cpuTotal += parts[i].toLongLong();
                    cpuIdle = parts[4].toLongLong(); // idle field
                }
            }
            break;
        }
        case MemInfo: {
            if (trimmed.startsWith("MemTotal:")) {
                memTotalKb = trimmed.split(QRegularExpression("\\s+")).value(1).toDouble();
            } else if (trimmed.startsWith("MemAvailable:")) {
                memAvailKb = trimmed.split(QRegularExpression("\\s+")).value(1).toDouble();
            }
            break;
        }
        case NetDev: {
            // Skip header lines
            if (trimmed.contains('|')) break;
            // Interface: rx_bytes ... tx_bytes ...
            // Skip lo
            if (trimmed.startsWith("lo:")) break;
            QStringList parts = trimmed.split(QRegularExpression("[:\\s]+"));
            if (parts.size() >= 10) {
                rxBytes += parts[1].toLongLong();
                txBytes += parts[9].toLongLong();
            }
            break;
        }
        case Uptime: {
            QStringList parts = trimmed.split(' ');
            if (!parts.isEmpty()) {
                double secs = parts[0].toDouble();
                int days = static_cast<int>(secs / 86400);
                int hours = static_cast<int>((secs - days * 86400) / 3600);
                int mins = static_cast<int>((secs - days * 86400 - hours * 3600) / 60);
                if (days > 0)
                    m_uptime = QString("%1d %2h %3m").arg(days).arg(hours).arg(mins);
                else
                    m_uptime = QString("%1h %2m").arg(hours).arg(mins);
            }
            break;
        }
        case Disk: {
            QString pct = trimmed;
            pct.remove('%');
            m_diskPercent = pct.trimmed().toInt();
            break;
        }
        case Docker: {
            m_dockerContainers = trimmed.toInt();
            break;
        }
        default:
            break;
        }
    }

    // CPU: compute delta since last poll
    if (m_prevCpuTotal >= 0 && cpuTotal > m_prevCpuTotal) {
        qint64 totalDelta = cpuTotal - m_prevCpuTotal;
        qint64 idleDelta = cpuIdle - m_prevCpuIdle;
        m_cpuPercent = 100.0 * (1.0 - static_cast<double>(idleDelta) / totalDelta);
    } else if (cpuTotal > 0) {
        // First poll — rough estimate
        m_cpuPercent = 100.0 * (1.0 - static_cast<double>(cpuIdle) / cpuTotal);
    }
    m_prevCpuTotal = cpuTotal;
    m_prevCpuIdle = cpuIdle;

    // RAM
    m_ramTotalMb = memTotalKb / 1024.0;
    m_ramUsedMb = (memTotalKb - memAvailKb) / 1024.0;

    // Network: compute delta KB/s since last poll
    if (m_prevRxBytes >= 0 && rxBytes >= m_prevRxBytes) {
        double intervalSec = kPollIntervalMs / 1000.0;
        m_netRxKBs = (rxBytes - m_prevRxBytes) / 1024.0 / intervalSec;
        m_netTxKBs = (txBytes - m_prevTxBytes) / 1024.0 / intervalSec;
    } else {
        m_netRxKBs = 0;
        m_netTxKBs = 0;
    }
    m_prevRxBytes = rxBytes;
    m_prevTxBytes = txBytes;

    // Update history lists (keep max kMaxHistory points)
    auto appendHistory = [](QList<double> &list, double value, int max) {
        list.append(value);
        while (list.size() > max)
            list.removeFirst();
    };

    appendHistory(m_cpuHistory, m_cpuPercent, kMaxHistory);
    appendHistory(m_ramHistory, m_ramUsedMb, kMaxHistory);
    appendHistory(m_netRxHistory, m_netRxKBs, kMaxHistory);
    appendHistory(m_netTxHistory, m_netTxKBs, kMaxHistory);

    emit metricsUpdated();
}

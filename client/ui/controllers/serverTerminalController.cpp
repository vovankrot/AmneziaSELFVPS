#include "serverTerminalController.h"

#include <QtConcurrent>
#include <QRegularExpression>

#include "core/controllers/serverController.h"
#include "logger.h"

namespace {
Logger logger("ServerTerminalController");

QString shellSingleQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QChar('\''), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

// Patterns that could brick the server if typed accidentally
const QStringList dangerousPatterns = {
    "rm\\s+(-[a-zA-Z]*f[a-zA-Z]*\\s+)?/",    // rm -rf /
    "mkfs\\.",                                   // mkfs.ext4 etc
    "dd\\s+.*of\\s*=\\s*/dev/",                 // dd to block device
    ":\\(\\)\\{\\s*:\\|:\\s*&\\s*\\};:",         // fork bomb
    ">(\\s)*/dev/sd",                            // overwrite disk
    "chmod\\s+(-[a-zA-Z]*\\s+)?[0-7]*\\s+/\\s*$", // chmod / recursively
    "chown\\s+.*\\s+/\\s*$",                    // chown / recursively
    "shutdown|reboot|halt|poweroff|init\\s+0",   // shutdown commands
};
}

ServerTerminalController::ServerTerminalController(const QSharedPointer<ServersModel> &serversModel,
                                                     const std::shared_ptr<Settings> &settings,
                                                     QObject *parent)
    : QObject(parent), m_serversModel(serversModel), m_settings(settings),
      m_alive(std::make_shared<std::atomic<bool>>(true))
{
}

ServerTerminalController::~ServerTerminalController()
{
    m_alive->store(false);
}

QString ServerTerminalController::outputText() const
{
    return m_output;
}

bool ServerTerminalController::isBusy() const
{
    return m_isBusy;
}

void ServerTerminalController::appendOutput(const QString &text)
{
    m_output += text;
    // Trim if output exceeds limit — keep the tail
    if (m_output.size() > MaxOutputSize) {
        int trimTo = m_output.size() - MaxOutputSize * 3 / 4;
        int newlinePos = m_output.indexOf('\n', trimTo);
        if (newlinePos > 0) {
            m_output = "... [output truncated] ...\n" + m_output.mid(newlinePos + 1);
        } else {
            m_output = "... [output truncated] ...\n" + m_output.mid(trimTo);
        }
    }
    emit outputChanged();
}

bool ServerTerminalController::isDangerousCommand(const QString &cmd) const
{
    for (const QString &pattern : dangerousPatterns) {
        QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
        if (re.match(cmd).hasMatch()) {
            return true;
        }
    }
    return false;
}

void ServerTerminalController::clearOutput()
{
    m_output.clear();
    emit outputChanged();
}

void ServerTerminalController::executeCommand(const QString &command)
{
    QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) return;

    // Block dangerous commands
    if (isDangerousCommand(trimmed)) {
        appendOutput("\n$ " + trimmed + "\n");
        appendOutput(tr("BLOCKED: this command is potentially destructive.") + "\n");
        appendOutput(tr("If you really need it, connect via SSH directly.") + "\n");
        emit commandFinished(false);
        return;
    }

    appendOutput("\n$ " + trimmed + "\n");
    runRemoteCommand(trimmed);
}

void ServerTerminalController::fetchXrayLogs()
{
    appendOutput(QString("\n--- %1 ---\n").arg(tr("XRay Logs")));
    // Try amnezia-xray first, then amnezia-ssxray; use { cmd; } grouping to avoid
    // journalctl returning 0 with "-- No entries --" short-circuiting the chain.
    runRemoteCommand(QString(
        "for c in amnezia-xray amnezia-ssxray; do "
        "  if docker inspect \"$c\" >/dev/null 2>&1; then "
        "    echo \"=== Container: $c ===\"; "
        "    docker logs \"$c\" --tail 100 2>&1 || true; "
        "    docker exec \"$c\" cat /opt/amnezia/xray/error.log 2>/dev/null || "
        "    docker exec \"$c\" cat /opt/amnezia/ssxray/error.log 2>/dev/null || true; "
        "    exit 0; "
        "  fi; "
        "done; "
        "echo %1")
                     .arg(shellSingleQuote(tr("No XRay/SSXray container found on server"))));
}

void ServerTerminalController::fetchDockerLogs()
{
    appendOutput(QString("\n--- %1 ---\n").arg(tr("Docker Containers")));
    runRemoteCommand(QString("docker ps -a --format 'table {{.Names}}\\t{{.Status}}\\t{{.Ports}}' 2>&1; "
                             "echo ''; echo '--- Recent Docker Events ---'; "
                             "docker events --since 1h --until 0s --format '{{.Time}} {{.Action}} {{.Actor.Attributes.name}}' 2>&1 | head -30 || "
                             "echo %1")
                     .arg(shellSingleQuote(tr("Docker not available"))));
}

void ServerTerminalController::fetchSystemErrors()
{
    appendOutput(QString("\n--- %1 ---\n").arg(tr("System Errors (last 24h)")));
    runRemoteCommand(QString("journalctl --priority=err --since '24 hours ago' --no-pager -n 50 2>/dev/null || "
                             "dmesg --level=err,crit,alert,emerg 2>/dev/null | tail -50 || "
                             "echo %1; "
                             "echo ''; echo '--- Disk Usage ---'; df -h / 2>/dev/null; "
                             "echo ''; echo '--- Memory ---'; free -h 2>/dev/null")
                     .arg(shellSingleQuote(tr("No system error logs available"))));
}

void ServerTerminalController::fetchSshAuthLog()
{
    appendOutput(QString("\n--- %1 ---\n").arg(tr("SSH Auth Log (last 50)")));
    runRemoteCommand(QString("journalctl -u sshd --no-pager -n 50 2>/dev/null || "
                             "tail -50 /var/log/auth.log 2>/dev/null || "
                             "tail -50 /var/log/secure 2>/dev/null || "
                             "echo %1")
                     .arg(shellSingleQuote(tr("No SSH auth logs found"))));
}

void ServerTerminalController::runRemoteCommand(const QString &cmd)
{
    if (m_isBusy) return;

    m_isBusy = true;
    emit isBusyChanged();

    int serverIndex = m_serversModel->getDefaultServerIndex();
    if (serverIndex < 0) {
        appendOutput(tr("Error: no server selected") + "\n");
        m_isBusy = false;
        emit isBusyChanged();
        emit commandFinished(false);
        return;
    }

    ServerCredentials credentials = m_serversModel->getServerCredentials(serverIndex);

    // Capture shared_ptr to alive flag — safe even if `this` is destroyed
    std::weak_ptr<std::atomic<bool>> weakAlive = m_alive;

    QtConcurrent::run([this, weakAlive, credentials, cmd]() {
        QSharedPointer<ServerController> serverController(new ServerController(m_settings));
        QObject::connect(serverController.get(), &ServerController::logLineReady, this, [this, weakAlive](const QString &line) {
            auto alive = weakAlive.lock();
            if (!alive || !alive->load()) {
                return;
            }

            QString text = line;
            if (!text.endsWith('\n')) {
                text += '\n';
            }
            appendOutput(text);
        });

        QString stdOut;
        auto cbRead = [&](const QString &data, libssh::Client &) {
            stdOut += data;
            return ErrorCode::NoError;
        };
        auto cbErr = [&](const QString &data, libssh::Client &) {
            stdOut += data;
            return ErrorCode::NoError;
        };

        ErrorCode e = serverController->runScript(credentials, cmd, cbRead, cbErr);

        bool success = (e == ErrorCode::NoError);
        QString resultText = stdOut;
        if (!success && resultText.isEmpty()) {
            resultText = tr("Error: SSH command failed") + "\n";
        }

        // Check if the controller is still alive before touching it
        auto alive = weakAlive.lock();
        if (!alive || !alive->load()) {
            return; // Controller destroyed — silently discard results
        }

        QMetaObject::invokeMethod(this, [this, weakAlive, resultText, success]() {
            auto alive = weakAlive.lock();
            if (!alive || !alive->load()) return;

            appendOutput(resultText);
            if (!m_output.endsWith('\n')) appendOutput("\n");
            m_isBusy = false;
            emit isBusyChanged();
            emit commandFinished(success);
        }, Qt::QueuedConnection);
    });
}

#include "logger.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QStandardPaths>
#include <QUrl>

#include <cstdlib>

#ifdef Q_OS_WIN
#include <windows.h>
#include <process.h>
#endif

#include "utilities.h"
#include "version.h"

#ifdef AMNEZIA_DESKTOP
    #include <core/ipcclient.h>
#endif

#ifdef Q_OS_IOS
    #include <AmneziaVPN-Swift.h>
#endif

QFile Logger::m_file;
QTextStream Logger::m_textStream;
QString Logger::m_logFileName = QString("%1.log").arg(APPLICATION_NAME);
QString Logger::m_serviceLogFileName = QString("%1.log").arg(SERVICE_NAME);
QString Logger::m_userLogFilePath;
QString Logger::m_serviceLogFilePath;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (msg.simplified().isEmpty()) {
        return;
    }

    // Skip annoying messages from Qt
    if (msg.contains("OpenType support missing for")) {
        return;
    }

    if (msg.startsWith("Unknown property") || msg.startsWith("Could not create pixmap")
        || msg.startsWith("Populating font") || msg.startsWith("stale focus object")) {
        return;
    }

    switch (type) {
    case QtDebugMsg: Logger::Instance().debug() << msg; break;
    case QtInfoMsg: Logger::Instance().info() << msg; break;
    case QtWarningMsg: Logger::Instance().warning() << msg; break;
    case QtCriticalMsg: Logger::Instance().error() << msg; break;
    case QtFatalMsg: {
        Logger::Instance().error() << "FATAL: " << msg;
        Logger::flush();
#ifdef Q_OS_WIN
        // Write crash info directly to ProgramData bypassing Logger in case it's broken
        Logger::writeEmergencyCrashFile("QtFatalMsg", msg);
#endif
        // Do NOT call abort() — it bypasses SEH and produces no crash dump.
        // Terminate immediately but cleanly.
#ifdef Q_OS_WIN
        _exit(3);
#else
        std::_Exit(3);
#endif
    }
    }
}

#ifdef Q_OS_WIN
void Logger::writeEmergencyCrashFile(const QString &reason, const QString &details)
{
    wchar_t dir[MAX_PATH] = {};
    if (ExpandEnvironmentStringsW(L"%ProgramData%\\AmneziaVPN\\log", dir, MAX_PATH) == 0)
        wcscpy_s(dir, L"C:\\ProgramData\\AmneziaVPN\\log");
    CreateDirectoryW(dir, nullptr);

    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\service-crash.txt", dir);
    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        QByteArray data = QString("[%1] %2: %3\r\nPID=%4 TID=%5\r\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                 reason, details,
                 QString::number(GetCurrentProcessId()),
                 QString::number(GetCurrentThreadId()))
            .toUtf8();
        DWORD written;
        WriteFile(hFile, data.constData(), (DWORD)data.size(), &written, nullptr);
        CloseHandle(hFile);
    }
}
#endif

Logger &Logger::Instance()
{
    static Logger s;
    return s;
}

bool Logger::init(bool isServiceLogger)
{
    const QString logFileName = isServiceLogger ? m_serviceLogFileName : m_logFileName;
    QStringList candidateDirs;
    candidateDirs << (isServiceLogger ? systemLogDir() : userLogsDir())
                  << fallbackLogsDir(isServiceLogger);
    candidateDirs.removeAll(QString());
    candidateDirs.removeDuplicates();

    QString filePath;
    for (const QString &path : candidateDirs) {
        QDir appDir(path);
        if (!appDir.mkpath(QStringLiteral("."))) {
            continue;
        }

        const QString candidateFilePath = appDir.filePath(logFileName);

        // Rotate log if it exceeds 10 MB
        constexpr qint64 maxLogSize = 10 * 1024 * 1024;
        QFileInfo fi(candidateFilePath);
        if (fi.exists() && fi.size() > maxLogSize) {
            QString prevPath = candidateFilePath + ".prev";
            QFile::remove(prevPath);
            QFile::rename(candidateFilePath, prevPath);
        }

        m_file.setFileName(candidateFilePath);
        if (m_file.open(QIODevice::Append)) {
            filePath = candidateFilePath;
            break;
        }
    }

    if (filePath.isEmpty()) {
        qWarning() << "Cannot open log file:" << logFileName;
        return false;
    }

    if (isServiceLogger) {
        m_serviceLogFilePath = filePath;
    } else {
        m_userLogFilePath = filePath;
    }

    m_file.setTextModeEnabled(true);
    m_textStream.setDevice(&m_file);

    qInstallMessageHandler(messageHandler);
    qInfo() << "Logger initialized at" << filePath;

    return true;
}

void Logger::deInit()
{
    m_textStream.setDevice(nullptr);
    m_file.close();
}

void Logger::flush()
{
    m_textStream.flush();
    m_file.flush();
}

bool Logger::setServiceLogsEnabled(bool enabled)
{
#ifdef AMNEZIA_DESKTOP
    return IpcClient::withInterface([enabled](QSharedPointer<IpcInterfaceReplica> iface) {
        iface->setLogsEnabled(enabled);
        qDebug() << "Logger::setServiceLogsEnabled(): Logs transitioned to be " << (enabled ? "enabled" : "disabled");
        return true;
    },[](){
        qWarning() << "Logger::setServiceLogsEnabled(): Service is not running";
        return false;
    });
#endif

    return true;
}

QString Logger::userLogsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/log";
}

QString Logger::fallbackLogsDir(bool isServiceLogger)
{
    return QString("%1/%2/%3")
            .arg(QDir::tempPath(), APPLICATION_NAME, isServiceLogger ? QStringLiteral("service-log") : QStringLiteral("log"));
}

QString Logger::systemLogDir()
{
#ifdef Q_OS_WIN
    QStringList locationList = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    QString primaryLocation = "ProgramData";
    foreach (const QString &location, locationList) {
        if (location.contains(primaryLocation)) {
            return QString("%1/%2/log").arg(location).arg(APPLICATION_NAME);
        }
    }
    return QString();
#else
    return QString("/var/log/%1").arg(APPLICATION_NAME);
#endif
}

QString Logger::userLogsFilePath()
{
    return !m_userLogFilePath.isEmpty() ? m_userLogFilePath : userLogsDir() + QDir::separator() + m_logFileName;
}

QString Logger::serviceLogsFilePath()
{
    return !m_serviceLogFilePath.isEmpty()
            ? m_serviceLogFilePath
            : systemLogDir() + QDir::separator() + m_serviceLogFileName;
}

QString Logger::getLogFile()
{
    m_file.flush();
    QFile file(userLogsFilePath());

    file.open(QIODevice::ReadOnly);
    QString qtLog = file.readAll();

#ifdef Q_OS_IOS
    return QString().fromStdString(AmneziaVPN::swiftUpdateLogData(qtLog.toStdString()));
#else
    return qtLog;
#endif
}

QString Logger::getServiceLogFile()
{
    m_file.flush();
    QFile file(serviceLogsFilePath());

    file.open(QIODevice::ReadOnly);
    QString qtLog = file.readAll();

#ifdef Q_OS_IOS
    return QString().fromStdString(AmneziaVPN::swiftUpdateLogData(qtLog.toStdString()));
#else
    return qtLog;
#endif
}

bool Logger::openLogsFolder(bool isServiceLogger)
{
    const QString logFilePath = isServiceLogger ? serviceLogsFilePath() : userLogsFilePath();
    const QString path = QFileInfo(logFilePath).absolutePath();
    if (path.isEmpty()) {
        qWarning() << "Can't determine log folder for" << logFilePath;
        return false;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        qWarning() << "Can't open url:" << path;
        return false;
    }
    return true;
}

void Logger::clearLogs(bool isServiceLogger)
{
    bool isLogActive = m_file.isOpen();
    m_file.close();

    const QString logFilePath = isServiceLogger ? serviceLogsFilePath() : userLogsFilePath();
    QFile file(logFilePath);

    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.resize(0);
    file.close();

#ifdef Q_OS_IOS
    AmneziaVPN::swiftDeleteLog();
#endif

    if (isLogActive) {
        init(isServiceLogger);
    }
}

void Logger::clearServiceLogs()
{
#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([](QSharedPointer<IpcInterfaceReplica> iface) {
        iface->clearLogs();
        qDebug() << "Logger::clearServiceLogs(): Logs cleared";
    }, []() {
        qWarning() << "Logger::clearServiceLogs(): Service is not running";
    });
#endif
}

void Logger::cleanUp()
{
    clearLogs(false);
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dir.removeRecursively();

    clearLogs(true);
}

Logger::LogStreamer::LogStreamer(Logger *logger, LogLevel logLevel)
    : m_logger(logger), m_logLevel(logLevel), m_data(new Data())
{
}

Logger::LogStreamer::~LogStreamer()
{
    QString logLevelString;
    switch (m_logLevel) {
    case LogLevel::Trace: logLevelString = "[TRACE]"; break;
    case LogLevel::Debug: logLevelString = "[DEBUG]"; break;
    case LogLevel::Info: logLevelString = "[INFO]"; break;
    case LogLevel::Warning: logLevelString = "[WARNING]"; break;
    case LogLevel::Error: logLevelString = "[ERROR]"; break;
    }

    const QString message = QString("%1 %2 Amnezia %3 : %4")
                                    .arg(QDateTime::currentDateTimeUtc().toString("[yyyy-MM-dd hh:mm:ss.zzzZ]"),
                                         logLevelString, m_logger->className(), m_data->m_buffer.trimmed());

    if (m_file.isOpen()) {
        QTextStream logToFile(&m_file);
        logToFile << message << Qt::endl << Qt::flush;
    }

    QTextStream logToOutput((m_logLevel == LogLevel::Error) ? stderr : stdout);
    logToOutput << message << Qt::endl << Qt::flush;

    delete m_data;
}

Logger::LogStreamer Logger::error()
{
    return { this, LogLevel::Error };
}

Logger::LogStreamer Logger::warning()
{
    return { this, LogLevel::Warning };
}

Logger::LogStreamer Logger::info()
{
    return { this, LogLevel::Info };
}

Logger::LogStreamer Logger::debug()
{
    return { this, LogLevel::Debug };
}

QString Logger::sensitive(const QString &input)
{
#ifdef Q_DEBUG
    return input;
#else
    Q_UNUSED(input);
    return { 8, 'X' };
#endif
}

#define CREATE_LOGSTREAMER_OP_REF(x)                                                                                   \
    Logger::LogStreamer &Logger::LogStreamer::operator<<(x t)                                                          \
    {                                                                                                                  \
        m_data->m_ts << t << ' ';                                                                                      \
        return *this;                                                                                                  \
    }

CREATE_LOGSTREAMER_OP_REF(uint64_t);
CREATE_LOGSTREAMER_OP_REF(const char *);
CREATE_LOGSTREAMER_OP_REF(const QString &);
CREATE_LOGSTREAMER_OP_REF(const QByteArray &);
CREATE_LOGSTREAMER_OP_REF(const void *);

#undef CREATE_LOGSTREAMER_OP_REF

Logger::LogStreamer &Logger::LogStreamer::operator<<(const QStringList &t)
{
    m_data->m_ts << '[' << t.join(",") << ']' << ' ';
    return *this;
}

Logger::LogStreamer &Logger::LogStreamer::operator<<(const QJsonObject &t)
{
    m_data->m_ts << QJsonDocument(t).toJson(QJsonDocument::Indented) << ' ';
    return *this;
}

Logger::LogStreamer &Logger::LogStreamer::operator<<(QTextStreamFunction t)
{
    m_data->m_ts << t;
    return *this;
}

void Logger::LogStreamer::addMetaEnum(quint64 value, const QMetaObject *meta, const char *name)
{
    QMetaEnum me = meta->enumerator(meta->indexOfEnumerator(name));

    QString out;
    QTextStream ts(&out);

    if (const char *scope = me.scope()) {
        ts << scope << "::";
    }

    const char *key = me.valueToKey(static_cast<int>(value));
    const bool scoped = me.isScoped();
    if (scoped || !key) {
        ts << me.enumName() << (!key ? "(" : "::");
    }

    if (key) {
        ts << key;
    } else {
        ts << value << ")";
    }

    m_data->m_ts << out;
}

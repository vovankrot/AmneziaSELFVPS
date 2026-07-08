#ifndef LOGGER_H
#define LOGGER_H

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTextStream>

#include "mozilla/shared/loglevel.h"

class Logger : public QObject
{
    Q_OBJECT

public:
    static Logger &Instance();

    static bool init(bool isServiceLogger);
    static void deInit();

    static bool setServiceLogsEnabled(bool enabled);

    static bool openLogsFolder(bool isServiceLogger);

    static void clearLogs(bool isServiceLogger);
    static void clearServiceLogs();
    static void cleanUp();

    static QString userLogsFilePath();
    static QString serviceLogsFilePath();
    static QString systemLogDir();

    static QString getLogFile();
    static QString getServiceLogFile();

#ifdef Q_OS_WIN
    static void writeEmergencyCrashFile(const QString &reason, const QString &details);
#endif

    // compat with Mozilla logger
    Logger(const QString &className)
    {
        m_className = className;
    }

    Logger(Logger const &) = delete;
    Logger &operator=(Logger const &) = delete;

    const QString &className() const
    {
        return m_className;
    }

    class LogStreamer
    {
    public:
        LogStreamer(Logger *logger, LogLevel level);
        ~LogStreamer();

        LogStreamer &operator<<(uint64_t t);
        LogStreamer &operator<<(const char *t);
        LogStreamer &operator<<(const QString &t);
        LogStreamer &operator<<(const QStringList &t);
        LogStreamer &operator<<(const QByteArray &t);
        LogStreamer &operator<<(const QJsonObject &t);
        LogStreamer &operator<<(QTextStreamFunction t);
        LogStreamer &operator<<(const void *t);

        // Q_ENUM
        template<typename T> typename std::enable_if<QtPrivate::IsQEnumHelper<T>::Value, LogStreamer &>::type operator<<(T t)
        {
            const QMetaObject *meta = qt_getEnumMetaObject(t);
            const char *name = qt_getEnumName(t);
            addMetaEnum(typename QFlags<T>::Int(t), meta, name);
            return *this;
        }

    private:
        void addMetaEnum(quint64 value, const QMetaObject *meta, const char *name);

        Logger *m_logger;
        LogLevel m_logLevel;

        struct Data
        {
            Data() : m_ts(&m_buffer, QIODevice::WriteOnly)
            {
            }

            QString m_buffer;
            QTextStream m_ts;
        };

        Data *m_data;
    };

    LogStreamer error();
    LogStreamer warning();
    LogStreamer info();
    LogStreamer debug();
    QString sensitive(const QString &input);

    static void flush();

private:
    Logger() = default;

    static QString userLogsDir();
    static QString fallbackLogsDir(bool isServiceLogger);

    static QFile m_file;
    static QTextStream m_textStream;
    static QString m_logFileName;
    static QString m_serviceLogFileName;
    static QString m_userLogFilePath;
    static QString m_serviceLogFilePath;

    friend void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

    // compat with Mozilla logger
    QString m_className;
};

#endif // LOGGER_H

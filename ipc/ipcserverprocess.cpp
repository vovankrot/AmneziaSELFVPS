#include "ipcserverprocess.h"
#include "ipc.h"
#include <QProcess>

#ifndef Q_OS_IOS

IpcServerProcess::IpcServerProcess(QObject *parent) :
    IpcProcessInterfaceSource(parent),
    m_process(QSharedPointer<QProcess>(new QProcess()))
{
    connect(m_process.data(), &QProcess::errorOccurred, this, &IpcServerProcess::errorOccurred);
    connect(m_process.data(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &IpcServerProcess::finished);
    connect(m_process.data(), &QProcess::readyRead, this, &IpcServerProcess::readyRead);
    connect(m_process.data(), &QProcess::readyReadStandardError, this, &IpcServerProcess::readyReadStandardError);
    connect(m_process.data(), &QProcess::readyReadStandardOutput, this, &IpcServerProcess::readyReadStandardOutput);
    connect(m_process.data(), &QProcess::started, this, &IpcServerProcess::started);
    connect(m_process.data(), &QProcess::stateChanged, this, &IpcServerProcess::stateChanged);

    connect(m_process.data(), &QProcess::errorOccurred, [&](QProcess::ProcessError error){
        qDebug() << "IpcServerProcess errorOccurred " << error << "program=" << m_process->program();
    });

    // The client-side "finished" handler that would normally log the exit code never
    // fires for these crashes (the IPC channel itself appears to go down with the
    // process), so log it here on the service side where we know it's reliably seen.
    // On Windows this exit code IS the NTSTATUS the process died with when Crashed
    // (e.g. 0xC0000005 access violation, 0xC00000FD stack overflow) -- critical for
    // diagnosing the recurring tun2socks crash without a WER dump. by vovankrot
    connect(m_process.data(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        qDebug() << "IpcServerProcess finished" << m_process->program()
                  << "exitStatus=" << exitStatus
                  << "exitCode(dec)=" << exitCode
                  << "exitCode(hex)=" << Qt::hex << exitCode << Qt::dec;
    });

}

IpcServerProcess::~IpcServerProcess()
{
    qDebug() << "IpcServerProcess::~IpcServerProcess";
}

void IpcServerProcess::start()
{
    if (m_process->state() != QProcess::NotRunning) {
        qWarning() << "IPC: ignoring duplicate process start";
        return;
    }
    if (m_process->program().isEmpty() || !m_argumentsAccepted) {
        qCritical() << "IPC: refusing to start an invalid or empty privileged program";
        emit errorOccurred(QProcess::FailedToStart);
        return;
    }

    m_process->start();
    qDebug() << "IpcServerProcess started" << m_process->program()
             << "argument count=" << m_process->arguments().size();

    // QProcess reports started/errorOccurred asynchronously. Waiting here stalls
    // all service IPC, including cancellation and network policy requests.
}

void IpcServerProcess::terminate() {
    m_process->terminate();
}

void IpcServerProcess::kill() {
    m_process->kill();
}

void IpcServerProcess::close()
{
    if (isRunning()) m_process->kill();
    emit releaseRequested();
}

void IpcServerProcess::setArguments(const QStringList &arguments)
{
    const auto sanitized = amnezia::sanitizeArguments(m_program, arguments);
    m_argumentsAccepted = !sanitized.isEmpty() && sanitized == arguments;
    m_process->setArguments(m_argumentsAccepted ? sanitized : QStringList{});
    if (!m_argumentsAccepted) qWarning() << "IPC: rejected privileged process arguments";
}

void IpcServerProcess::setInputChannelMode(QProcess::InputChannelMode mode)
{
     m_process->setInputChannelMode(mode);
}

void IpcServerProcess::setNativeArguments(const QString &arguments)
{
#ifdef Q_OS_WIN
    // Native command-line strings would bypass the typed argument allowlist.
    m_process->setNativeArguments({});
    if (!arguments.isEmpty()) {
        m_argumentsAccepted = false;
        qWarning() << "IPC: rejected native privileged process arguments";
    }
#endif
}

void IpcServerProcess::setProcessChannelMode(QProcess::ProcessChannelMode mode)
{
    m_process->setProcessChannelMode(mode);
}

void IpcServerProcess::setProgram(int programId)
{
    m_argumentsAccepted = false;
#ifdef Q_OS_WIN
    m_process->setNativeArguments({});
#endif
    if (programId <= static_cast<int>(amnezia::PermittedProcess::Invalid)
            || programId >= static_cast<int>(amnezia::PermittedProcess::PermittedProcessCount)) {
        qCritical() << "IPC: rejected invalid privileged program id" << programId;
        m_program = amnezia::PermittedProcess::Invalid;
        m_process->setProgram({});
        m_process->setArguments({});
        return;
    }

    m_program = static_cast<amnezia::PermittedProcess>(programId);
    m_process->setProgram(amnezia::permittedProcessPath(m_program));
    m_process->setArguments({});
}

void IpcServerProcess::setWorkingDirectory(const QString &dir)
{
    // A caller-controlled working directory can affect privileged DLL/config loading.
    Q_UNUSED(dir)
    m_process->setWorkingDirectory(QFileInfo(m_process->program()).absolutePath());
}

QByteArray IpcServerProcess::readAll()
{
    return m_process->readAll();
}

QByteArray IpcServerProcess::readAllStandardError()
{
    return m_process->readAllStandardError();
}

QByteArray IpcServerProcess::readAllStandardOutput()
{
    return m_process->readAllStandardOutput();
}

bool IpcServerProcess::waitForStarted() {
    return m_process->waitForStarted();
}

bool IpcServerProcess::waitForStarted(int msecs) {
    return m_process->waitForStarted(msecs);
}

bool IpcServerProcess::waitForFinished() {
    return m_process->state() == QProcess::NotRunning || m_process->waitForFinished();
}

bool IpcServerProcess::waitForFinished(int msecs) {
    return m_process->state() == QProcess::NotRunning || m_process->waitForFinished(msecs);
}

#endif

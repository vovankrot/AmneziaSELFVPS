#include "sshclient.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QtConcurrent>

#include <fstream>

#ifndef Q_OS_WINDOWS
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#ifdef Q_OS_WINDOWS
#include <winsock2.h>
#include <mstcpip.h>
const uint32_t S_IRWXU = 0644;
#endif

namespace libssh {
    constexpr auto libsshTimeoutError{"Timeout connecting to"};

    std::function<QString()> Client::m_passphraseCallback;

    int Client::callback(const char *prompt, char *buf, size_t len, int echo, int verify, void *userdata)
    {
        auto passphrase = m_passphraseCallback();
        passphrase.toStdString().copy(buf, passphrase.size() + 1);
        return 0;
    }

    // Keep the TCP connection alive during long, output-silent remote commands
    // (e.g. `docker build` while curl downloads xray with no progress output) so
    // NAT/firewall idle-timeouts don't drop the session — that dropped session
    // otherwise surfaces as SshInternalError (SSH_FATAL, code 302) mid-install
    // and aborts the whole container build. by vovankrot
    static void enableTcpKeepalive(socket_t socketDescriptor)
    {
        if (socketDescriptor == SSH_INVALID_SOCKET) {
            return;
        }
#ifdef Q_OS_WINDOWS
        struct tcp_keepalive ka;
        ka.onoff = 1;
        ka.keepalivetime = 15000;    // begin probing after 15s idle
        ka.keepaliveinterval = 3000; // then probe every 3s
        DWORD bytesReturned = 0;
        WSAIoctl(socketDescriptor, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytesReturned, nullptr, nullptr);
#else
        int enable = 1;
        setsockopt(socketDescriptor, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
#ifdef TCP_KEEPIDLE
        int idle = 15;
        setsockopt(socketDescriptor, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
        int interval = 3;
        setsockopt(socketDescriptor, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
        int count = 5;
        setsockopt(socketDescriptor, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
#endif
    }

    ErrorCode Client::connectToHost(const ServerCredentials &credentials)
    {
        if (m_cancelRequested.load()) {
            return ErrorCode::ServerCancelInstallation;
        }

        if (m_session != nullptr) {
            if (!ssh_is_connected(m_session)) {
                ssh_free(m_session);
                m_session = nullptr;
                m_socketDescriptor.store(SSH_INVALID_SOCKET);
            }
        }

        if (m_session == nullptr) {
            m_session = ssh_new();

            if (m_session == nullptr) {
                qDebug() << "Failed to create ssh session";
                return ErrorCode::InternalError;
            }

            int port = credentials.port;
            int logVerbosity = SSH_LOG_NOLOG;
            long connectTimeout = 30; // seconds
            std::string hostIp = credentials.hostName.toStdString();
            std::string hostUsername = credentials.userName.toStdString() + "@" + hostIp;

            ssh_options_set(m_session, SSH_OPTIONS_HOST, hostIp.c_str());
            ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);
            ssh_options_set(m_session, SSH_OPTIONS_USER, hostUsername.c_str());
            ssh_options_set(m_session, SSH_OPTIONS_LOG_VERBOSITY, &logVerbosity);
            ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &connectTimeout);

            // NOTE: Do NOT use QEventLoop + QFutureWatcher::finished here!
            // This function may be called from a worker thread (QtConcurrent::run),
            // and QEventLoop::exec() in a worker thread without event loop = DEADLOCK.
            // QFutureWatcher::finished uses queued connection which requires event loop.
            QFuture<int> future = QtConcurrent::run([this]() {
                return ssh_connect(m_session);
            });
            future.waitForFinished();

            int connectionResult = future.result();

            if (connectionResult != SSH_OK) {
                m_socketDescriptor.store(SSH_INVALID_SOCKET);
                return fromLibsshErrorCode();
            }

            m_socketDescriptor.store(ssh_get_fd(m_session));
            enableTcpKeepalive(m_socketDescriptor.load());

            std::string authUsername = credentials.userName.toStdString();

            int authResult = SSH_ERROR;
            if (credentials.secretData.contains("BEGIN") && credentials.secretData.contains("PRIVATE KEY")) {
                ssh_key privateKey = nullptr;
                ssh_key publicKey = nullptr;
                authResult = ssh_pki_import_privkey_base64(credentials.secretData.toStdString().c_str(), nullptr, callback, nullptr, &privateKey);
                if (authResult == SSH_OK) {
                    authResult = ssh_pki_export_privkey_to_pubkey(privateKey, &publicKey);
                }

                if (authResult == SSH_OK) {
                    authResult = ssh_userauth_try_publickey(m_session, authUsername.c_str(), publicKey);
                }

                if (authResult == SSH_OK) {
                    authResult = ssh_userauth_publickey(m_session, authUsername.c_str(), privateKey);
                }

                if (publicKey) {
                    ssh_key_free(publicKey);
                }
                if (privateKey) {
                    ssh_key_free(privateKey);
                }
                if (authResult != SSH_OK) {
                    qCritical() << ssh_get_error(m_session);
                    ErrorCode errorCode = fromLibsshErrorCode();
                    if (errorCode == ErrorCode::NoError) {
                        errorCode = ErrorCode::SshPrivateKeyFormatError;
                    }
                    return errorCode;
                }
            } else {
                authResult = ssh_userauth_password(m_session, authUsername.c_str(), credentials.secretData.toStdString().c_str());
                if (authResult != SSH_OK) {
                    return fromLibsshErrorCode();
                }
            }
        }
        return ErrorCode::NoError;
    }

    void Client::requestCancel()
    {
        m_cancelRequested.store(true);
        interruptTransport();
    }

    void Client::resetCancel()
    {
        m_cancelRequested.store(false);
    }

    void Client::disconnectFromHost()
    {
        if (m_session != nullptr) {
            if (ssh_is_connected(m_session)) {
                ssh_disconnect(m_session);
            }
            ssh_free(m_session);
            m_session = nullptr;
        }
        m_socketDescriptor.store(SSH_INVALID_SOCKET);
    }

    void Client::interruptTransport()
    {
        const socket_t socketDescriptor = m_socketDescriptor.load();
        if (socketDescriptor == SSH_INVALID_SOCKET) {
            return;
        }

#ifdef Q_OS_WINDOWS
        ::shutdown(socketDescriptor, SD_BOTH);
#else
        ::shutdown(socketDescriptor, SHUT_RDWR);
#endif
    }

    int Client::lastExitStatus() const
    {
        return m_lastExitStatus;
    }

    ErrorCode Client::executeCommand(const QString &data,
                                        const std::function<ErrorCode (const QString &, Client &)> &cbReadStdOut,
                                        const std::function<ErrorCode (const QString &, Client &)> &cbReadStdErr,
                                        int idleTimeoutMs,
                                        int totalTimeoutMs)
    {
        if (m_cancelRequested.load()) {
            return ErrorCode::ServerCancelInstallation;
        }

        m_lastExitStatus = 0;

        m_channel = ssh_channel_new(m_session);

        if (m_channel == nullptr) {
            return closeChannel();
        }

        int result = ssh_channel_open_session(m_channel);

        if (result == SSH_OK && ssh_channel_is_open(m_channel)) {
            qDebug() << "SSH chanel opened";
        } else {
            return closeChannel();
        }

        // NOTE: Do NOT use QEventLoop + QFutureWatcher::finished here!
        // This function may be called from a worker thread (QtConcurrent::run),
        // and QEventLoop::exec() in a worker thread without event loop = DEADLOCK.
        QFuture<ErrorCode> future = QtConcurrent::run([this, &data, &cbReadStdOut, &cbReadStdErr, idleTimeoutMs, totalTimeoutMs]() {
            const size_t bufferSize = 2048;
            const int readTimeoutMs = 250;
            char buffer[bufferSize];

            QElapsedTimer totalTimer;
            QElapsedTimer lastActivityTimer;
            totalTimer.start();
            lastActivityTimer.start();

            int result = ssh_channel_request_exec(m_channel, data.toUtf8());
            if (result == SSH_OK) {
                while (true) {
                    if (m_cancelRequested.load()) {
                        closeChannel();
                        return ErrorCode::ServerCancelInstallation;
                    }

                    if (totalTimeoutMs > 0 && totalTimer.elapsed() >= totalTimeoutMs) {
                        qWarning() << "SSH command timed out after total wait" << totalTimeoutMs << "ms";
                        closeChannel();
                        return ErrorCode::SshCommandTimeoutError;
                    }

                    bool hadActivity = false;

                    auto drainStream = [&](bool isStdErr) -> ErrorCode {
                        while (true) {
                            if (m_cancelRequested.load()) {
                                return ErrorCode::ServerCancelInstallation;
                            }
                            const int bytesRead = ssh_channel_read_timeout(m_channel, buffer, sizeof(buffer), isStdErr, readTimeoutMs);
                            if (bytesRead > 0) {
                                hadActivity = true;
                                lastActivityTimer.restart();
                                const QString output = QString::fromUtf8(buffer, bytesRead);
                                if (isStdErr) {
                                    if (cbReadStdErr) {
                                        auto error = cbReadStdErr(output, *this);
                                        if (error != ErrorCode::NoError) {
                                            return error;
                                        }
                                    }
                                } else {
                                    if (cbReadStdOut) {
                                        auto error = cbReadStdOut(output, *this);
                                        if (error != ErrorCode::NoError) {
                                            return error;
                                        }
                                    }
                                }
                                continue;
                            }

                            if (bytesRead == SSH_AGAIN || bytesRead == 0) {
                                return ErrorCode::NoError;
                            }

                            closeChannel();
                            return fromLibsshErrorCode();
                        }
                    };

                    auto errorCode = drainStream(false);
                    if (errorCode != ErrorCode::NoError) {
                        return errorCode;
                    }

                    errorCode = drainStream(true);
                    if (errorCode != ErrorCode::NoError) {
                        return errorCode;
                    }

                    if (!hadActivity && idleTimeoutMs > 0 && lastActivityTimer.elapsed() >= idleTimeoutMs) {
                        qWarning() << "SSH command timed out due to inactivity after" << idleTimeoutMs << "ms";
                        closeChannel();
                        return ErrorCode::SshCommandTimeoutError;
                    }

                    if ((ssh_channel_is_eof(m_channel) || !ssh_channel_is_open(m_channel)) && !hadActivity) {
                        break;
                    }
                }
            } else {
                const ErrorCode error = fromLibsshErrorCode();
                closeChannel();
                return error != ErrorCode::NoError ? error : ErrorCode::ServerCommandFailedError;
            }
            return closeChannel();
        });
        future.waitForFinished();

        return future.result();
    }

    ErrorCode Client::writeResponse(const QString &data)
    {
        if (m_channel == nullptr) {
            qCritical() << "ssh channel not initialized";
            return fromLibsshErrorCode();
        }

        int bytesWritten = ssh_channel_write(m_channel, data.toUtf8(), (uint32_t)data.size());
        if (bytesWritten == data.size() && ssh_channel_write(m_channel, "\n", 1)) {
            return fromLibsshErrorCode();
        }
        return fromLibsshErrorCode();
    }

    ErrorCode Client::closeChannel()
    {
        if (m_channel != nullptr) {
            m_lastExitStatus = ssh_channel_get_exit_status(m_channel);

            if (!ssh_channel_is_eof(m_channel)) {
                ssh_channel_send_eof(m_channel);
            }
            if (ssh_channel_is_open(m_channel)) {
                ssh_channel_close(m_channel);
            }
            ssh_channel_free(m_channel);
            m_channel = nullptr;
        }

        const ErrorCode error = fromLibsshErrorCode();
        if (error != ErrorCode::NoError) {
            return error;
        }

        if (m_lastExitStatus != 0) {
            qWarning() << "SSH remote command exited with code" << m_lastExitStatus;
            return ErrorCode::ServerCommandFailedError;
        }

        return ErrorCode::NoError;
    }

    ErrorCode Client::scpFileCopy(const ScpOverwriteMode overwriteMode, const QString& localPath, const QString& remotePath, const QString &fileDesc)
    {
        if (m_cancelRequested.load()) {
            return ErrorCode::ServerCancelInstallation;
        }

        m_scpSession = ssh_scp_new(m_session, SSH_SCP_WRITE, remotePath.toStdString().c_str());

        if (m_scpSession == nullptr) {
            return fromLibsshErrorCode();
        }

        if (ssh_scp_init(m_scpSession) != SSH_OK) {
            auto errorCode = fromLibsshErrorCode();
            closeScpSession();
            return errorCode;
        }

        // NOTE: Do NOT use QEventLoop + QFutureWatcher::finished here!
        // This function may be called from a worker thread (QtConcurrent::run),
        // and QEventLoop::exec() in a worker thread without event loop = DEADLOCK.
        QFuture<ErrorCode> future = QtConcurrent::run([this, overwriteMode, &localPath, &remotePath, &fileDesc]() {
            const int accessType = O_WRONLY | O_CREAT | overwriteMode;
            const int localFileSize = QFileInfo(localPath).size();

            int result = ssh_scp_push_file(m_scpSession, remotePath.toStdString().c_str(), localFileSize, accessType);
            if (result != SSH_OK) {
                return fromLibsshErrorCode();
            }

            QFile fin(localPath);

            if (fin.open(QIODevice::ReadOnly)) {
                constexpr size_t bufferSize = 16384;
                int transferred = 0;
                int currentChunkSize = bufferSize;

                while (transferred < localFileSize) {
                    if (m_cancelRequested.load()) {
                        return ErrorCode::ServerCancelInstallation;
                    }

                    // Last Chunk
                    if ((localFileSize - transferred) < bufferSize) {
                        currentChunkSize = localFileSize % bufferSize;
                    }

                    QByteArray chunk = fin.read(currentChunkSize);
                    if (chunk.size() != currentChunkSize) {
                        return fromFileErrorCode(fin.error());
                    }

                    result = ssh_scp_write(m_scpSession, chunk.data(), chunk.size());
                    if (result != SSH_OK) {
                        return fromLibsshErrorCode();
                    }

                    transferred += currentChunkSize;
                }
            } else {
                return fromFileErrorCode(fin.error());
            }

            return ErrorCode::NoError;
        });
        future.waitForFinished();

        closeScpSession();
        return future.result();
    }

    void Client::closeScpSession()
    {
        if (m_scpSession != nullptr) {
            ssh_scp_free(m_scpSession);
            m_scpSession = nullptr;
        }
    }

    ErrorCode Client::fromLibsshErrorCode()
    {
        if (m_cancelRequested.load()) {
            return ErrorCode::ServerCancelInstallation;
        }

        int errorCode = ssh_get_error_code(m_session);
        if (errorCode != SSH_NO_ERROR) {
            QString errorMessage = ssh_get_error(m_session);
            qCritical() << errorMessage;
            if (errorMessage.contains(libsshTimeoutError)) {
                return ErrorCode::SshTimeoutError;
            }
        }

        switch (errorCode) {
        case(SSH_NO_ERROR): return ErrorCode::NoError;
        case(SSH_REQUEST_DENIED): return ErrorCode::SshRequestDeniedError;
        case(SSH_EINTR): return ErrorCode::SshInterruptedError;
        case(SSH_FATAL): return ErrorCode::SshInternalError;
        default: return ErrorCode::SshInternalError;
        }
    }

    ErrorCode Client::fromFileErrorCode(QFileDevice::FileError fileError)
    {
        switch (fileError) {
        case QFileDevice::NoError: return ErrorCode::NoError;
        case QFileDevice::ReadError: return ErrorCode::ReadError;
        case QFileDevice::OpenError: return ErrorCode::OpenError;
        case QFileDevice::PermissionsError: return ErrorCode::PermissionsError;
        case QFileDevice::FatalError: return ErrorCode::FatalError;
        case QFileDevice::AbortError: return ErrorCode::AbortError;
        default: return ErrorCode::UnspecifiedError;
        }
    }

    ErrorCode Client::getDecryptedPrivateKey(const ServerCredentials &credentials, QString &decryptedPrivateKey, const std::function<QString()> &passphraseCallback)
    {
        int authResult = SSH_ERROR;
        ErrorCode errorCode = ErrorCode::NoError;

        ssh_key privateKey = nullptr;
        m_passphraseCallback = passphraseCallback;
        authResult = ssh_pki_import_privkey_base64(credentials.secretData.toStdString().c_str(), nullptr, callback, nullptr, &privateKey);
        if (authResult == SSH_OK) {
            char *b64 = nullptr;

            authResult = ssh_pki_export_privkey_base64(privateKey, nullptr, nullptr, nullptr, &b64);
            decryptedPrivateKey = QString(b64);

            if (authResult != SSH_OK) {
                qDebug() << "failed to export private key";
                errorCode = ErrorCode::InternalError;
            }
            else {
                ssh_string_free_char(b64);
            }
        } else {
            errorCode = ErrorCode::SshPrivateKeyError;
        }

        if (privateKey) {
            ssh_key_free(privateKey);
        }
        return errorCode;
    }
}

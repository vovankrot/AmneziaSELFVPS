#pragma once
#include "settings.h"
#include "protocols/protocols_defs.h"
namespace libssh { enum ScpOverwriteMode { ScpOverwriteExisting }; }
class ServerController {
public:
    QMap<QString, QByteArray> remote;
    QString failedRead, failedUpload;
    bool failSwap = false, failRestart = false;
    QStringList scripts, uploads;
    QByteArray getTextFileFromContainer(DockerContainer, const ServerCredentials &, const QString &path, ErrorCode &error) {
        error = path == failedRead || !remote.contains(path) ? ErrorCode::InternalError : ErrorCode::NoError;
        return error == ErrorCode::NoError ? remote.value(path) : QByteArray();
    }
    ErrorCode uploadTextFileToContainer(DockerContainer, const ServerCredentials &, const QString &data,
        const QString &path, libssh::ScpOverwriteMode) {
        uploads << path;
        if (path.startsWith(failedUpload) && !failedUpload.isEmpty()) return ErrorCode::InternalError;
        remote[path] = data.toUtf8(); return ErrorCode::NoError;
    }
    ErrorCode runContainerScript(const ServerCredentials &, DockerContainer, const QString &script) {
        scripts << script;
        return failSwap && script.contains("trap rollback ERR") ? ErrorCode::InternalError : ErrorCode::NoError;
    }
    ErrorCode runScript(const ServerCredentials &, const QString &script) {
        scripts << script;
        return failRestart && script.contains("docker restart") ? ErrorCode::InternalError : ErrorCode::NoError;
    }
};

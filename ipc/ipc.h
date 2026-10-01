#ifndef IPC_H
#define IPC_H

#include <QObject>
#include <QString>
#include <QFileInfo>

#include "../client/utilities.h"

#define IPC_SERVICE_URL "local:AmneziaVpnIpcInterface"

namespace amnezia {

enum PermittedProcess {
    Invalid,
    OpenVPN,
    Wireguard,
    Tun2Socks,
    CertUtil,
    PermittedProcessCount
};

inline QString permittedProcessPath(PermittedProcess pid)
{
    switch (pid) {
        case PermittedProcess::OpenVPN:
            return Utils::openVpnExecPath();
        case PermittedProcess::Wireguard:
            return Utils::wireguardExecPath();
        case PermittedProcess::CertUtil:
            return Utils::certUtilPath();
        case PermittedProcess::Tun2Socks:
            return Utils::tun2socksPath();
        default:
            return "";
    }
}


inline QString getIpcServiceUrl() {
#ifdef Q_OS_WIN
    return IPC_SERVICE_URL;
#else
    return QString("/tmp/%1").arg(IPC_SERVICE_URL);
#endif
}

inline QString getIpcProcessUrl(int pid) {
#ifdef Q_OS_WIN
    return QString("%1_%2").arg(IPC_SERVICE_URL).arg(pid);
#else
    return QString("/tmp/%1_%2").arg(IPC_SERVICE_URL).arg(pid);
#endif
}

inline QStringList sanitizeArguments(PermittedProcess proc, const QStringList &args) {
    using Validator = std::function<bool(const QString&)>;
    QMap<QString, Validator> namedArgs;
    QList<Validator> positionalArgs;

    switch (proc) {
    case OpenVPN: {
        namedArgs["--config"] = [](const QString& v) { return !v.isEmpty(); };
        namedArgs["--management"] = [](const QString& v) { return !v.isEmpty(); };
        namedArgs["--management-client"] = nullptr;
        positionalArgs.append([](const QString& v) {
            bool ok = false;
            const int port = v.toInt(&ok);
            return ok && port > 0 && port <= 65535;
        });
        break;
    }
    case Tun2Socks:
        // v2.7.0 (upgraded 2026-08-05) uses cobra/pflag, which requires the
        // double-dash long form -- a single dash before a multi-character name
        // is parsed as bundled short flags and fails. See xrayprotocol.cpp::
        // startTun2Socks() for the full upgrade note.
        namedArgs["--device"] = [](const QString& v) { return v.startsWith("tun://"); };
        namedArgs["--proxy"] = [](const QString& v) { return v.startsWith("socks5://"); };
        // NOTE: `--tcp-auto-tuning` and `-stack` are NOT whitelisted on purpose --
        // auto-tuning is opt-in and unused, and this build still has no `-stack`
        // flag (single gVisor netstack).
        break;
    case CertUtil:
        // Only the IKEv2 certificate import operation is permitted. Never expose
        // certutil's download, decode, store deletion or arbitrary output modes.
        if (args.size() == 6 && args[0] == "-f" && args[1] == "-importpfx"
                && args[2] == "-p" && args[5] == "NoExport"
                && QFileInfo(args[4]).isAbsolute() && QFileInfo(args[4]).isFile()
                && !args[4].contains('\n') && !args[4].contains('\r')) {
            return args;
        }
        return {};
    default:
        return {};
    }


    QStringList sanitized;

    for (int i = 0, pos = 0; i < args.size(); i++) {
        const auto& key = args[i];

        if (const auto found = namedArgs.find(key); found != namedArgs.end()) {
            const auto validator = found.value();

            if (validator) {
                if (i + 1 < args.size()) {
                    const auto& value = args[i+1];
                    if (validator(value)) {
                        sanitized << key << value;
                        i++;
                    }
                }
            } else {
                sanitized << key;
            }
        } else if (pos < positionalArgs.size()) {
            if (const auto validator = positionalArgs[pos]; validator && validator(key)) {
                sanitized << key;
                pos++;
            }
        }
    }

    return sanitized;
}

} // namespace amnezia

#endif // IPC_H

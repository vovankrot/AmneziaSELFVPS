#ifndef UTILITIES_H
#define UTILITIES_H

#include <QRegExp>
#include <QRegularExpression>
#include <QString>
#include <QJsonDocument>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

class Utils : public QObject
{
    Q_OBJECT

public:
    static QString getRandomString(int len);
    static QString SafeBase64Decode(QString string);
    static QString VerifyJsonString(const QString &source);
    static QString JsonToString(const QJsonObject &json, QJsonDocument::JsonFormat format);
    static QString JsonToString(const QJsonArray &array, QJsonDocument::JsonFormat format);
    static QJsonObject JsonFromString(const QString &string);
    static QString executable(const QString &baseName, bool absPath);
    static QString usrExecutable(const QString &baseName);
    static bool createEmptyFile(const QString &path);
    static bool initializePath(const QString &path);

    static bool processIsRunning(const QString &fileName, const bool fullFlag = false);
    static bool killProcessByName(const QString &name);

    static QString openVpnExecPath();
    static QString wireguardExecPath();
    static QString certUtilPath();
    static QString tun2socksPath();
    static QString hysteriaPath();
    static QString anytlsPath();

    // Returns the Windows system proxy server string (e.g. "127.0.0.1:10801")
    // when ProxyEnable=1 in HKCU Internet Settings, otherwise an empty string.
    // Always returns an empty string on non-Windows platforms.
    static QString getActiveSystemProxy();
    // Returns true when an enabled system proxy points to a loopback address
    // (127.0.0.1 / localhost), i.e. a local interceptor that would sit in front
    // of the VPN. When provided, outServer receives the proxy server string.
    static bool isLoopbackSystemProxyActive(QString *outServer = nullptr);
    // Disables the Windows system proxy (ProxyEnable=0) and notifies running
    // applications about the change. Returns true on success; no-op elsewhere.
    static bool disableSystemProxy();

    static void logException(const std::exception &e);
    static void logException(const std::exception_ptr &eptr = std::current_exception());

#ifdef Q_OS_WIN
    static bool signalCtrl(DWORD dwProcessId, DWORD dwCtrlEvent);
    static QString getNextDriverLetter();
#endif
};

#endif // UTILITIES_H

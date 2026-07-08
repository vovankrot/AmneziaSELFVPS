#ifndef XRAY_H
#define XRAY_H

#include <QString>
#include <QByteArray>
#include <atomic>
#ifdef Q_OS_LINUX
#include <QMutex>
#endif

class Xray
{
public:
    static Xray& getInstance()
    {
        static Xray instance;
        return instance;
    }

    bool startXray(const QString& cfg);
    bool stopXray();

    // Re-detect the default physical NIC. Thread-safe; called from
    // NetworkWatcher::networkChanged while sockCallback runs on Go threads.
    void refreshDefaultInterface();

private:
    static void ctxSockCallback(uintptr_t fd, void* ctx) {
        reinterpret_cast<Xray*>(ctx)->sockCallback(fd);
    }
    static void ctxLogHandler(char* str, void* ctx) {
        reinterpret_cast<Xray*>(ctx)->logHandler(str);
    }

    void sockCallback(uintptr_t fd);
    void logHandler(char* str);

#ifdef Q_OS_LINUX
    QByteArray m_defaultIfaceName;
    QMutex m_ifaceMutex;
#else
    std::atomic<int> m_defaultIfaceIdx{0};
#endif
};

#endif // XRAY_H

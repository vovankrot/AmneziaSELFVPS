#include "client/core/serviceHealthProbe.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <cstdio>

int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if (args.size()==3) {
        QLocalServer server;
        if (!server.listen(args[1])) return 1;
        QObject::connect(&server,&QLocalServer::newConnection,[&] {
            auto *socket=server.nextPendingConnection();
            QObject::connect(socket,&QLocalSocket::readyRead,[&,socket] {
                socket->readAll();
                if (args[2]=="reply") { socket->write("{\"type\":\"logs\",\"logs\":\"\"}\n"); socket->flush(); }
            });
        });
        std::puts("READY"); std::fflush(stdout);
        QTimer::singleShot(10000,&app,&QCoreApplication::quit);
        return app.exec();
    }
    for (const auto &mode : {QString("reply"),QString("silent")}) {
        const QString name="selfvps-health-test-"+QUuid::createUuid().toString(QUuid::Id128);
        QProcess child;
        child.start(app.applicationFilePath(),{name,mode});
        if (!child.waitForReadyRead(2000) || !child.readAllStandardOutput().contains("READY")) return 2;
        const bool healthy=serviceHealthProbe(name);
        child.kill(); child.waitForFinished(2000);
        if (healthy != (mode=="reply")) return 3;
    }
    std::puts("PASS: IPC must reply; connection to a silent pipe is insufficient");
    return 0;
}

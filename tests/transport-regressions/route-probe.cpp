#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include "client/core/networkUtilities.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
    const auto route = NetworkUtilities::getGatewayAndIface();
    const bool valid = route.second.isValid() && NetworkUtilities::checkIPv4Format(route.first);
    // Read-only live check: no IP addresses or network configuration exported.
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"valid", valid},
                              {"interface_index", route.second.index()}}).toJson(QJsonDocument::Compact) << '\n';
    return valid ? 0 : 1;
}

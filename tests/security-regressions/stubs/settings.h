#pragma once
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include "containers/containers_defs.h"
#include "core/defs.h"
#include "core/protectedBlob.h"
using namespace amnezia;
// Only local persistence and remote transport are substituted. The complete
// production snapshot manager, encryption and transaction control flow run.
class Settings {
public:
    QByteArray key = QByteArray(32, 's');
    ServerCredentials credentials {"snapshot.example", "root", "test", 22};
    QJsonObject current {{"container", "amnezia-xray"}, {"revision", "current"}};
    QByteArray protectSnapshot(const QByteArray &data) const { return ProtectedBlob::seal(data, key); }
    QByteArray openSnapshot(const QByteArray &data) const { return ProtectedBlob::open(data, key); }
    QJsonArray serversArray() const { return {QJsonObject()}; }
    int serversCount() const { return 1; }
    ServerCredentials serverCredentials(int) const { return credentials; }
    QJsonObject containerConfig(int index, DockerContainer) const { return index == 0 ? current : QJsonObject(); }
};

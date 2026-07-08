#include "hysteria2ConfigModel.h"

#include "protocols/protocols_defs.h"

Hysteria2ConfigModel::Hysteria2ConfigModel(QObject *parent) : QAbstractListModel(parent)
{
}

int Hysteria2ConfigModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

bool Hysteria2ConfigModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= ContainerProps::allContainers().size()) {
        return false;
    }

    switch (role) {
    case Roles::SiteRole: m_protocolConfig.insert(config_key::site, value.toString()); break;
    case Roles::PortRole: m_protocolConfig.insert(config_key::port, value.toString()); break;
    }

    emit dataChanged(index, index, QList { role });
    return true;
}

QVariant Hysteria2ConfigModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return false;
    }

    switch (role) {
    case Roles::SiteRole:
        return m_protocolConfig.value(config_key::site).toString(protocols::hysteria2::defaultMasqueradeHost);
    case Roles::PortRole: return m_protocolConfig.value(config_key::port).toString(protocols::hysteria2::defaultPort);
    }

    return QVariant();
}

void Hysteria2ConfigModel::updateModel(const QJsonObject &config)
{
    beginResetModel();
    m_container = ContainerProps::containerFromString(config.value(config_key::container).toString());

    m_fullConfig = config;
    QJsonObject protocolConfig = config.value(config_key::hysteria2).toObject();

    m_protocolConfig.insert(config_key::transport_proto,
                            protocolConfig.value(config_key::transport_proto)
                                    .toString(ProtocolProps::transportProtoToString(
                                            ProtocolProps::defaultTransportProto(Proto::Hysteria2), Proto::Hysteria2)));
    m_protocolConfig.insert(config_key::port, protocolConfig.value(config_key::port).toString(protocols::hysteria2::defaultPort));
    m_protocolConfig.insert(config_key::site,
                            protocolConfig.value(config_key::site).toString(protocols::hysteria2::defaultMasqueradeHost));

    endResetModel();
}

QJsonObject Hysteria2ConfigModel::getConfig()
{
    m_fullConfig.insert(config_key::hysteria2, m_protocolConfig);
    return m_fullConfig;
}

QHash<int, QByteArray> Hysteria2ConfigModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[SiteRole] = "site";
    roles[PortRole] = "port";

    return roles;
}
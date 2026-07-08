#include "anyTlsConfigModel.h"

#include "protocols/protocols_defs.h"

AnyTlsConfigModel::AnyTlsConfigModel(QObject *parent) : QAbstractListModel(parent)
{
}

int AnyTlsConfigModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

bool AnyTlsConfigModel::setData(const QModelIndex &index, const QVariant &value, int role)
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

QVariant AnyTlsConfigModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return false;
    }

    switch (role) {
    case Roles::SiteRole: return m_protocolConfig.value(config_key::site).toString(protocols::anytls::defaultSni);
    case Roles::PortRole: return m_protocolConfig.value(config_key::port).toString(protocols::anytls::defaultPort);
    }

    return QVariant();
}

void AnyTlsConfigModel::updateModel(const QJsonObject &config)
{
    beginResetModel();
    m_container = ContainerProps::containerFromString(config.value(config_key::container).toString());

    m_fullConfig = config;
    QJsonObject protocolConfig = config.value(config_key::anytls).toObject();

    m_protocolConfig.insert(config_key::transport_proto,
                            protocolConfig.value(config_key::transport_proto)
                                    .toString(ProtocolProps::transportProtoToString(
                                            ProtocolProps::defaultTransportProto(Proto::AnyTls), Proto::AnyTls)));
    m_protocolConfig.insert(config_key::port, protocolConfig.value(config_key::port).toString(protocols::anytls::defaultPort));
    m_protocolConfig.insert(config_key::site, protocolConfig.value(config_key::site).toString(protocols::anytls::defaultSni));

    endResetModel();
}

QJsonObject AnyTlsConfigModel::getConfig()
{
    m_fullConfig.insert(config_key::anytls, m_protocolConfig);
    return m_fullConfig;
}

QHash<int, QByteArray> AnyTlsConfigModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[SiteRole] = "site";
    roles[PortRole] = "port";

    return roles;
}
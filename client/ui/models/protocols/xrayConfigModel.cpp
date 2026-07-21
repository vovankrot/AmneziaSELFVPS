#include "xrayConfigModel.h"

#include "protocols/protocols_defs.h"

XrayConfigModel::XrayConfigModel(QObject *parent) : QAbstractListModel(parent)
{
}

int XrayConfigModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

bool XrayConfigModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= ContainerProps::allContainers().size()) {
        return false;
    }

    switch (role) {
    case Roles::SiteRole: m_protocolConfig.insert(config_key::site, value.toString()); break;
    case Roles::PortRole: m_protocolConfig.insert(config_key::port, value.toString()); break;
    case Roles::MaskTypeRole: m_protocolConfig.insert(config_key::maskType, value.toString()); break;
    case Roles::PacketSizeRole: m_protocolConfig.insert(config_key::packetSize, value.toString()); break;
    case Roles::KcpMtuRole: m_protocolConfig.insert(config_key::kcpMtu, value.toInt()); break;
    case Roles::KcpTtiRole: m_protocolConfig.insert(config_key::kcpTti, value.toInt()); break;
    case Roles::KcpUplinkCapacityRole: m_protocolConfig.insert(config_key::kcpUplinkCapacity, value.toInt()); break;
    case Roles::KcpDownlinkCapacityRole: m_protocolConfig.insert(config_key::kcpDownlinkCapacity, value.toInt()); break;
    case Roles::KcpCongestionRole: m_protocolConfig.insert(config_key::kcpCongestion, value.toBool()); break;
    case Roles::KcpReadBufferSizeRole: m_protocolConfig.insert(config_key::kcpReadBufferSize, value.toInt()); break;
    case Roles::KcpWriteBufferSizeRole: m_protocolConfig.insert(config_key::kcpWriteBufferSize, value.toInt()); break;
    }

    emit dataChanged(index, index, QList { role });
    return true;
}

QVariant XrayConfigModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return false;
    }

    switch (role) {
    case Roles::SiteRole: return m_protocolConfig.value(config_key::site).toString(protocols::xray::defaultSite);
    case Roles::PortRole: return m_protocolConfig.value(config_key::port).toString(protocols::xray::defaultPort);
    case Roles::MaskTypeRole:
        return m_protocolConfig.value(config_key::maskType).toString(protocols::xray::defaultMaskType);
    case Roles::PacketSizeRole:
        return m_protocolConfig.value(config_key::packetSize).toString(protocols::xray::defaultPacketSize);
    case Roles::KcpMtuRole: return m_protocolConfig.value(config_key::kcpMtu).toInt(protocols::xray::defaultKcpMtu);
    case Roles::KcpTtiRole: return m_protocolConfig.value(config_key::kcpTti).toInt(protocols::xray::defaultKcpTti);
    case Roles::KcpUplinkCapacityRole:
        return m_protocolConfig.value(config_key::kcpUplinkCapacity).toInt(protocols::xray::defaultKcpUplinkCapacity);
    case Roles::KcpDownlinkCapacityRole:
        return m_protocolConfig.value(config_key::kcpDownlinkCapacity).toInt(protocols::xray::defaultKcpDownlinkCapacity);
    case Roles::KcpCongestionRole:
        return m_protocolConfig.value(config_key::kcpCongestion).toBool(protocols::xray::defaultKcpCongestion);
    case Roles::KcpReadBufferSizeRole:
        return m_protocolConfig.value(config_key::kcpReadBufferSize).toInt(protocols::xray::defaultKcpReadBufferSize);
    case Roles::KcpWriteBufferSizeRole:
        return m_protocolConfig.value(config_key::kcpWriteBufferSize).toInt(protocols::xray::defaultKcpWriteBufferSize);
    }

    return QVariant();
}

void XrayConfigModel::updateModel(const QJsonObject &config)
{
    beginResetModel();
    m_container = ContainerProps::containerFromString(config.value(config_key::container).toString());

    m_fullConfig = config;
    QJsonObject protocolConfig = config.value(config_key::xray).toObject();

    auto defaultTransportProto = ProtocolProps::transportProtoToString(ProtocolProps::defaultTransportProto(Proto::Xray), Proto::Xray);
    m_protocolConfig.insert(config_key::transport_proto,
                            protocolConfig.value(config_key::transport_proto).toString(defaultTransportProto));
    m_protocolConfig.insert(config_key::port, protocolConfig.value(config_key::port).toString(protocols::xray::defaultPort));
    m_protocolConfig.insert(config_key::site, protocolConfig.value(config_key::site).toString(protocols::xray::defaultSite));

    // Advanced mKCP + FinalMask values fall back to the stock ones when absent, so
    // configs saved before these knobs existed keep behaving exactly as they did.
    m_protocolConfig.insert(config_key::maskType,
                            protocolConfig.value(config_key::maskType).toString(protocols::xray::defaultMaskType));
    m_protocolConfig.insert(config_key::packetSize,
                            protocolConfig.value(config_key::packetSize).toString(protocols::xray::defaultPacketSize));
    m_protocolConfig.insert(config_key::kcpMtu,
                            protocolConfig.value(config_key::kcpMtu).toInt(protocols::xray::defaultKcpMtu));
    m_protocolConfig.insert(config_key::kcpTti,
                            protocolConfig.value(config_key::kcpTti).toInt(protocols::xray::defaultKcpTti));
    m_protocolConfig.insert(config_key::kcpUplinkCapacity,
                            protocolConfig.value(config_key::kcpUplinkCapacity).toInt(protocols::xray::defaultKcpUplinkCapacity));
    m_protocolConfig.insert(config_key::kcpDownlinkCapacity,
                            protocolConfig.value(config_key::kcpDownlinkCapacity).toInt(protocols::xray::defaultKcpDownlinkCapacity));
    m_protocolConfig.insert(config_key::kcpCongestion,
                            protocolConfig.value(config_key::kcpCongestion).toBool(protocols::xray::defaultKcpCongestion));
    m_protocolConfig.insert(config_key::kcpReadBufferSize,
                            protocolConfig.value(config_key::kcpReadBufferSize).toInt(protocols::xray::defaultKcpReadBufferSize));
    m_protocolConfig.insert(config_key::kcpWriteBufferSize,
                            protocolConfig.value(config_key::kcpWriteBufferSize).toInt(protocols::xray::defaultKcpWriteBufferSize));

    endResetModel();
}

QJsonObject XrayConfigModel::getConfig()
{
    m_fullConfig.insert(config_key::xray, m_protocolConfig);
    return m_fullConfig;
}

void XrayConfigModel::resetAdvancedToDefaults()
{
    beginResetModel();

    m_protocolConfig.insert(config_key::maskType, QString(protocols::xray::defaultMaskType));
    m_protocolConfig.insert(config_key::packetSize, QString(protocols::xray::defaultPacketSize));
    m_protocolConfig.insert(config_key::kcpMtu, protocols::xray::defaultKcpMtu);
    m_protocolConfig.insert(config_key::kcpTti, protocols::xray::defaultKcpTti);
    m_protocolConfig.insert(config_key::kcpUplinkCapacity, protocols::xray::defaultKcpUplinkCapacity);
    m_protocolConfig.insert(config_key::kcpDownlinkCapacity, protocols::xray::defaultKcpDownlinkCapacity);
    m_protocolConfig.insert(config_key::kcpCongestion, protocols::xray::defaultKcpCongestion);
    m_protocolConfig.insert(config_key::kcpReadBufferSize, protocols::xray::defaultKcpReadBufferSize);
    m_protocolConfig.insert(config_key::kcpWriteBufferSize, protocols::xray::defaultKcpWriteBufferSize);

    endResetModel();
}

bool XrayConfigModel::isAdvancedModified() const
{
    const QModelIndex idx = index(0, 0);

    return data(idx, MaskTypeRole).toString() != QString(protocols::xray::defaultMaskType)
        || data(idx, PacketSizeRole).toString() != QString(protocols::xray::defaultPacketSize)
        || data(idx, KcpMtuRole).toInt() != protocols::xray::defaultKcpMtu
        || data(idx, KcpTtiRole).toInt() != protocols::xray::defaultKcpTti
        || data(idx, KcpUplinkCapacityRole).toInt() != protocols::xray::defaultKcpUplinkCapacity
        || data(idx, KcpDownlinkCapacityRole).toInt() != protocols::xray::defaultKcpDownlinkCapacity
        || data(idx, KcpCongestionRole).toBool() != protocols::xray::defaultKcpCongestion
        || data(idx, KcpReadBufferSizeRole).toInt() != protocols::xray::defaultKcpReadBufferSize
        || data(idx, KcpWriteBufferSizeRole).toInt() != protocols::xray::defaultKcpWriteBufferSize;
}

QHash<int, QByteArray> XrayConfigModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[SiteRole] = "site";
    roles[PortRole] = "port";
    roles[MaskTypeRole] = "maskType";
    roles[PacketSizeRole] = "packetSize";
    roles[KcpMtuRole] = "kcpMtu";
    roles[KcpTtiRole] = "kcpTti";
    roles[KcpUplinkCapacityRole] = "kcpUplinkCapacity";
    roles[KcpDownlinkCapacityRole] = "kcpDownlinkCapacity";
    roles[KcpCongestionRole] = "kcpCongestion";
    roles[KcpReadBufferSizeRole] = "kcpReadBufferSize";
    roles[KcpWriteBufferSizeRole] = "kcpWriteBufferSize";

    return roles;
}

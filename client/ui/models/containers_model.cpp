#include "containers_model.h"

#include <QJsonArray>

namespace {
bool isCuratedInstallableVpn(amnezia::DockerContainer container)
{
    // Desktop-hardened curated set: obfuscated/proxy-class protocols suitable
    // for new installs. AnyTLS is exposed from the XRay setup page as a
    // variant toggle; already-installed AnyTLS containers still remain visible
    // through the isInstalled branch in the QML filters.
    switch (container) {
    case amnezia::DockerContainer::Cloak:
    case amnezia::DockerContainer::Xray:
    case amnezia::DockerContainer::SSXray:
    case amnezia::DockerContainer::Hysteria2:
        return true;
    default:
        return false;
    }
}
}

ContainersModel::ContainersModel(QObject *parent) : QAbstractListModel(parent)
{
    // Default to None so a never-selected processed index can't be cast to a
    // random DockerContainer and silently target the wrong (or non-existent)
    // container on removal. by vovankrot
    m_processedContainerIndex = static_cast<int>(DockerContainer::None);
}

int ContainersModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ContainerProps::allContainers().size();
}

QVariant ContainersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= ContainerProps::allContainers().size()) {
        return QVariant();
    }

    DockerContainer container = ContainerProps::allContainers().at(index.row());
    QString protocolKey = ContainerProps::containerTypeToProtocolString(container);
    auto isThirdPartyConfig = m_containers.value(container).value(protocolKey).toObject().value(config_key::isThirdPartyConfig).toBool();

    switch (role) {
    case NameRole: {
        if (container == DockerContainer::Awg && !isThirdPartyConfig) {
            return "AmneziaWG Legacy";
        }
        return ContainerProps::containerHumanNames().value(container);
    }
    case DescriptionRole: {
        if (container == DockerContainer::Awg && !isThirdPartyConfig) {
            return QObject::tr("AmneziaWG Legacy is a outdated version of AmneziaWG protocol. To upgrade, install AmneziaWG and recreate users.");
        }

        return ContainerProps::containerDescriptions().value(container);
    }
    case DetailedDescriptionRole: return ContainerProps::containerDetailedDescriptions().value(container);
    case ConfigRole: {
        if (container == DockerContainer::None) {
            return QJsonObject();
        }
        return m_containers.value(container);
    }
    case IsThirdPartyConfigRole: return isThirdPartyConfig;
    case ServiceTypeRole: return ContainerProps::containerService(container);
    case DockerContainerRole: return container;
    case IsEasySetupContainerRole: return ContainerProps::isEasySetupContainer(container);
    case EasySetupHeaderRole: return ContainerProps::easySetupHeader(container);
    case EasySetupDescriptionRole: return ContainerProps::easySetupDescription(container);
    case EasySetupOrderRole: return ContainerProps::easySetupOrder(container);
    case IsInstallationAllowedRole: return ContainersModel::isInstallationAllowed(container);
    case IsInstalledRole: return m_containers.contains(container);
    case IsCurrentlyProcessedRole: return container == static_cast<DockerContainer>(m_processedContainerIndex);
    case IsSupportedRole: return ContainerProps::isSupportedByCurrentPlatform(container);
    case IsShareableRole: return ContainerProps::isShareable(container);
    case InstallPageOrderRole: return ContainerProps::installPageOrder(container);
    }

    return QVariant();
}

QVariant ContainersModel::data(const int index, int role) const
{
    QModelIndex modelIndex = this->index(index);
    return data(modelIndex, role);
}

void ContainersModel::updateModel(const QJsonArray &containers)
{
    beginResetModel();
    m_containers.clear();
    for (const QJsonValue &val : containers) {
        m_containers.insert(ContainerProps::containerFromString(val.toObject().value(config_key::container).toString()), val.toObject());
    }
    endResetModel();
}

void ContainersModel::setProcessedContainerIndex(int index)
{
    m_processedContainerIndex = index;
}

int ContainersModel::getProcessedContainerIndex()
{
    return m_processedContainerIndex;
}

QString ContainersModel::getProcessedContainerName()
{
    return ContainerProps::containerHumanNames().value(static_cast<DockerContainer>(m_processedContainerIndex));
}

QJsonObject ContainersModel::getContainerConfig(const int containerIndex)
{
    return qvariant_cast<QJsonObject>(data(index(containerIndex), ConfigRole));
}

bool ContainersModel::isSupportedByCurrentPlatform(const int containerIndex)
{
    return qvariant_cast<bool>(data(index(containerIndex), IsSupportedRole));
}

bool ContainersModel::isServiceContainer(const int containerIndex)
{
    return qvariant_cast<amnezia::ServiceType>(data(index(containerIndex), ServiceTypeRole) == ServiceType::Other);
}

bool ContainersModel::isInstalled(const int containerIndex) const
{
    return m_containers.contains(static_cast<DockerContainer>(containerIndex));
}

bool ContainersModel::hasInstalledServices()
{
    for (const auto &container : m_containers.keys()) {
        if (ContainerProps::containerService(container) == ServiceType::Other) {
            return true;
        }
    }
    return false;
}

bool ContainersModel::hasInstalledProtocols()
{
    for (const auto &container : m_containers.keys()) {
        if (ContainerProps::containerService(container) == ServiceType::Vpn) {
            return true;
        }
    }
    return false;
}

bool ContainersModel::isInstallationAllowed(DockerContainer container)
{
    if (ContainerProps::containerService(container) != ServiceType::Vpn) {
        return container != DockerContainer::Awg;
    }

    // desktop-hardened exposes only the curated VPN set for new installs.
    // Keep field-proven obfuscated options available; already-installed legacy
    // containers stay visible via isInstalled filters.
    return isCuratedInstallableVpn(container);
}

QHash<int, QByteArray> ContainersModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[DescriptionRole] = "description";
    roles[DetailedDescriptionRole] = "detailedDescription";
    roles[ServiceTypeRole] = "serviceType";
    roles[DockerContainerRole] = "dockerContainer";
    roles[ConfigRole] = "config";
    roles[IsThirdPartyConfigRole] = "isThirdPartyConfig";

    roles[IsEasySetupContainerRole] = "isEasySetupContainer";
    roles[EasySetupHeaderRole] = "easySetupHeader";
    roles[EasySetupDescriptionRole] = "easySetupDescription";
    roles[EasySetupOrderRole] = "easySetupOrder";

    roles[IsInstalledRole] = "isInstalled";
    roles[IsCurrentlyProcessedRole] = "isCurrentlyProcessed";
    roles[IsSupportedRole] = "isSupported";
    roles[IsShareableRole] = "isShareable";
    roles[IsInstallationAllowedRole] = "isInstallationAllowed";
    roles[InstallPageOrderRole] = "installPageOrder";
    return roles;
}

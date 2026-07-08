#include "sitesController.h"

#include <QFile>
#include <QHostInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include "containers/containers_defs.h"
#include "systemController.h"
#include "core/networkUtilities.h"

namespace {
bool hasMeaningfulWildcardContent(const QString &hostname)
{
    static const QRegularExpression meaningfulPattern(QStringLiteral("[A-Za-z0-9]"));

    QString stripped = hostname;
    stripped.remove('*');
    return meaningfulPattern.match(stripped).hasMatch();
}

QString normalizeSiteInput(QString hostname)
{
    hostname = hostname.trimmed();
    const bool isIpSubnet = NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(hostname);

    if (!isIpSubnet) {
        if (hostname.startsWith("https://", Qt::CaseInsensitive)) {
            hostname.remove(0, 8);
        } else if (hostname.startsWith("http://", Qt::CaseInsensitive)) {
            hostname.remove(0, 7);
        } else if (hostname.startsWith("ftp://", Qt::CaseInsensitive)) {
            hostname.remove(0, 6);
        }

        const int slashIndex = hostname.indexOf('/');
        if (slashIndex >= 0) {
            hostname = hostname.left(slashIndex);
        }

        hostname = hostname.trimmed().toLower();
    }

    if (hostname.contains('*')) {
        QString normalizedMask;
        normalizedMask.reserve(hostname.size());

        bool previousWasWildcard = false;
        for (const QChar character : hostname) {
            if (character == '*') {
                if (!previousWasWildcard) {
                    normalizedMask.append(character);
                }
                previousWasWildcard = true;
                continue;
            }

            previousWasWildcard = false;
            normalizedMask.append(character);
        }

        hostname = normalizedMask;
    }

    while (hostname.endsWith('.')) {
        hostname.chop(1);
    }

    return hostname;
}

bool isWildcardSitePattern(const QString &hostname)
{
    return hostname.contains('*') && hasMeaningfulWildcardContent(hostname);
}

bool isXrayLikeWildcardSupported(const std::shared_ptr<Settings> &settings)
{
    const DockerContainer container = settings->defaultContainer(settings->defaultServerIndex());
    const Proto protocol = ContainerProps::defaultProtocol(container);
    return protocol == Proto::Xray || protocol == Proto::SSXray;
}
}

SitesController::SitesController(const std::shared_ptr<Settings> &settings, const QSharedPointer<SitesModel> &sitesModel, QObject *parent)
    : QObject(parent), m_settings(settings), m_sitesModel(sitesModel)
{
}

void SitesController::addSite(QString hostname)
{
    hostname = normalizeSiteInput(hostname);
    if (hostname.isEmpty()) {
        return;
    }

    const bool isPattern = isWildcardSitePattern(hostname);

    if (hostname.contains('*') && !isPattern) {
        emit errorOccurred(tr("Wildcard mask is empty or invalid"));
        return;
    }

    if (!hostname.contains(".") && !NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(hostname)) {
        if (!isPattern) {
            emit errorOccurred(tr("Hostname not look like ip adress or domain name"));
            return;
        }
    }

    if (isPattern) {
        if (!isXrayLikeWildcardSupported(m_settings)) {
            emit errorOccurred(tr("Wildcard masks with * are supported only with XRay/SSXray. For other protocols, use an exact host name or IP."));
            return;
        }

        m_sitesModel->addSite(hostname, "");
        emit finished(tr("New site added: %1").arg(hostname));
        return;
    }

    const auto &resolveCallback = [this](const QHostInfo &hostInfo) {
        for (const QHostAddress &addr : hostInfo.addresses()) {
            if (addr.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv4Protocol) {
                m_sitesModel->addSite(hostInfo.hostName(), addr.toString());
                break;
            }
        }
    };

    if (NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(hostname)) {
        m_sitesModel->addSite(hostname, "");
    } else {
        m_sitesModel->addSite(hostname, "");
        QHostInfo::lookupHost(hostname, this, resolveCallback);
    }

    emit finished(tr("New site added: %1").arg(hostname));
}

void SitesController::removeSite(int index)
{
    auto modelIndex = m_sitesModel->index(index);
    auto hostname = m_sitesModel->data(modelIndex, SitesModel::Roles::UrlRole).toString();
    m_sitesModel->removeSite(modelIndex);

    emit finished(tr("Site removed: %1").arg(hostname));
}

void SitesController::removeSites()
{
    m_sitesModel->removeSites();

    emit finished(tr("Site list cleared!"));
}

void SitesController::importSites(const QString &fileName, bool replaceExisting)
{
    QByteArray jsonData;
    if (!SystemController::readFile(fileName, jsonData)) {
        emit errorOccurred(tr("Can't open file: %1").arg(fileName));
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonData);
    if (jsonDocument.isNull()) {
        emit errorOccurred(tr("Failed to parse JSON data from file: %1").arg(fileName));
        return;
    }

    if (!jsonDocument.isArray()) {
        emit errorOccurred(tr("The JSON data is not an array in file: %1").arg(fileName));
        return;
    }

    auto jsonArray = jsonDocument.array();
    QVector<SitesModel::SiteEntry> sites;
    sites.reserve(jsonArray.size());

    const bool defaultUseVpn = m_sitesModel->getRouteMode() == Settings::VpnOnlyForwardSites;

    for (auto jsonValue : jsonArray) {
        auto jsonObject = jsonValue.toObject();
        auto hostname = normalizeSiteInput(jsonObject.value("hostname").toString(""));
        auto ip = jsonObject.value("ip").toString("");
        const bool useVpn = jsonObject.contains("useVpn")
                                ? jsonObject.value("useVpn").toBool(defaultUseVpn)
                                : defaultUseVpn;

        if (hostname.isEmpty()) {
            continue;
        }

        if (!hostname.contains(".") && !NetworkUtilities::ipAddressWithSubnetRegExp().exactMatch(hostname)) {
            if (!isWildcardSitePattern(hostname)) {
                qDebug() << hostname << " not look like ip adress or domain name";
                continue;
            }
        }

        if (hostname.contains('*') && !isWildcardSitePattern(hostname)) {
            continue;
        }

        if (isWildcardSitePattern(hostname)) {
            if (!isXrayLikeWildcardSupported(m_settings)) {
                qWarning() << "Skipping wildcard site import because current protocol is not XRay-like:" << hostname;
                continue;
            }
        }

        SitesModel::SiteEntry site;
        site.hostname = hostname;
        site.ip = ip;
        site.useVpn = useVpn;
        sites.append(site);
    }

    m_sitesModel->addSites(sites, replaceExisting);

    emit finished(tr("Import completed"));
}

void SitesController::exportSites(const QString &fileName)
{
    auto sites = m_sitesModel->getCurrentSites();

    QJsonArray jsonArray;

    for (const auto &site : sites) {
        QJsonObject jsonObject {
            { "hostname", site.hostname },
            { "ip", site.ip },
            { "useVpn", site.useVpn }
        };
        jsonArray.append(jsonObject);
    }

    QJsonDocument jsonDocument(jsonArray);
    QByteArray jsonData = jsonDocument.toJson();

    SystemController::saveFile(fileName, jsonData);

    emit finished(tr("Export completed"));
}

#ifndef XRAYCONFIGMODEL_H
#define XRAYCONFIGMODEL_H

#include <QAbstractListModel>
#include <QJsonObject>

#include "containers/containers_defs.h"

class XrayConfigModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        SiteRole,
        PortRole,
        // Advanced mKCP + FinalMask knobs. by vovankrot
        MaskTypeRole,
        PacketSizeRole,
        KcpMtuRole,
        KcpTtiRole,
        KcpUplinkCapacityRole,
        KcpDownlinkCapacityRole,
        KcpCongestionRole,
        KcpReadBufferSizeRole,
        KcpWriteBufferSizeRole
    };

    explicit XrayConfigModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

public slots:
    void updateModel(const QJsonObject &config);
    QJsonObject getConfig();

    // Restores the stock mKCP/FinalMask values (the ones this fork ships with).
    void resetAdvancedToDefaults();
    // True when any advanced value differs from stock -- drives the "modified" warning.
    bool isAdvancedModified() const;

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    DockerContainer m_container;
    QJsonObject m_protocolConfig;
    QJsonObject m_fullConfig;
};

#endif // XRAYCONFIGMODEL_H

pragma Singleton

import QtQuick 2.15

import SortFilterProxyModel 0.2

import ProtocolEnum 1.0

Item {
    ValueFilter {
        id: vpnTypeFilter
        roleName: "serviceType"
        value: ProtocolEnum.Vpn
    }

    ValueFilter {
        id: serviceTypeFilter
        roleName: "serviceType"
        value: ProtocolEnum.Other
    }

    ValueFilter {
        id: supportedFilter
        roleName: "isSupported"
        value: true
    }

    ValueFilter {
        id: installedFilter
        roleName: "isInstalled"
        value: true
    }

    ValueFilter {
        id: installationAllowedFilter
        roleName: "isInstallationAllowed"
        value: true
    }

    AllOf {
        id: supportedAndAllowedFilter
        ValueFilter {
            roleName: "isInstallationAllowed"
            value: true
        }
        ValueFilter {
            roleName: "isSupported"
            value: true
        }
    }

    AnyOf {
        id: showProtocolFilter
        filters: [ installedFilter, supportedAndAllowedFilter ]
    }

    function getWriteAccessProtocolsListFilters() {
        return [ vpnTypeFilter, showProtocolFilter ]
    }
    function getReadAccessProtocolsListFilters() {
        return [vpnTypeFilter, installedFilter]
    }

    function getWriteAccessServicesListFilters() {
        return [serviceTypeFilter]
    }
    function getReadAccessServicesListFilters() {
        return [serviceTypeFilter, installedFilter]
    }
}

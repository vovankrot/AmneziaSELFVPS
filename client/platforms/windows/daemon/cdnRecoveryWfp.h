#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fwpmu.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <QByteArray>
#include <QString>
#include "core/cdnRecoveryPolicy.h"

// Experimental control only. Never replace the shipping driver's baseline
// implicitly, and never edit filters belonging to the driver's WFP session.
namespace CdnRecoveryWfp {
inline constexpr GUID DriverLayer = {0x29b69bc2,0x64a2,0x4cc7,{0xa5,0x02,0x47,0xf9,0x9f,0x75,0x68,0x25}};
inline constexpr GUID RecoveryLayer = {0x24af6fab,0xa41d,0x40b7,{0x96,0x27,0xd6,0x0e,0x62,0xb4,0x69,0xb3}};
inline constexpr GUID ConnectFilter = {0x4207f127,0xcc80,0x477e,{0xad,0xdf,0x26,0xf7,0x65,0x85,0xe0,0x73}};
inline constexpr GUID AuthFilter = {0xd8602ff5,0x436b,0x414a,{0xa2,0x21,0x7b,0x4d,0xe8,0xce,0x96,0xc7}};
inline constexpr GUID ReceiveFilter = {0xfc3f8d71,0x33f7,0x4d24,{0x93,0x06,0xa3,0xde,0xe3,0xf7,0xc8,0x65}};
inline constexpr UINT16 DriverWeight = 1;
inline constexpr UINT16 RecoveryWeight = 2;

struct NativeApi {
    static inline QString layoutConflict;
    static inline UINT16 minimumProtectedWeight = 0xffff;
    static DWORD open(FWPM_SESSION0 *session, HANDLE *engine) {
        return FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, session, engine);
    }
    static void close(HANDLE engine) { FwpmEngineClose0(engine); }
    static DWORD begin(HANDLE engine) { return FwpmTransactionBegin0(engine, 0); }
    static DWORD commit(HANDLE engine) { return FwpmTransactionCommit0(engine); }
    static void abort(HANDLE engine) { FwpmTransactionAbort0(engine); }
    static DWORD layerAdd(HANDLE engine, FWPM_SUBLAYER0 *layer) {
        return FwpmSubLayerAdd0(engine, layer, nullptr);
    }
    static DWORD layerDelete(HANDLE engine, const GUID &key) { return FwpmSubLayerDeleteByKey0(engine, &key); }
    static DWORD filterAdd(HANDLE engine, FWPM_FILTER0 *filter, UINT64 *id) {
        return FwpmFilterAdd0(engine, filter, nullptr, id);
    }
    static DWORD filterDelete(HANDLE engine, UINT64 id) { return FwpmFilterDeleteById0(engine, id); }
    static DWORD layerWeight(HANDLE engine, const GUID &key, UINT16 &weight) {
        FWPM_SUBLAYER0 *layer = nullptr;
        const DWORD result = FwpmSubLayerGetByKey0(engine, &key, &layer);
        if (result == ERROR_SUCCESS) { weight = layer->weight; FwpmFreeMemory0(reinterpret_cast<void **>(&layer)); }
        return result;
    }
    static DWORD filterLayer(HANDLE engine, const GUID &key, GUID &layer) {
        FWPM_FILTER0 *filter = nullptr;
        const DWORD result = FwpmFilterGetByKey0(engine, &key, &filter);
        if (result == ERROR_SUCCESS) { layer = filter->subLayerKey; FwpmFreeMemory0(reinterpret_cast<void **>(&filter)); }
        return result;
    }
    static DWORD compatibleLayout(HANDLE engine) {
        layoutConflict.clear();
        minimumProtectedWeight = 0xffff;
        bool incompatible = false;
        // A hard permit must not precede another provider's policy. Refuse an
        // unknown/lower provider instead of silently overriding its decisions.
        for (const auto &layer : {FWPM_LAYER_ALE_CONNECT_REDIRECT_V4,
                                 FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4}) {
            FWPM_FILTER_ENUM_TEMPLATE0 query {};
            query.layerKey = layer;
            query.enumType = FWP_FILTER_ENUM_FULLY_CONTAINED;
            query.actionMask = 0xffffffffu;
            HANDLE enumeration = nullptr;
            DWORD error = FwpmFilterCreateEnumHandle0(engine, &query, &enumeration);
            if (error != ERROR_SUCCESS) return error;
            bool complete = false;
            while (!complete && error == ERROR_SUCCESS) {
                FWPM_FILTER0 **filters = nullptr;
                UINT32 count = 0;
                error = FwpmFilterEnum0(engine, enumeration, 64, &filters, &count);
                if (error != ERROR_SUCCESS) break;
                complete = count < 64;
                for (UINT32 i = 0; i < count; ++i) {
                    const auto &key = filters[i]->subLayerKey;
                    if (IsEqualGUID(key, DriverLayer) || IsEqualGUID(key, RecoveryLayer)) continue;
                    // Static permits and inspection-only callouts cannot deny
                    // this connection. Keep every blocking-capable policy ahead.
                    if (filters[i]->action.type == FWP_ACTION_PERMIT
                        || filters[i]->action.type == FWP_ACTION_CALLOUT_INSPECTION) continue;
                    UINT16 weight = 0;
                    error = layerWeight(engine, key, weight);
                    if (error == ERROR_SUCCESS && weight < minimumProtectedWeight) {
                        minimumProtectedWeight = weight;
                        layoutConflict = QString("Sublayer weight=%1, filter=%2, action=%3")
                            .arg(weight).arg(QString::fromWCharArray(filters[i]->displayData.name
                                ? filters[i]->displayData.name : L"unnamed")).arg(filters[i]->action.type);
                    }
                    if (error == ERROR_SUCCESS && weight <= RecoveryWeight) incompatible = true;
                    if (error != ERROR_SUCCESS) break;
                }
                if (filters) FwpmFreeMemory0(reinterpret_cast<void **>(&filters));
            }
            FwpmFilterDestroyEnumHandle0(engine, enumeration);
            if (error != ERROR_SUCCESS) return error;
        }
        return incompatible ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
    }
    static DWORD appId(const QString &path, QByteArray &id) {
        FWP_BYTE_BLOB *blob = nullptr;
        const DWORD result = FwpmGetAppIdFromFileName0(reinterpret_cast<const wchar_t *>(path.utf16()), &blob);
        if (result == ERROR_SUCCESS) {
            id = QByteArray(reinterpret_cast<const char *>(blob->data), int(blob->size));
            FwpmFreeMemory0(reinterpret_cast<void **>(&blob));
        }
        return result;
    }
    static bool tunnelRoute(UINT32 destination, UINT32 source, UINT32 interfaceIndex) {
        SOCKADDR_INET dest {}, chosenSource {};
        dest.Ipv4.sin_family = AF_INET;
        dest.Ipv4.sin_addr.s_addr = htonl(destination);
        MIB_IPFORWARD_ROW2 route {};
        if (GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &chosenSource) != NO_ERROR) return false;
        return route.InterfaceIndex == interfaceIndex && chosenSource.si_family == AF_INET
            && ntohl(chosenSource.Ipv4.sin_addr.s_addr) == source;
    }
};

// Called only by a separately compiled experimental service, before driver
// initialization. Existing sublayers/weights are never modified or deleted.
template<class Api = NativeApi> DWORD prepareDriverLayer() {
    HANDLE engine = nullptr;
    DWORD error = Api::open(nullptr, &engine);
    if (error != ERROR_SUCCESS) return error;
    error = Api::compatibleLayout(engine);
    if (error != ERROR_SUCCESS) { Api::close(engine); return error; }
    UINT16 weight = 0;
    error = Api::layerWeight(engine, DriverLayer, weight);
    if (error == ERROR_SUCCESS) {
        Api::close(engine);
        return weight == DriverWeight ? ERROR_SUCCESS : ERROR_INVALID_STATE;
    }
    if (error != FWP_E_SUBLAYER_NOT_FOUND) { Api::close(engine); return error; }
    error = Api::begin(engine);
    if (error != ERROR_SUCCESS) { Api::close(engine); return error; }
    FWPM_SUBLAYER0 layer {};
    layer.subLayerKey = DriverLayer;
    layer.weight = DriverWeight;
    layer.displayData.name = const_cast<wchar_t *>(L"SELFVPS experimental driver baseline");
    error = Api::layerAdd(engine, &layer);
    if (error == ERROR_SUCCESS) error = Api::commit(engine);
    if (error != ERROR_SUCCESS) Api::abort(engine);
    Api::close(engine);
    return error;
}

template<class Api = NativeApi> class Control {
public:
    ~Control() { if (m_engine) Api::close(m_engine); }
    Control() = default;
    Control(const Control &) = delete;
    Control &operator=(const Control &) = delete;

    DWORD providerLayout() {
        const DWORD error = open();
        return error == ERROR_SUCCESS ? Api::compatibleLayout(m_engine) : error;
    }

    DWORD preflight() {
        const DWORD opened = open();
        if (opened != ERROR_SUCCESS) return opened;
        UINT16 weight = 0;
        DWORD error = Api::layerWeight(m_engine, DriverLayer, weight);
        if (error != ERROR_SUCCESS || weight != DriverWeight) return ERROR_NOT_SUPPORTED;
        for (const auto &key : {ConnectFilter, AuthFilter, ReceiveFilter}) {
            GUID layer {};
            error = Api::filterLayer(m_engine, key, layer);
            if (error != ERROR_SUCCESS || !IsEqualGUID(layer, DriverLayer)) return ERROR_NOT_SUPPORTED;
        }
        return Api::compatibleLayout(m_engine);
    }

    DWORD install(const QString &app, const QHostAddress &target, const QHostAddress &tunSource, UINT32 tunIndex) {
        return installImpl(app, target, tunSource, tunIndex, false);
    }
    // Ask the real WFP engine to validate conditions/actions, then always abort.
    // This does not require moving the driver or applying traffic exceptions.
    DWORD validate(const QString &app, const QHostAddress &target, const QHostAddress &tunSource, UINT32 tunIndex) {
        return installImpl(app, target, tunSource, tunIndex, true);
    }
private:
    DWORD installImpl(const QString &app, const QHostAddress &target, const QHostAddress &tunSource,
                      UINT32 tunIndex, bool validateOnly) {
        if (m_installed) return ERROR_ALREADY_EXISTS;
        if (!CdnRecovery::publicIpv4(target) || tunSource.protocol() != QAbstractSocket::IPv4Protocol
            || tunSource.isNull() || !tunIndex || app.isEmpty()) return ERROR_INVALID_PARAMETER;
        DWORD error = validateOnly ? open() : preflight();
        if (error != ERROR_SUCCESS) return error;
        const UINT32 targetIp = target.toIPv4Address(), sourceIp = tunSource.toIPv4Address();
        if (!validateOnly && !Api::tunnelRoute(targetIp, sourceIp, tunIndex)) return ERROR_NOT_SUPPORTED;
        QByteArray appBytes;
        error = Api::appId(app, appBytes);
        if (error != ERROR_SUCCESS) return error;
        if (appBytes.isEmpty()) return ERROR_INVALID_DATA;
        error = Api::begin(m_engine);
        if (error != ERROR_SUCCESS) return error;
        FWPM_SUBLAYER0 layer {};
        layer.subLayerKey = RecoveryLayer;
        layer.weight = RecoveryWeight;
        layer.displayData.name = const_cast<wchar_t *>(L"SELFVPS scoped CDN control");
        error = Api::layerAdd(m_engine, &layer);
        UINT64 ids[3] {};
        const GUID layers[] = {FWPM_LAYER_ALE_CONNECT_REDIRECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V4,
                               FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4};
        for (int i = 0; error == ERROR_SUCCESS && i < 3; ++i) {
            FWP_BYTE_BLOB appBlob {ULONG(appBytes.size()), reinterpret_cast<UINT8 *>(appBytes.data())};
            FWPM_FILTER_CONDITION0 conditions[5] {};
            conditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
            conditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
            conditions[0].conditionValue.byteBlob = &appBlob;
            conditions[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
            conditions[1].conditionValue.type = FWP_UINT32;
            conditions[1].conditionValue.uint32 = targetIp;
            conditions[2].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
            conditions[2].conditionValue.type = FWP_UINT8;
            conditions[2].conditionValue.uint8 = IPPROTO_TCP;
            conditions[3].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
            conditions[3].conditionValue.type = FWP_UINT16;
            conditions[3].conditionValue.uint16 = 443;
            conditions[4].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
            conditions[4].conditionValue.type = FWP_UINT32;
            conditions[4].conditionValue.uint32 = sourceIp;
            for (auto &condition : conditions) condition.matchType = FWP_MATCH_EQUAL;
            FWPM_FILTER0 filter {};
            filter.displayData.name = const_cast<wchar_t *>(L"SELFVPS exact app/IP TCP443 control");
            filter.layerKey = layers[i];
            filter.subLayerKey = RecoveryLayer;
            filter.action.type = FWP_ACTION_PERMIT;
            filter.flags = FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT;
            filter.weight.type = FWP_UINT8;
            filter.weight.uint8 = 15;
            filter.numFilterConditions = i == 0 ? 4 : 5;
            filter.filterCondition = conditions;
            error = Api::filterAdd(m_engine, &filter, &ids[i]);
        }
        if (validateOnly) { Api::abort(m_engine); return error; }
        if (error == ERROR_SUCCESS) error = Api::commit(m_engine);
        if (error != ERROR_SUCCESS) { Api::abort(m_engine); return error; }
        for (int i = 0; i < 3; ++i) m_ids[i] = ids[i];
        m_installed = true;
        return ERROR_SUCCESS;
    }

public:
    DWORD remove() {
        if (!m_installed) return ERROR_SUCCESS;
        DWORD error = Api::begin(m_engine);
        if (error != ERROR_SUCCESS) return error;
        for (auto id : m_ids) {
            error = Api::filterDelete(m_engine, id);
            if (error != ERROR_SUCCESS && error != FWP_E_FILTER_NOT_FOUND) { Api::abort(m_engine); return error; }
        }
        error = Api::layerDelete(m_engine, RecoveryLayer);
        if (error != ERROR_SUCCESS && error != FWP_E_SUBLAYER_NOT_FOUND) { Api::abort(m_engine); return error; }
        error = Api::commit(m_engine);
        if (error != ERROR_SUCCESS) { Api::abort(m_engine); return error; }
        m_installed = false;
        return ERROR_SUCCESS;
    }
    bool installed() const { return m_installed; }
private:
    DWORD open() {
        if (m_engine) return ERROR_SUCCESS;
        FWPM_SESSION0 session {};
        session.flags = FWPM_SESSION_FLAG_DYNAMIC;
        return Api::open(&session, &m_engine);
    }
    HANDLE m_engine = nullptr;
    UINT64 m_ids[3] {};
    bool m_installed = false;
};
} // namespace CdnRecoveryWfp

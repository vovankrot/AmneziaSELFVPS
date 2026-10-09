#include "platforms/windows/daemon/cdnRecoveryWfp.h"
#include <QtTest>
#include <vector>

using namespace CdnRecoveryWfp;
struct FakeApi {
    struct Rule { GUID layer; UINT32 target, source; UINT16 port; UINT8 protocol;
                  QByteArray app; UINT32 flags, count; };
    static inline bool driverReady, routeReady, layoutReady, dynamic, recoveryLayer, backupLayer;
    static inline UINT16 weight;
    static inline int writes, failAt, aborts, closes;
    static inline std::vector<Rule> rules, backup;
    static void reset() {
        driverReady = routeReady = layoutReady = true;
        dynamic = recoveryLayer = backupLayer = false;
        weight = DriverWeight;
        writes = failAt = aborts = closes = 0;
        rules.clear(); backup.clear();
    }
    static DWORD mutation() { return ++writes == failAt ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; }
    static DWORD open(FWPM_SESSION0 *session, HANDLE *engine) {
        dynamic = session && (session->flags & FWPM_SESSION_FLAG_DYNAMIC);
        *engine = reinterpret_cast<HANDLE>(1); return ERROR_SUCCESS;
    }
    static void close(HANDLE) { ++closes; if (dynamic) { rules.clear(); recoveryLayer = false; } }
    static DWORD begin(HANDLE) { backup = rules; backupLayer = recoveryLayer; return ERROR_SUCCESS; }
    static DWORD commit(HANDLE) { return mutation(); }
    static void abort(HANDLE) { ++aborts; rules = backup; recoveryLayer = backupLayer; }
    static DWORD layerWeight(HANDLE, const GUID &, UINT16 &value) { value = weight; return ERROR_SUCCESS; }
    static DWORD filterLayer(HANDLE, const GUID &, GUID &layer) {
        layer = driverReady ? DriverLayer : RecoveryLayer; return ERROR_SUCCESS;
    }
    static DWORD compatibleLayout(HANDLE) { return layoutReady ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED; }
    static bool tunnelRoute(UINT32, UINT32, UINT32) { return routeReady; }
    static DWORD appId(const QString &, QByteArray &value) { value = QByteArray("exact-executable"); return ERROR_SUCCESS; }
    static DWORD layerAdd(HANDLE, FWPM_SUBLAYER0 *layer) {
        if (layer->weight != RecoveryWeight && layer->weight != DriverWeight) return ERROR_INVALID_DATA;
        const auto error = mutation();
        if (error == ERROR_SUCCESS) recoveryLayer = true;
        return error;
    }
    static DWORD filterAdd(HANDLE, FWPM_FILTER0 *filter, UINT64 *id) {
        const auto error = mutation();
        if (error != ERROR_SUCCESS) return error;
        if (filter->action.type != FWP_ACTION_PERMIT || !IsEqualGUID(filter->subLayerKey, RecoveryLayer)) return ERROR_INVALID_DATA;
        const auto *c = filter->filterCondition;
        if (!IsEqualGUID(c[0].fieldKey, FWPM_CONDITION_ALE_APP_ID)
            || !IsEqualGUID(c[1].fieldKey, FWPM_CONDITION_IP_REMOTE_ADDRESS)
            || !IsEqualGUID(c[2].fieldKey, FWPM_CONDITION_IP_PROTOCOL)
            || !IsEqualGUID(c[3].fieldKey, FWPM_CONDITION_IP_REMOTE_PORT)) return ERROR_INVALID_DATA;
        for (UINT32 i = 0; i < filter->numFilterConditions; ++i)
            if (c[i].matchType != FWP_MATCH_EQUAL) return ERROR_INVALID_DATA;
        const auto blob = c[0].conditionValue.byteBlob;
        rules.push_back({filter->layerKey, c[1].conditionValue.uint32,
            filter->numFilterConditions == 5 ? c[4].conditionValue.uint32 : 0,
            c[3].conditionValue.uint16, c[2].conditionValue.uint8,
            QByteArray(reinterpret_cast<char *>(blob->data), int(blob->size)), filter->flags, filter->numFilterConditions});
        *id = rules.size();
        return ERROR_SUCCESS;
    }
    static DWORD filterDelete(HANDLE, UINT64 id) {
        if (!id || id > 3) return ERROR_INVALID_PARAMETER;
        const auto error = mutation();
        if (error == ERROR_SUCCESS && !rules.empty()) rules.pop_back();
        return error;
    }
    static DWORD layerDelete(HANDLE, const GUID &key) {
        if (!IsEqualGUID(key, RecoveryLayer)) return ERROR_INVALID_PARAMETER;
        const auto error = mutation();
        if (error == ERROR_SUCCESS) recoveryLayer = false;
        return error;
    }
};

class Tests : public QObject {
    Q_OBJECT
    using Backend = Control<FakeApi>;
    static DWORD install(Backend &backend, QString target = "104.16.57.21") {
        return backend.install("C:/Test/test.exe", QHostAddress(target), QHostAddress("10.33.0.2"), 28);
    }
private slots:
    void init() { FakeApi::reset(); }
    void shippingOrUnexpectedDriverBaselineRefusesWithoutWrites() {
        Backend backend;
        FakeApi::driverReady = false;
        QCOMPARE(install(backend), DWORD(ERROR_NOT_SUPPORTED));
        QCOMPARE(FakeApi::writes, 0);
        FakeApi::driverReady = true;
        FakeApi::weight = 0xffff;
        QCOMPARE(install(backend), DWORD(ERROR_NOT_SUPPORTED));
        QCOMPARE(FakeApi::writes, 0);
    }
    void conflictingProviderLayoutRefusesBeforeFilterChanges() {
        Backend backend;
        FakeApi::layoutReady = false;
        QCOMPARE(install(backend), DWORD(ERROR_NOT_SUPPORTED));
        QCOMPARE(FakeApi::writes, 0);
        QCOMPARE(prepareDriverLayer<FakeApi>(), DWORD(ERROR_NOT_SUPPORTED));
        QCOMPARE(FakeApi::writes, 0);
    }
    void installsOnlyExactAppIpTcp443AndTunnelAuth() {
        Backend backend;
        QCOMPARE(install(backend), DWORD(ERROR_SUCCESS));
        QVERIFY(backend.installed());
        QCOMPARE(FakeApi::rules.size(), size_t(3));
        QVERIFY(IsEqualGUID(FakeApi::rules[0].layer, FWPM_LAYER_ALE_CONNECT_REDIRECT_V4));
        QVERIFY(IsEqualGUID(FakeApi::rules[1].layer, FWPM_LAYER_ALE_AUTH_CONNECT_V4));
        QVERIFY(IsEqualGUID(FakeApi::rules[2].layer, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4));
        for (size_t i = 0; i < FakeApi::rules.size(); ++i) {
            const auto &rule = FakeApi::rules[i];
            QCOMPARE(rule.target, QHostAddress("104.16.57.21").toIPv4Address());
            QCOMPARE(rule.app, QByteArray("exact-executable"));
            QCOMPARE(rule.port, UINT16(443));
            QCOMPARE(rule.protocol, UINT8(IPPROTO_TCP));
            QVERIFY(rule.flags & FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT);
            QCOMPARE(rule.count, UINT32(i == 0 ? 4 : 5));
            if (i) QCOMPARE(rule.source, QHostAddress("10.33.0.2").toIPv4Address());
        }
        QCOMPARE(backend.remove(), DWORD(ERROR_SUCCESS));
        QVERIFY(!backend.installed());
        QVERIFY(FakeApi::rules.empty());
        QVERIFY(!FakeApi::recoveryLayer);
        QCOMPARE(install(backend), DWORD(ERROR_SUCCESS));
    }
    void everyInstallationMutationFailureRollsBack() {
        for (int failure = 1; failure <= 5; ++failure) {
            FakeApi::reset();
            Backend backend;
            FakeApi::failAt = failure;
            QCOMPARE(install(backend), DWORD(ERROR_ACCESS_DENIED));
            QVERIFY(!backend.installed());
            QVERIFY(FakeApi::rules.empty());
            QVERIFY(!FakeApi::recoveryLayer);
            QCOMPARE(FakeApi::aborts, 1);
        }
    }
    void failedRemovalRetainsOwnershipAndCanRetry() {
        for (int failure = 1; failure <= 5; ++failure) {
            FakeApi::reset();
            Backend backend;
            QCOMPARE(install(backend), DWORD(ERROR_SUCCESS));
            FakeApi::failAt = FakeApi::writes + failure;
            QCOMPARE(backend.remove(), DWORD(ERROR_ACCESS_DENIED));
            QVERIFY(backend.installed());
            QCOMPARE(FakeApi::rules.size(), size_t(3));
            QVERIFY(FakeApi::recoveryLayer);
            FakeApi::failAt = 0;
            QCOMPARE(backend.remove(), DWORD(ERROR_SUCCESS));
            QVERIFY(!backend.installed());
        }
    }
    void privateReservedIpv6AndWrongRouteAreRejectedBeforeWrites() {
        Backend backend;
        for (const auto &ip : {"127.0.0.1", "192.168.0.1", "10.0.0.1", "100.64.0.1", "169.254.1.1",
                               "0.0.0.0", "224.0.0.1", "192.0.2.1", "::1", "2001:db8::1"}) {
            QCOMPARE(install(backend, ip), DWORD(ERROR_INVALID_PARAMETER));
            QCOMPARE(FakeApi::writes, 0);
        }
        FakeApi::routeReady = false;
        QCOMPARE(install(backend), DWORD(ERROR_NOT_SUPPORTED));
        QCOMPARE(FakeApi::writes, 0);
    }
    void duplicateInstallDoesNotAddFiltersAndDynamicCloseCleansOwnObjects() {
        {
            Backend backend;
            QCOMPARE(install(backend), DWORD(ERROR_SUCCESS));
            const auto writes = FakeApi::writes;
            QCOMPARE(install(backend), DWORD(ERROR_ALREADY_EXISTS));
            QCOMPARE(FakeApi::writes, writes);
        }
        QCOMPARE(FakeApi::closes, 1);
        QVERIFY(FakeApi::rules.empty());
        QVERIFY(!FakeApi::recoveryLayer);
    }
    void nativeSchemaValidationNeverCommitsOrMovesTheDriver() {
        Backend backend;
        FakeApi::driverReady = false;
        FakeApi::routeReady = false;
        QCOMPARE(backend.validate("C:/Test/test.exe", QHostAddress("104.16.57.21"),
                                 QHostAddress("10.33.0.2"), 28), DWORD(ERROR_SUCCESS));
        QVERIFY(!backend.installed());
        QCOMPARE(FakeApi::writes, 4); // one layer, three filters, no commit
        QCOMPARE(FakeApi::aborts, 1);
        QVERIFY(FakeApi::rules.empty());
        QVERIFY(!FakeApi::recoveryLayer);
    }
};
QTEST_GUILESS_MAIN(Tests)
#include "tests.moc"

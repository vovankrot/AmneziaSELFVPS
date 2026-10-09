#include "platforms/windows/daemon/cdnRecoveryWfp.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTextStream output(stdout);
    CdnRecoveryWfp::Control<> control;
    const auto args = app.arguments();
    if (args.size() == 2 && args[1] == "--cleanup-baseline") {
        HANDLE engine = nullptr;
        DWORD result = CdnRecoveryWfp::NativeApi::open(nullptr, &engine);
        if (result != ERROR_SUCCESS) return 2;
        // Delete only our empty experimental sublayer. WFP refuses deletion
        // while any filters still use it; never delete the driver's filters.
        result = CdnRecoveryWfp::NativeApi::layerDelete(engine, CdnRecoveryWfp::DriverLayer);
        CdnRecoveryWfp::NativeApi::close(engine);
        output << "Experimental baseline cleanup Windows status: " << result << Qt::endl;
        return result == ERROR_SUCCESS || result == FWP_E_SUBLAYER_NOT_FOUND ? 0 : 3;
    }
    if (args.size() == 2 && args[1] == "--layout") {
        const auto result = control.providerLayout();
        output << "Provider layout Windows status: " << result << "; no filters changed." << Qt::endl;
        if (!CdnRecoveryWfp::NativeApi::layoutConflict.isEmpty())
            output << CdnRecoveryWfp::NativeApi::layoutConflict << Qt::endl;
        output << "Minimum protected sublayer weight: " << CdnRecoveryWfp::NativeApi::minimumProtectedWeight << Qt::endl;
        return result == ERROR_SUCCESS ? 0 : 2;
    }
    if (args.size() == 6 && args[1] == "--validate") {
        const QFileInfo exe(args[2]);
        bool valid = false;
        const auto index = args[5].toUInt(&valid);
        if (!exe.isAbsolute() || !exe.isFile() || !valid || !index) return 2;
        const auto result = control.validate(exe.canonicalFilePath(), QHostAddress(args[3]), QHostAddress(args[4]), index);
        output << "WFP validation status: " << result << "; transaction aborted, no filters applied." << Qt::endl;
        return result == ERROR_SUCCESS ? 0 : 3;
    }
    const DWORD ready = control.preflight();
    output << "Preflight Windows status: " << ready << Qt::endl;
    if (ready != ERROR_SUCCESS) {
        output << "No filters changed. Requires the separately compiled experimental service baseline." << Qt::endl;
        return 2;
    }
    if (args.size() == 2 && args[1] == "--preflight") return 0;
    if (args.size() != 6 || args[1] != "--apply") {
        output << "Usage: --preflight | --validate/--apply <exe-path> <public-IPv4> <TUN-IPv4> <TUN-index>" << Qt::endl;
        return 2;
    }
    const QFileInfo exe(args[2]);
    bool indexValid = false;
    const auto index = args[5].toUInt(&indexValid);
    if (!exe.isAbsolute() || !exe.isFile() || !indexValid || !index) return 2;
    const auto result = control.install(exe.canonicalFilePath(), QHostAddress(args[3]), QHostAddress(args[4]), index);
    output << "Install Windows status: " << result << Qt::endl;
    if (result != ERROR_SUCCESS) return 3;
    output << "Control active for 60 seconds: one executable, one IPv4, TCP/443. Existing flows are unchanged." << Qt::endl;
    QTimer::singleShot(60000, &app, [&] {
        const auto removed = control.remove();
        output << "Remove Windows status: " << removed << Qt::endl;
        // Dynamic-session close on return also removes owned objects if an
        // explicit delete failed. It cannot delete another provider's objects.
        app.exit(removed == ERROR_SUCCESS ? 0 : 4);
    });
    return app.exec();
}

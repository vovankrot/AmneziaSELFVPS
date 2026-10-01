# QML regression checks

Loads the production QML files with test controllers, using Qt Basic (the
application's style). Does not start the VPN or read the user's configuration.

Checks the ConnectButton hit area/background and actual pointer dispatch once
per click in both connection states. Loads PageSettings, PageHome and PageShare;
checks desktop layouts at 750 and 650 pixel content widths; clicks the production
top-navigation, connection-details and add-client controls; renders
navigation-runtime.png, settings-runtime.png, home-runtime.png and
share-runtime.png; and fails on QML ReferenceError, TypeError or binding loops.

Build with Qt 6.8.3 MSVC:

    cmake -G "Visual Studio 17 2022" -A x64 -S tests/ui-regressions -B build-installer/ui-regressions-msvc -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build build-installer/ui-regressions-msvc --config Release

Run Release/ui-regressions.exe from its build directory with the matching Qt
bin on PATH, QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=software and
QT_FORCE_STDERR_LOGGING=1.

This proves QML geometry and event dispatch, not real VPN disconnection or a
complete end-to-end application flow.

The production SSH trust dialog is also opened above the modal busy popup. Real
mouse clicks test Yes and No after reopening the loader (a late loading signal),
and verify that the pending loading state remains active after the decision.
The test saves ssh-trust-runtime.png. Controllers are mocks; no real server key
is trusted and the user's SSH pins/settings are not read or modified.

Russian and English catalogs are compiled from the committed TS files by this
test target. The test verifies translated client/SSH labels and retranslation
of an open dialog when switching languages in both directions.

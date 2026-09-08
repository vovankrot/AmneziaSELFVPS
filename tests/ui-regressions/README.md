# QML regression checks

Loads the production QML files with test controllers, using Qt Basic (the
application's style). Does not start the VPN or read the user's configuration.

Checks the ConnectButton hit area/background and actual pointer dispatch once
per click in both connection states. Loads PageSettings, checks its right panel
at 750 and 650 pixel content widths, renders settings-runtime.png, and fails on
QML ReferenceError, TypeError or binding loops.

Build with Qt 6.8.3 MSVC:

    cmake -G "Visual Studio 17 2022" -A x64 -S tests/ui-regressions -B build-installer/ui-regressions-msvc -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build build-installer/ui-regressions-msvc --config Release

Run Release/ui-regressions.exe from its build directory with the matching Qt
bin on PATH, QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=software and
QT_FORCE_STDERR_LOGGING=1.

This proves QML geometry and event dispatch, not real VPN disconnection or a
complete end-to-end application flow.

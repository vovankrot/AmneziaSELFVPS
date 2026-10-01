# Updater regressions

Build with Qt 6.8.3 MSVC and Visual Studio 2022:

    cmake -S tests/update-regressions -B build-installer/update-regressions -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build build-installer/update-regressions --config Release
    build-installer/update-regressions/Release/update-regressions.exe -o update-results.txt,txt

The production updater runs against injected QNetworkReply instances. Tests cover
release selection, hash/size validation, cancellation, preservation of a previous
download, altered installer refusal before UAC, single-flight checks and disabling
automatic polling. Settings/files stay in disposable directories. No real
installer or service is launched. Address-selection tests exercise the production
split-tunnel helper; actual WFP/driver routing still needs a VM test.

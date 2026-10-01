# Client refresh regressions

CMake extracts the exact `ExportController::updateClientManagementModel` implementation at configure time. Controlled model/SSH doubles exercise its production QtConcurrent/QPromise orchestration without accessing saved credentials or a real VPS.

Checks cover early client-name publication before runtime statistics, GUI event-loop responsiveness, 30-second cache reuse, forced refresh, invalidation, authentication and protocol isolation, serialized requests with the latest selection, failure recovery and controller destruction during an outstanding worker.

```powershell
cmake -S tests/client-refresh-regressions -B build-installer/client-refresh-regressions-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-installer/client-refresh-regressions-msvc --config Release
```

Run `Release/client-refresh-regressions.exe` with the Qt bin directory on PATH. No actual SSH latency or installed application behavior is proven by this test.

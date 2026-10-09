# Transport regressions

Build with Visual Studio 2022 and Qt 6.8.3 (Core, RemoteObjects, Test):

```powershell
cmake -S tests/transport-regressions -B build/transport -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build/transport --config Release
build/transport/Release/transport-regressions.exe -o transport-results.txt,txt
```

The production asynchronous output reader runs against an actual local Qt
Remote Objects host with a minimal output-source fixture. Tests exercise split
startup lines, repeated readiness, continued draining after connection, bursts
of process output, event-loop progress, shutdown guards and reader destruction.
Parser bounds and credential redaction are also covered by security-regressions.

Additional cases cover serial routing, deadlines and late replies, exclusion
failures before catch-all routes, application split routing and IPv6 handling.
Default-route selection tests include combined metrics and connected LAN routes.
Local HTTPS tests create an ephemeral certificate and exercise certificate trust,
HTTP response validation, total time budgets and target validation. No private
test key is saved. The read-only route-probe prints the selected interface index.

Startup cases cover service/process replica discovery, one total request deadline,
cancellation before late discovery, failed creation, and cancelable background
SOCKS probing. The process observer reports an unexpected zero-code exit or failed
startup exactly once while ignoring controlled shutdown notifications.

These tests do not launch tun2socks or change the installed VPN, network routes,
driver, application settings or VPS. They cannot prove that a browser video
timeout or Riot authentication failure is resolved.

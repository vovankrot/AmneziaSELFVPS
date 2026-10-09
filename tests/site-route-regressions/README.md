# Site route lifetime regressions

CMake extracts the production `VpnConnection::addSitesRoutes` method. A controlled DNS fixture delivers responses at specific session boundaries; route writes use an actual local Qt Remote Objects host. Settings are in memory and no OS network route is changed.

Cases cover a response after disconnection, generation change, protocol replacement, cancellation between DNS completion and the first queued IPC request, connection destruction while its old protocol remains alive, valid routing and persistence, and rejected route writes.

The suite also extracts the production service signal registration block and
checks that 20 session registrations still deliver one handler call per network
change or wakeup, rather than accumulating duplicate reconnect requests.

Build with Visual Studio 2022, Qt 6.8.3 Core/Network/RemoteObjects/Test and CMake. Run the Release executable with `-o results.txt,txt`.

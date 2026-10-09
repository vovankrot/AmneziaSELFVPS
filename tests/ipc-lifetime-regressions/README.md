# IPC lifetime regressions

Build with CMake, Visual Studio 2022 and Qt 6.8.3 Core/Network/RemoteObjects/Test. Run the Release executable with `-o results.txt,txt`.

Tests extract the production process creation and release methods and use real local pipes with a fake process and authentication fixture. The cleanup delay is shortened from 30 seconds to 40 milliseconds for testing. No elevated process is launched.

Cases cover explicit closure, disconnected peers, preservation of running processes and connected idle endpoints, abandoned endpoint cleanup, listener failure and destruction of shared objects without reference cycles. Authentication is independently covered by security-regressions.

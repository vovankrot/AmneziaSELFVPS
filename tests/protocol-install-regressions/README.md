# Protocol installation probes

Native Qt test validates input rejection, shell quoting, expected interfaces,
process names, and both IPv4/IPv6 socket tables in the production helper.
The executable accepts `container kind port` to emit the exact production shell
probe for isolated Linux Docker tests. Kind values follow the helper's enum.

Build with CMake and Qt 6, then run CTest with Qt's bin directory on PATH.
No network changes are performed by this native test.

Live VPS validation on 2026-10-04 used uniquely labelled temporary containers,
without public ports, host networking, privileged mode, or host module mounts.
It checked the actual AWG, XRay mKCP, and Hysteria2 configuration/startup scripts,
invalid AWG key rejection, wrong-port/idle/dead-process rejection, and retention
of the previous container's ID, writable files and working configuration.
Repeat Hysteria2 configuration also checks unchanged credential/certificate
hashes. All test containers are removed in finally; production container IDs
and start times must match their initial snapshot.

This validates server startup, not a Windows client handshake or throughput.
AnyTLS has no image on this VPS and was not tested live.

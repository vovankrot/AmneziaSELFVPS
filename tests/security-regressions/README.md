# Security regressions

Build with Visual Studio 2022 and the same Qt 6.8.3 MSVC distribution as the
Windows release. Requires Qt Core, Core5Compat, Network, Qml, Test and the
repository's tracked OpenSSL library.

```
cmake -S tests/security-regressions -B build/security -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build/security --config Release
build/security/Release/security-regressions.exe -o security-results.txt,txt
```

Tests use production implementations for SSH trust, named-pipe peer identity,
CertUtil argument filtering, list normalization/atomic writes, peer statistics,
configuration revision/capability checks, AES-GCM, SOCKS reply parsing,
bounded tun2socks output parsing and proxy credential redaction, and
the complete snapshot manager. Snapshot remote operations and settings access
are isolated test doubles. Failures cover required reads, missing XHTTP paths,
encryption unavailable, stage upload, swap, restart and recovery, corruption
and path traversal. No actual service, driver, SSH/VPS, application settings
or installed client is changed.

The tests confirm local control flow and generated shell commands; a VM/VPS
test is still needed to prove the real container restart, socket availability,
permissions, packet routing and kernel driver behavior. Named-pipe tests do
create actual local pipes and child processes with trusted/untrusted executable
paths. Use `-o <file>,txt` to preserve QtTest results even without a console.
# Stage 20 additions

The production DNS model method is compiled with in-memory settings. Cases cover
server index selection, imported private DNS with the switch off/on, installed
AmneziaDNS, custom resolver selection and invalid/duplicate secondary DNS.
SSH policy tests cover host/user/port/password identity and bounded UTF-8
passphrase copying. The exact Windows autostart command is tested with a path
containing spaces; no registry entry is changed.

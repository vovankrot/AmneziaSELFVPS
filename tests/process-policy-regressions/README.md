# Windows process policy regressions

Tests compile the production process termination, start and finished-state methods. Two copies of a harmless test executable are launched from temporary folders. Terminating one by its expected path must leave the other running. Only these test processes are started or terminated; installed VPN processes are untouched.

Additional cases cover missing paths, asynchronous startup, duplicate startup without killing the current child, failed startup and acknowledgement of a child that has already exited.

Build with Visual Studio 2022, Qt 6.8.3 Core/Test and CMake. Run the Release executable with `-o results.txt,txt` and the Qt bin folder on PATH.

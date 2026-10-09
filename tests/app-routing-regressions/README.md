# Application routing regressions

Build with CMake, Visual Studio 2022 and Qt 6.8.3 Core/Test. Run the Release executable with `-o results.txt,txt`.

Tests use the production folder scanner and application model with in-memory settings. They cover folder and executable bounds, mixed-case extensions, batch persistence, duplicates, group changes and removal. No installed application settings are written.

The UI regression suite separately checks the actual confirmation drawer and its buttons at small window sizes.

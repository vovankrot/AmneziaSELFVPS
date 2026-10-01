cmake_minimum_required(VERSION 3.25)
project(SecurityRegressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Core5Compat Network Qml Test)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(security-regressions main.cpp
    ${REPO}/client/core/defs.h
    ${REPO}/client/core/sshHostTrust.h ${REPO}/client/core/sshHostTrust.cpp
    ${REPO}/client/core/controllers/configSnapshotManager.cpp ${REPO}/client/core/controllers/configSnapshotManager.h)
target_include_directories(security-regressions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO} ${REPO}/client
    ${REPO}/client/3rd-prebuilt/3rd-prebuilt/openssl/windows/include)
target_link_libraries(security-regressions PRIVATE Qt6::Core Qt6::Core5Compat Qt6::Network Qt6::Qml Qt6::Test
    ${REPO}/client/3rd-prebuilt/3rd-prebuilt/openssl/windows/win64/libcrypto.lib
    ws2_32 crypt32 advapi32 user32)
enable_testing()
target_compile_definitions(security-regressions PRIVATE SELFVPS_SNAPSHOT_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/snapshot-fixtures")
add_test(NAME security-regressions COMMAND security-regressions)

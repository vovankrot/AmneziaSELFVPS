cmake_minimum_required(VERSION 3.25)
project(protocol_install_regressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core)
enable_testing()
add_executable(protocol-install-regressions main.cpp)
target_include_directories(protocol-install-regressions PRIVATE ../..)
target_link_libraries(protocol-install-regressions PRIVATE Qt6::Core)
add_custom_command(TARGET protocol-install-regressions POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Core> $<TARGET_FILE_DIR:protocol-install-regressions>)
add_test(NAME protocol-install-probes COMMAND protocol-install-regressions)

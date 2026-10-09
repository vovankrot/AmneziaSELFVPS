cmake_minimum_required(VERSION 3.25)
project(service_stop_regressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()
add_executable(service-stop-regressions main.cpp)
target_include_directories(service-stop-regressions PRIVATE ../..)
add_test(NAME stop-watchdog-independent-of-event-loop COMMAND service-stop-regressions)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
add_executable(service-ipc-health-regressions ipc-health.cpp)
target_include_directories(service-ipc-health-regressions PRIVATE ../..)
target_link_libraries(service-ipc-health-regressions PRIVATE Qt6::Core Qt6::Network)
add_custom_command(TARGET service-ipc-health-regressions POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Core> $<TARGET_FILE:Qt6::Network>
        $<TARGET_FILE_DIR:service-ipc-health-regressions>)
add_test(NAME service-health-requires-responsive-ipc COMMAND service-ipc-health-regressions)

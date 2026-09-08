cmake_minimum_required(VERSION 3.25)
project(UiRegressions LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Quick Test QuickControls2)
qt_add_executable(ui-regressions main.cpp)
file(GLOB UI_ICONS ../../client/images/controls/*.svg)
qt_add_resources(ui-regressions app-resources PREFIX "/" BASE ../../client FILES
    ${UI_ICONS} ../../client/fonts/Inter.ttf)
target_link_libraries(ui-regressions PRIVATE Qt6::Quick Qt6::Test Qt6::QuickControls2)
target_compile_definitions(ui-regressions PRIVATE SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../client/ui/qml")

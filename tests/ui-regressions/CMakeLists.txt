cmake_minimum_required(VERSION 3.25)
project(UiRegressions LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Quick Test QuickControls2 LinguistTools)
qt_add_executable(ui-regressions main.cpp)
add_subdirectory(../../client/3rd/SortFilterProxyModel sortfilter)
target_link_libraries(ui-regressions PRIVATE SortFilterProxyModel)
file(GLOB UI_ICONS ../../client/images/controls/*.svg ../../client/images/controls/*.png)
qt_add_resources(ui-regressions app-resources PREFIX "/" BASE ../../client FILES
    ${UI_ICONS} ../../client/fonts/Inter.ttf)
target_link_libraries(ui-regressions PRIVATE Qt6::Quick Qt6::Test Qt6::QuickControls2)
target_compile_definitions(ui-regressions PRIVATE SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../client/ui/qml")
qt_add_lrelease(TS_FILES
    ../../client/translations/amneziavpn_ru_RU.ts
    ../../client/translations/amneziavpn_en_US.ts
    QM_FILES_OUTPUT_VARIABLE TEST_QM_FILES)
add_custom_target(ui-test-translations DEPENDS ${TEST_QM_FILES})
add_dependencies(ui-regressions ui-test-translations)
target_compile_definitions(ui-regressions PRIVATE TRANSLATION_DIR="${CMAKE_CURRENT_BINARY_DIR}")

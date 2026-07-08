cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)

set(PROJECT AmneziaVPN)
set(AMNEZIAVPN_VERSION 4.8.15.0)

project(${PROJECT} VERSION ${AMNEZIAVPN_VERSION}
        DESCRIPTION "AmneziaVPN"
        HOMEPAGE_URL "https://amnezia.org/"
)

string(TIMESTAMP CURRENT_DATE "%Y-%m-%d")
set(RELEASE_DATE "${CURRENT_DATE}")

set(APP_MAJOR_VERSION ${CMAKE_PROJECT_VERSION_MAJOR}.${CMAKE_PROJECT_VERSION_MINOR}.${CMAKE_PROJECT_VERSION_PATCH})
math(EXPR APP_ANDROID_VERSION_CODE
    "${CMAKE_PROJECT_VERSION_MAJOR} * 1000000 + ${CMAKE_PROJECT_VERSION_MINOR} * 10000 + ${CMAKE_PROJECT_VERSION_PATCH} * 100 + ${CMAKE_PROJECT_VERSION_TWEAK}")

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(MZ_PLATFORM_NAME "linux")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(MZ_PLATFORM_NAME "windows")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(MZ_PLATFORM_NAME "macos")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    set(MZ_PLATFORM_NAME "android")
endif()

set(QT_BUILD_TOOLS_WHEN_CROSS_COMPILING ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Silence LNK4099 for the prebuilt OpenSSL static libs: they reference
# ossl_static.pdb which is not shipped with the 3rd-prebuilt package, so the
# MSVC linker floods the build log with harmless "PDB not found" warnings.
if(MSVC)
    add_link_options("/ignore:4099")
endif()

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64")
endif()

add_subdirectory(client)

add_subdirectory(service)

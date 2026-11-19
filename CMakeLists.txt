cmake_minimum_required(VERSION 3.21)
project(xsync VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(XSYNC_BUILD_TESTS "Build xsync unit and stability tests" ON)

# ---------------------------------------------------------------------------
# Qt 6 (Core, Gui, Widgets, Network, Svg)
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Svg)
if(XSYNC_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

# ---------------------------------------------------------------------------
# OpenSSL + ZLIB + libssh2 (fetched & built statically so modern ED25519/ECDSA
# and OpenSSH private keys work identically on Windows & Linux)
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(ZLIB QUIET)

include(FetchContent)

if(NOT ZLIB_FOUND)
    message(STATUS "System ZLIB not found; fetching zlib 1.3.1 via FetchContent")
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(zlib)
    if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
        set(ZLIB_LIBRARIES zlibstatic CACHE STRING "" FORCE)
        set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
    endif()
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND "OpenSSL" CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION ON CACHE BOOL "" FORCE)
set(CLEAR_MEMORY ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    libssh2
    URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libssh2)

# Restore BUILD_SHARED_LIBS default
set(LIBSSH2_TARGET libssh2_static)
if(NOT TARGET libssh2_static)
    set(LIBSSH2_TARGET libssh2)
endif()

# ---------------------------------------------------------------------------
# Core Library (no GUI dependency beyond Qt6::Core / Qt6::Network)
# ---------------------------------------------------------------------------
add_library(xsync_core STATIC
    src/core/Types.h
    src/core/Types.cpp
    src/core/SecretStore.h
    src/core/SecretStore.cpp
    src/core/SessionStore.h
    src/core/SessionStore.cpp
    src/core/MemoryStats.h
    src/core/MemoryStats.cpp
    src/core/ShellUtils.h
    src/core/ShellUtils.cpp
    src/core/SshClient.h
    src/core/SshClient.cpp
    src/core/ProtocolNegotiator.h
    src/core/ProtocolNegotiator.cpp
    src/core/FileSystemBackend.h
    src/core/FileSystemBackend.cpp
    src/core/TransferWorker.h
    src/core/TransferWorker.cpp
    src/core/TransferManager.h
    src/core/TransferManager.cpp
)

target_include_directories(xsync_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${libssh2_SOURCE_DIR}/include
)

target_link_libraries(xsync_core PUBLIC
    Qt6::Core
    Qt6::Network
    ${LIBSSH2_TARGET}
    OpenSSL::Crypto
    OpenSSL::SSL
)

if(WIN32)
    target_link_libraries(xsync_core PUBLIC ws2_32 crypt32 psapi bcrypt gdi32 advapi32 user32)
    target_compile_definitions(xsync_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    target_compile_options(xsync_core PRIVATE /W3 /utf-8)
else()
    target_compile_options(xsync_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# UI Library (separated so tests can also test FileListModel / MIME / GUI)
# ---------------------------------------------------------------------------
add_library(xsync_ui STATIC
    src/ui/IconCache.h
    src/ui/IconCache.cpp
    src/ui/FileListModel.h
    src/ui/FileListModel.cpp
    src/ui/FileTableView.h
    src/ui/FileTableView.cpp
    src/ui/FilePane.h
    src/ui/FilePane.cpp
    src/ui/PaneTabWidget.h
    src/ui/PaneTabWidget.cpp
    src/ui/SessionEditDialog.h
    src/ui/SessionEditDialog.cpp
    src/ui/SessionManagerDialog.h
    src/ui/SessionManagerDialog.cpp
    src/ui/TransferQueueWidget.h
    src/ui/TransferQueueWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    resources/xsync.qrc
)
set_property(SOURCE resources/xsync.qrc PROPERTY AUTORCC_OPTIONS "--no-zstd")

target_include_directories(xsync_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xsync_ui PUBLIC
    xsync_core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
)

if(MSVC)
    target_compile_options(xsync_ui PRIVATE /W3 /utf-8)
else()
    target_compile_options(xsync_ui PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Main Executable
# ---------------------------------------------------------------------------
set(XSYNC_MAIN_SOURCES src/main.cpp)
if(WIN32)
    list(APPEND XSYNC_MAIN_SOURCES resources/app.rc)
endif()

add_executable(xsync WIN32 ${XSYNC_MAIN_SOURCES})
target_include_directories(xsync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_link_libraries(xsync PRIVATE xsync_ui)

if(MSVC)
    target_compile_options(xsync PRIVATE /W3 /utf-8)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(XSYNC_BUILD_TESTS)
    enable_testing()

    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE xsync_ui Qt6::Test)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_memory_stability tests/test_memory_stability.cpp)
    target_link_libraries(test_memory_stability PRIVATE xsync_ui Qt6::Test)
    add_test(NAME test_memory_stability COMMAND test_memory_stability)

    add_executable(test_transfer_local tests/test_transfer_local.cpp)
    target_link_libraries(test_transfer_local PRIVATE xsync_ui Qt6::Test)
    add_test(NAME test_transfer_local COMMAND test_transfer_local)
endif()

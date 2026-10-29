cmake_minimum_required(VERSION 3.20)
project(holesail-cpp VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOLESAIL_BUILD_TESTS "Build the test suite" ON)

# Two modes, mirroring hyperdht-cpp's own CMakeLists:
#  1. Desktop POSIX — pkg-config finds everything (the nix devShell path).
#  2. Cross-compile / Android — no pkg-config exists for the target, so every
#     archive is passed in explicitly. Triggered by -DSODIUM_LIBRARY=..., the
#     same switch hyperdht-cpp uses, so one recipe drives both projects.
#
# Mode 2 expects, all as absolute paths:
#   SODIUM_INCLUDE_DIR / SODIUM_LIBRARY
#   UV_INCLUDE_DIR     / UV_LIBRARY
#   HYPERDHT_INCLUDE_DIR (the dir holding hyperdht/hyperdht.h)
#   HYPERDHT_LIBRARY   / UDX_LIBRARY
if(NOT CMAKE_DISABLE_FIND_PACKAGE_PkgConfig AND NOT SODIUM_LIBRARY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    pkg_check_modules(UV REQUIRED IMPORTED_TARGET libuv)
    set(SODIUM_TARGET PkgConfig::SODIUM)
    set(UV_TARGET PkgConfig::UV)

    # hyperdht-cpp installs hyperdht-config.cmake; fall back to pkg-config.
    find_package(hyperdht QUIET)
    if(NOT hyperdht_FOUND)
        pkg_check_modules(HYPERDHT REQUIRED IMPORTED_TARGET hyperdht)
        add_library(hyperdht::hyperdht ALIAS PkgConfig::HYPERDHT)
    endif()
    set(HYPERDHT_TARGET hyperdht::hyperdht)
else()
    foreach(v SODIUM_INCLUDE_DIR SODIUM_LIBRARY UV_INCLUDE_DIR UV_LIBRARY
              HYPERDHT_INCLUDE_DIR HYPERDHT_LIBRARY UDX_LIBRARY)
        if(NOT ${v})
            message(FATAL_ERROR "Cross build: -D${v}=<path> is required")
        endif()
    endforeach()

    add_library(sodium_ext STATIC IMPORTED)
    set_target_properties(sodium_ext PROPERTIES
        IMPORTED_LOCATION "${SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}")

    add_library(uv_ext STATIC IMPORTED)
    set_target_properties(uv_ext PROPERTIES
        IMPORTED_LOCATION "${UV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UV_INCLUDE_DIR}")

    add_library(udx_ext STATIC IMPORTED)
    set_target_properties(udx_ext PROPERTIES
        IMPORTED_LOCATION "${UDX_LIBRARY}")

    # libhyperdht.a needs udx/sodium/uv AFTER it on the link line. Declaring
    # that as INTERFACE_LINK_LIBRARIES lets CMake order the archives instead of
    # us hand-tuning the link line.
    add_library(hyperdht_ext STATIC IMPORTED)
    set_target_properties(hyperdht_ext PROPERTIES
        IMPORTED_LOCATION "${HYPERDHT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${HYPERDHT_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "udx_ext;sodium_ext;uv_ext")

    set(SODIUM_TARGET sodium_ext)
    set(UV_TARGET uv_ext)
    set(HYPERDHT_TARGET hyperdht_ext)
endif()

add_library(holesail_lib STATIC)
target_include_directories(holesail_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(holesail_lib PUBLIC
    ${HYPERDHT_TARGET} ${SODIUM_TARGET} ${UV_TARGET})
target_compile_options(holesail_lib PRIVATE -Wall -Wextra)
target_include_directories(holesail_lib PRIVATE third_party/qrcodegen)

# Every unit of the library, registered up front so a task that fills one of
# these files never has to touch this file.
target_sources(holesail_lib PRIVATE
    src/addr.cpp
    src/keys.cpp
    src/record.cpp
    src/logger.cpp
    src/pipe.cpp
    src/udp_pipe.cpp
    src/server.cpp
    src/client.cpp
    src/holesail.cpp
    src/cli.cpp
    third_party/qrcodegen/qrcodegen.cpp)

add_executable(holesail app/main.cpp)
target_link_libraries(holesail PRIVATE holesail_lib)

# Install the CLI *and* the static library with its headers. The library half
# is the point of the lib+CLI split: other projects in the workspace embed the
# tunnel rather than shelling out to the binary.
# Without at least one install rule the flake's package output fails
# ("ninja: unknown target 'install'").
include(GNUInstallDirs)

install(TARGETS holesail RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES third_party/qrcodegen/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/holesail-cpp
    RENAME qrcodegen-LICENSE)
install(FILES third_party/qrcode-terminal/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/holesail-cpp
    RENAME qrcode-terminal-LICENSE)

install(TARGETS holesail_lib
    EXPORT holesail-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/holesail
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

# Lets a consumer do find_package(holesail) + target_link_libraries(x holesail::holesail_lib),
# the same way this project consumes hyperdht.
install(EXPORT holesail-targets
    FILE holesail-targets.cmake
    NAMESPACE holesail::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/holesail)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/holesail-config.cmake
"include(CMakeFindDependencyMacro)
find_dependency(hyperdht)
include(\"\${CMAKE_CURRENT_LIST_DIR}/holesail-targets.cmake\")
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/holesail-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/holesail)

if(HOLESAIL_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    function(holesail_add_test name)
        add_executable(${name} test/${name}.cpp)
        target_link_libraries(${name} PRIVATE holesail_lib GTest::gtest GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    # Every suite, registered up front — see the note on target_sources above.
    holesail_add_test(test_smoke)
    holesail_add_test(test_addr)
    holesail_add_test(test_keys)
    holesail_add_test(test_record)
    holesail_add_test(test_logger)
    holesail_add_test(test_pipe)
    holesail_add_test(test_udp_pipe)
    holesail_add_test(test_server)
    holesail_add_test(test_client)
    holesail_add_test(test_holesail)
    holesail_add_test(test_cli)
    holesail_add_test(test_integration)
endif()

cmake_minimum_required(VERSION 3.20)
project(holesail-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOLESAIL_BUILD_TESTS "Build the test suite" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(UV REQUIRED IMPORTED_TARGET libuv)

# hyperdht-cpp installs hyperdht-config.cmake; fall back to pkg-config.
find_package(hyperdht QUIET)
if(NOT hyperdht_FOUND)
    pkg_check_modules(HYPERDHT REQUIRED IMPORTED_TARGET hyperdht)
    add_library(hyperdht::hyperdht ALIAS PkgConfig::HYPERDHT)
endif()

add_library(holesail_lib STATIC)
target_include_directories(holesail_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(holesail_lib PUBLIC
    hyperdht::hyperdht PkgConfig::SODIUM PkgConfig::UV)
target_compile_options(holesail_lib PRIVATE -Wall -Wextra)

# Every unit of the library, registered up front so a task that fills one of
# these files never has to touch this file.
target_sources(holesail_lib PRIVATE
    src/keys.cpp
    src/record.cpp
    src/logger.cpp
    src/pipe.cpp
    src/udp_pipe.cpp
    src/server.cpp
    src/client.cpp
    src/holesail.cpp
    src/cli.cpp)

add_executable(holesail app/main.cpp)
target_link_libraries(holesail PRIVATE holesail_lib)

# The CLI is the only artefact worth installing; holesail_lib is internal.
# Without this the flake's package output fails ("ninja: unknown target 'install'").
install(TARGETS holesail RUNTIME DESTINATION bin)

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

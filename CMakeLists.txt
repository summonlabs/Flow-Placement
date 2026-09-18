# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# Flow Placement 1.0.0 - vendor-neutral, C++20, dependency-free build.

cmake_minimum_required(VERSION 3.20)

project(flowplace
  VERSION 1.0.0
  DESCRIPTION "Deterministic, generation-bound placement of admitted flows onto authorized candidate paths"
  HOMEPAGE_URL "https://github.com/summonlabs/Flow-Placement"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FLOWPLACE_IS_TOP_LEVEL ON)
else()
  set(FLOWPLACE_IS_TOP_LEVEL OFF)
endif()

option(FLOWPLACE_BUILD_CLI "Build the flowplace command line tool" ${FLOWPLACE_IS_TOP_LEVEL})
option(FLOWPLACE_BUILD_TESTS "Build the flowplace test suite" ${FLOWPLACE_IS_TOP_LEVEL})
option(FLOWPLACE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(FLOWPLACE_ENABLE_ASAN "Enable AddressSanitizer for this build" OFF)
option(FLOWPLACE_ENABLE_ANALYZE "Enable MSVC static analysis (/analyze) for this build" OFF)
option(FLOWPLACE_INSTALL "Generate install and export rules" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings interface
# ---------------------------------------------------------------------------
# Warnings are applied directly to each first-party target so that no build-only
# target leaks into the exported package.
function(flowplace_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(FLOWPLACE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(FLOWPLACE_ENABLE_ANALYZE)
      target_compile_options(${target} PRIVATE /analyze /analyze:external:disable)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wconversion -Wsign-conversion)
    if(FLOWPLACE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(flowplace_apply_sanitizers target)
  if(FLOWPLACE_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(FLOWPLACE_SOURCES
  src/status.cpp
  src/hash.cpp
  src/bytes.cpp
  src/model.cpp
  src/explain.cpp
  src/validate.cpp
  src/engine.cpp
  src/codec.cpp
  src/store.cpp
  src/runtime.cpp
  src/wire.cpp
  src/transport.cpp
  src/server.cpp
  src/process.cpp
  src/scenario.cpp
  src/bench.cpp
)

add_library(flowplace ${FLOWPLACE_SOURCES})
add_library(flowplace::flowplace ALIAS flowplace)

target_compile_features(flowplace PUBLIC cxx_std_20)
target_include_directories(flowplace
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
flowplace_apply_warnings(flowplace)
target_link_libraries(flowplace PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(flowplace PUBLIC ws2_32)
endif()

set_target_properties(flowplace PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME flowplace)

flowplace_apply_sanitizers(flowplace)

# ---------------------------------------------------------------------------
# Command line tool
# ---------------------------------------------------------------------------
if(FLOWPLACE_BUILD_CLI)
  add_executable(flowplace_cli apps/flowplace_main.cpp)
  set_target_properties(flowplace_cli PROPERTIES OUTPUT_NAME flowplace)
  target_link_libraries(flowplace_cli PRIVATE flowplace)
  flowplace_apply_warnings(flowplace_cli)
  flowplace_apply_sanitizers(flowplace_cli)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FLOWPLACE_BUILD_TESTS)
  enable_testing()
  add_executable(flowplace_tests
    tests/main.cpp
    tests/test_ids_and_model.cpp
    tests/test_engine_basic.cpp
    tests/test_engine_constraints.cpp
    tests/test_engine_staleness.cpp
    tests/test_engine_policy.cpp
    tests/test_property_determinism.cpp
    tests/test_adversarial_input.cpp
    tests/test_codec_wire.cpp
    tests/test_store.cpp
    tests/test_runtime.cpp
    tests/test_concurrency.cpp
    tests/test_multiprocess.cpp
    tests/test_scenario_bench.cpp
  )
  target_include_directories(flowplace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(flowplace_tests PRIVATE flowplace)
  flowplace_apply_warnings(flowplace_tests)
  target_compile_definitions(flowplace_tests PRIVATE
    FLOWPLACE_CLI_PATH="$<TARGET_FILE:flowplace_cli>"
    FLOWPLACE_TEST_BINARY="$<TARGET_FILE:flowplace_tests>")
  flowplace_apply_sanitizers(flowplace_tests)

  set(FLOWPLACE_TEST_SUITES
    ids_and_model engine_basic engine_constraints engine_staleness engine_policy
    property_determinism adversarial_input codec_wire store runtime concurrency
    multiprocess scenario_bench)
  foreach(suite IN LISTS FLOWPLACE_TEST_SUITES)
    add_test(NAME ${suite} COMMAND flowplace_tests ${suite})
  endforeach()
  # The full suite runs every test in one process exactly once.
  add_test(NAME all COMMAND flowplace_tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
if(FLOWPLACE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS flowplace
    EXPORT flowplaceTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  if(FLOWPLACE_BUILD_CLI)
    install(TARGETS flowplace_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  set(FLOWPLACE_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/flowplace)
  install(EXPORT flowplaceTargets
    FILE flowplaceTargets.cmake
    NAMESPACE flowplace::
    DESTINATION ${FLOWPLACE_CMAKE_INSTALL_DIR})

  configure_package_config_file(
    cmake/flowplaceConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/flowplaceConfig.cmake
    INSTALL_DESTINATION ${FLOWPLACE_CMAKE_INSTALL_DIR})

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/flowplaceConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/flowplaceConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/flowplaceConfigVersion.cmake
    DESTINATION ${FLOWPLACE_CMAKE_INSTALL_DIR})

  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/flowplace)
  install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/flowplace)
endif()

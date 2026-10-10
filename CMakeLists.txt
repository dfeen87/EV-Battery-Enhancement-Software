cmake_minimum_required(VERSION 3.15)

project(
    DS_Battery_Enhancement
    VERSION 8.0.1
    DESCRIPTION "DS Theory Battery Management & BMS Middleware"
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Default Build Type & Sanitizers
# ============================================================================

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" build_type_lower)
if(build_type_lower STREQUAL "debug")
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer for Debug build")
endif()

# ============================================================================
# Project Configuration
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(DS_BUILD_EXAMPLES "Build example applications" ON)
option(DS_BUILD_TESTS "Build unit tests" ON)
option(DS_BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(DS_USE_GPU "Enable GPU acceleration (CUDA)" OFF)
option(DS_INSTALL "Generate install target" ON)
option(DS_ENABLE_FEEN "Enable FEEN integration" OFF)
option(DS_ENABLE_PYTHON_GOVERNOR "Embed the optional Python AILEE governor" OFF)

# ============================================================================
# Header-Only Library Target
# ============================================================================

include(FetchContent)
if(DS_ENABLE_FEEN)
    set(FEEN_BUILD_PYTHON OFF CACHE BOOL "Disable FEEN python" FORCE)
    FetchContent_Declare(
        feen
        GIT_REPOSITORY https://github.com/dfeen87/feen.git
        GIT_TAG v3.8.1
    )
    FetchContent_MakeAvailable(feen)
endif()

if(DS_ENABLE_PYTHON_GOVERNOR)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.12.0
    )
    FetchContent_MakeAvailable(pybind11)
endif()

add_library(ds_battery INTERFACE)

target_include_directories(ds_battery INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# AILEE Automotive Domain & Governor Library
add_library(ds_ailee_governor STATIC
    src/ailee_horsepower_governor.cpp
    src/ds_torque_manager.cpp
)
target_link_libraries(ds_ailee_governor PUBLIC ds_battery)
if(DS_ENABLE_PYTHON_GOVERNOR)
    target_link_libraries(ds_ailee_governor PUBLIC pybind11::embed)
    target_compile_definitions(ds_ailee_governor PRIVATE DS_ENABLE_PYTHON_GOVERNOR=1)
endif()
target_include_directories(ds_ailee_governor PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(ds_battery INTERFACE cxx_std_17)
if(DS_ENABLE_FEEN)
    target_link_libraries(ds_battery INTERFACE $<BUILD_INTERFACE:feen>)
endif()

# Public compile definitions (available to consumers)
target_compile_definitions(ds_battery INTERFACE
    DS_BMS_MIDDLEWARE_ENABLED
    DS_HEADER_ONLY
    $<$<BOOL:${DS_ENABLE_FEEN}>:DS_ENABLE_FEEN>
)

# Warnings (applied per-target, not globally)
if(MSVC)
    target_compile_options(ds_battery INTERFACE /W4 /WX)
else()
    target_compile_options(ds_battery INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# ============================================================================
# GPU Support (Optional)
# ============================================================================

if(DS_USE_GPU)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "CUDA toolkit found - enabling GPU support")
        target_compile_definitions(ds_battery INTERFACE DS_USE_GPU)
    else()
        message(WARNING "CUDA requested but not found - continuing without GPU")
    endif()
endif()

# ============================================================================
# Example Applications
# ============================================================================

if(DS_BUILD_EXAMPLES)
    message(STATUS "Building examples")

    add_executable(basic_integration examples/basic_integration.cpp)
    target_link_libraries(basic_integration PRIVATE ds_battery)

    add_executable(rest_api_server examples/rest_api_server.cpp)
    target_link_libraries(rest_api_server PRIVATE ds_battery pthread)

    add_executable(raps_ev_stability_demo examples/raps_ev_stability_demo.cpp)
    target_link_libraries(raps_ev_stability_demo PRIVATE ds_ailee_governor)

    # Note: multi_cell_pack.cpp currently has API mismatches and is disabled
    # add_executable(multi_cell_pack examples/multi_cell_pack.cpp)
    # target_link_libraries(multi_cell_pack PRIVATE ds_battery)

    install(
        TARGETS basic_integration rest_api_server raps_ev_stability_demo
        RUNTIME DESTINATION bin/examples
    )
endif()

# ============================================================================
# Unit Tests
# ============================================================================

if(DS_BUILD_TESTS)
    message(STATUS "Building tests")
    enable_testing()

    # The test executables use the standard assert contract. Keep assertions
    # active even when the repository default build type is Release.
    function(ds_enable_test_assertions target)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE ds_battery)
    add_test(NAME test_core COMMAND test_core)
    ds_enable_test_assertions(test_core)

    add_executable(test_feen_integration tests/test_feen_integration.cpp)
    target_link_libraries(test_feen_integration PRIVATE ds_battery)
    add_test(NAME test_feen_integration COMMAND test_feen_integration)
    ds_enable_test_assertions(test_feen_integration)

    add_executable(test_advanced tests/test_advanced.cpp)
    target_link_libraries(test_advanced PRIVATE ds_battery)
    add_test(NAME test_advanced COMMAND test_advanced)
    ds_enable_test_assertions(test_advanced)

    add_executable(test_middleware tests/test_middleware.cpp)
    target_link_libraries(test_middleware PRIVATE ds_battery)
    add_test(NAME test_middleware COMMAND test_middleware)
    ds_enable_test_assertions(test_middleware)

    add_executable(test_protection_envelopes tests/test_protection_envelopes.cpp)
    target_link_libraries(test_protection_envelopes PRIVATE ds_battery)
    add_test(NAME test_protection_envelopes COMMAND test_protection_envelopes)
    ds_enable_test_assertions(test_protection_envelopes)

    add_executable(test_ailee_governor tests/test_ailee_governor.cpp)
    target_link_libraries(test_ailee_governor PRIVATE ds_ailee_governor)
    if(DS_ENABLE_PYTHON_GOVERNOR)
        target_compile_definitions(test_ailee_governor PRIVATE DS_TEST_PYTHON_GOVERNOR=1)
    endif()
    add_test(NAME test_ailee_governor COMMAND test_ailee_governor)
    ds_enable_test_assertions(test_ailee_governor)

    add_executable(test_raps_ev_stability tests/test_raps_ev_stability.cpp)
    target_link_libraries(test_raps_ev_stability PRIVATE ds_ailee_governor)
    add_test(NAME test_raps_ev_stability COMMAND test_raps_ev_stability)
    ds_enable_test_assertions(test_raps_ev_stability)

    add_executable(test_ailee_trust_integration tests/test_ailee_trust_integration.cpp)
    target_link_libraries(test_ailee_trust_integration PRIVATE ds_ailee_governor)
    add_test(NAME test_ailee_trust_integration COMMAND test_ailee_trust_integration)
    ds_enable_test_assertions(test_ailee_trust_integration)
endif()

# ============================================================================
# Benchmarks
# ============================================================================

if(DS_BUILD_BENCHMARKS)
    message(STATUS "Building benchmarks")

    add_executable(benchmark_update benchmarks/benchmark_update.cpp)
    target_link_libraries(benchmark_update PRIVATE ds_battery)

    add_executable(benchmark_multicell benchmarks/benchmark_multicell.cpp)
    target_link_libraries(benchmark_multicell PRIVATE ds_battery)
endif()

# ============================================================================
# DS Core Executable & Shared Library Distribution
# ============================================================================

message(STATUS "Configuring DS Core Distribution Targets")

# 1. CLI Executable
add_executable(ds_enhancer ds_core/src/ds_enhancer.cpp)
target_link_libraries(ds_enhancer PRIVATE ds_battery)
target_compile_definitions(ds_enhancer PRIVATE DS_BUILD_CLI)
set_target_properties(ds_enhancer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ds_core/bin"
)
if(DS_BUILD_TESTS)
    add_test(NAME test_cli_version COMMAND ds_enhancer --version)
    set_tests_properties(test_cli_version PROPERTIES
        PASS_REGULAR_EXPRESSION "DS-EV 8\\.0\\.1"
    )
endif()

# 2. Shared Library for Python Wrapper
add_library(ds_enhancer_shared SHARED ds_core/src/ds_enhancer.cpp)
target_link_libraries(ds_enhancer_shared PRIVATE ds_battery)
set_target_properties(ds_enhancer_shared PROPERTIES
    OUTPUT_NAME ds_enhancer
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ds_core/lib"
)

# 3. pybind11 Python Module
if(DS_ENABLE_PYTHON_GOVERNOR)
    pybind11_add_module(ds_enhancer_pybind ds_core/src/ds_pybind11_wrapper.cpp)
    target_link_libraries(ds_enhancer_pybind PRIVATE ds_battery)
    set_target_properties(ds_enhancer_pybind PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ds_core/lib"
    )
endif()

# ============================================================================
# Installation & Package Export
# ============================================================================

if(DS_INSTALL)
    include(GNUInstallDirs)

    install(
        DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        TARGETS ds_battery
        EXPORT ds_battery-targets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        EXPORT ds_battery-targets
        FILE ds_battery-targets.cmake
        NAMESPACE ds::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ds_battery
    )

    include(CMakePackageConfigHelpers)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ds_battery-config-version.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ds_battery-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ds_battery-config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ds_battery
    )

    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/ds_battery-config.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/ds_battery-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ds_battery
    )
endif()

# ============================================================================
# Packaging
# ============================================================================

set(CPACK_PACKAGE_NAME "DS_Battery_Enhancement")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "DS Battery Management & BMS Middleware")
set(CPACK_PACKAGE_VENDOR "Don Michael Feeney Jr.")
set(CPACK_PACKAGE_CONTACT "dfeen87@gmail.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

include(CPack)

# ============================================================================
# Status Summary
# ============================================================================

message(STATUS "")
message(STATUS "=======================================")
message(STATUS "DS Battery Enhancement Build Summary")
message(STATUS "=======================================")
message(STATUS "Version:        ${PROJECT_VERSION}")
message(STATUS "Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Options:")
message(STATUS "  Examples:     ${DS_BUILD_EXAMPLES}")
message(STATUS "  Tests:        ${DS_BUILD_TESTS}")
message(STATUS "  Benchmarks:   ${DS_BUILD_BENCHMARKS}")
message(STATUS "  GPU Support:  ${DS_USE_GPU}")
message(STATUS "  Install:      ${DS_INSTALL}")
message(STATUS "")
message(STATUS "Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "=======================================")
message(STATUS "")

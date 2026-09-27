cmake_minimum_required(VERSION 3.21)

project(FacilityStateLedger
        VERSION 1.0.0
        DESCRIPTION "Durable, provenance-preserving ledger of authoritative facility-state transitions (DCCP Tranche 1)"
        HOMEPAGE_URL "https://github.com/summonlabs/Facility-State-Ledger"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FSL_BUILD_TESTS "Build the Facility State Ledger test suite" ON)
option(FSL_BUILD_TOOLS "Build the fsl inspection CLI" ON)
option(FSL_BUILD_EXAMPLES "Build the Facility State Ledger examples" ON)
option(FSL_BUILD_BENCHMARKS "Build the Facility State Ledger benchmarks" ON)
option(FSL_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FSL_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration: strict, and never suppressed with pragmas
# ---------------------------------------------------------------------------
add_library(fsl_warnings INTERFACE)
if(MSVC)
  target_compile_options(fsl_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
  if(FSL_WARNINGS_AS_ERRORS)
    target_compile_options(fsl_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(fsl_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(fsl_warnings INTERFACE
                         -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
                         -Wold-style-cast -Wcast-qual -Wdouble-promotion -Wformat=2
                         -Wundef -Wnull-dereference -Wnon-virtual-dtor -Woverloaded-virtual)
  if(FSL_WARNINGS_AS_ERRORS)
    target_compile_options(fsl_warnings INTERFACE -Werror)
  endif()
endif()

if(FSL_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(fsl_warnings INTERFACE /fsanitize=address)
  else()
    target_compile_options(fsl_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fsl_warnings INTERFACE -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(facility_state_ledger
  src/audit.cpp
  src/clock.cpp
  src/codec.cpp
  src/derived.cpp
  src/event.cpp
  src/frame.cpp
  src/hash.cpp
  src/ids.cpp
  src/ledger.cpp
  src/ledger_append.cpp
  src/ledger_index.cpp
  src/ledger_locate.cpp
  src/ledger_open.cpp
  src/ledger_read.cpp
  src/options.cpp
  src/paths.cpp
  src/payload.cpp
  src/platform.cpp
  src/provenance.cpp
  src/replay.cpp
  src/status.cpp
  src/subject.cpp
  src/text.cpp
)
add_library(FacilityStateLedger::facility_state_ledger ALIAS facility_state_ledger)

target_include_directories(facility_state_ledger
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# The warnings interface is a build-time concern only; it must not leak into the
# exported package as a usage requirement for consumers.
target_link_libraries(facility_state_ledger PRIVATE "$<BUILD_INTERFACE:fsl_warnings>")
target_compile_features(facility_state_ledger PUBLIC cxx_std_20)
set_target_properties(facility_state_ledger PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME facility_state_ledger)

if(WIN32)
  target_compile_definitions(facility_state_ledger PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Inspection CLI
# ---------------------------------------------------------------------------
if(FSL_BUILD_TOOLS)
  add_executable(fsl tools/fsl/main.cpp)
  target_link_libraries(fsl PRIVATE facility_state_ledger fsl_warnings)
  set_target_properties(fsl PROPERTIES OUTPUT_NAME fsl)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(FSL_BUILD_EXAMPLES)
  add_executable(fsl_example_facility_journal examples/facility_journal.cpp)
  target_link_libraries(fsl_example_facility_journal PRIVATE facility_state_ledger fsl_warnings)

  add_executable(fsl_example_crash_writer examples/crash_writer.cpp)
  target_link_libraries(fsl_example_crash_writer PRIVATE facility_state_ledger fsl_warnings)

  add_executable(fsl_example_audit_export examples/audit_export.cpp)
  target_link_libraries(fsl_example_audit_export PRIVATE facility_state_ledger fsl_warnings)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(FSL_BUILD_BENCHMARKS)
  add_executable(fsl_bench bench/fsl_bench.cpp)
  target_link_libraries(fsl_bench PRIVATE facility_state_ledger fsl_warnings)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FSL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------
install(TARGETS facility_state_ledger
        EXPORT FacilityStateLedgerTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/fsl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FSL_BUILD_TOOLS)
  install(TARGETS fsl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT FacilityStateLedgerTargets
        FILE FacilityStateLedgerTargets.cmake
        NAMESPACE FacilityStateLedger::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityStateLedger)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FacilityStateLedgerConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FacilityStateLedgerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityStateLedger)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FacilityStateLedgerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/FacilityStateLedgerConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/FacilityStateLedgerConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityStateLedger)

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/FacilityStateLedger
        OPTIONAL)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Facility State Ledger ${PROJECT_VERSION}")
message(STATUS "  build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler             : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  warnings as errors   : ${FSL_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizer            : ${FSL_ENABLE_ASAN}")
message(STATUS "  tests/tools/examples : ${FSL_BUILD_TESTS}/${FSL_BUILD_TOOLS}/${FSL_BUILD_EXAMPLES}")

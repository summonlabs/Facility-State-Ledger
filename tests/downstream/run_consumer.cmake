# Configures, builds and runs the downstream consumer against a freshly
# installed FacilityStateLedger package.
#
# The install tree, the consumer build tree and the consumer's working directory
# all live outside the source tree, and the consumer is configured from its own
# directory so that no in-tree state can leak into the result.

if(NOT DEFINED FSL_SOURCE_DIR OR NOT DEFINED FSL_BINARY_DIR OR
   NOT DEFINED FSL_CONSUMER_SOURCE_DIR OR NOT DEFINED FSL_WORK_DIR)
  message(FATAL_ERROR "downstream consumer test is missing required variables")
endif()

set(prefix "${FSL_WORK_DIR}/prefix")
set(build "${FSL_WORK_DIR}/build")
set(run "${FSL_WORK_DIR}/run")

file(REMOVE_RECURSE "${FSL_WORK_DIR}")
file(MAKE_DIRECTORY "${run}")

# 1. Install the built package into a clean prefix.
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${FSL_BINARY_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result}): ${install_output}${install_error}")
endif()

# 2. The install tree must actually contain the exported package and headers.
foreach(required
        "lib/cmake/FacilityStateLedger/FacilityStateLedgerConfig.cmake"
        "lib/cmake/FacilityStateLedger/FacilityStateLedgerConfigVersion.cmake"
        "lib/cmake/FacilityStateLedger/FacilityStateLedgerTargets.cmake"
        "include/fsl/ledger.hpp"
        "include/fsl/status.hpp"
        "include/fsl/event.hpp")
  if(NOT EXISTS "${prefix}/${required}")
    # Windows installs libraries under lib; allow the multiarch spelling too.
    if(NOT EXISTS "${prefix}/lib64/${required}" AND NOT EXISTS "${prefix}/bin/${required}")
      message(FATAL_ERROR "the install tree is missing ${required}")
    endif()
  endif()
endforeach()

# 3. Configure the consumer from outside the source tree, pointing only at the
#    installed prefix.
set(configure_command
    "${CMAKE_COMMAND}"
    -S "${FSL_CONSUMER_SOURCE_DIR}"
    -B "${build}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DCMAKE_BUILD_TYPE=${FSL_BUILD_TYPE}")
if(FSL_GENERATOR)
  list(APPEND configure_command -G "${FSL_GENERATOR}")
endif()
if(FSL_CXX_COMPILER)
  list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${FSL_CXX_COMPILER}")
endif()

execute_process(
  COMMAND ${configure_command}
  WORKING_DIRECTORY "${run}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "downstream configure failed: ${configure_output}${configure_error}")
endif()

# 4. Build it.
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${build}"
  WORKING_DIRECTORY "${run}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "downstream build failed: ${build_output}${build_error}")
endif()

# 5. Locate and run the executable.
set(candidates
    "${build}/downstream_consumer"
    "${build}/downstream_consumer.exe"
    "${build}/Release/downstream_consumer.exe"
    "${build}/Debug/downstream_consumer.exe")
set(consumer "")
foreach(candidate ${candidates})
  if(EXISTS "${candidate}")
    set(consumer "${candidate}")
    break()
  endif()
endforeach()
if(consumer STREQUAL "")
  message(FATAL_ERROR "downstream build produced no runnable executable")
endif()

execute_process(
  COMMAND "${consumer}" "${run}/ledger"
  WORKING_DIRECTORY "${run}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "downstream consumer failed (${run_result}): ${run_output}${run_error}")
endif()

message(STATUS "downstream consumer: ${run_output}")

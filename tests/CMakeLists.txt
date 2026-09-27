# Facility State Ledger test suite.
#
# Every test is a standalone executable that returns non-zero on failure. No test
# carries a timeout: a hanging test is a defect to diagnose, not to kill.

add_library(fsl_test_support STATIC
  support/test_support.cpp
  support/journal.cpp
)
target_include_directories(fsl_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(fsl_test_support PUBLIC facility_state_ledger)
target_link_libraries(fsl_test_support PRIVATE "$<BUILD_INTERFACE:fsl_warnings>")

function(fsl_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE fsl_test_support)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

fsl_add_test(test_status)
fsl_add_test(test_hash)
fsl_add_test(test_text)
fsl_add_test(test_ids)
fsl_add_test(test_codec)
fsl_add_test(test_frame)
fsl_add_test(test_ledger_basics)
fsl_add_test(test_ledger_admission)
fsl_add_test(test_ledger_idempotency)
fsl_add_test(test_ledger_batches)
fsl_add_test(test_ledger_queries)
fsl_add_test(test_ledger_replay)
fsl_add_test(test_ledger_verify)
fsl_add_test(test_ledger_rotation)
fsl_add_test(test_recovery)
fsl_add_test(test_corruption)
fsl_add_test(test_multiprocess)
fsl_add_test(test_concurrency)
fsl_add_test(test_property)

if(FSL_BUILD_TOOLS AND FSL_BUILD_EXAMPLES)
  fsl_add_test(test_cli)
  target_compile_definitions(test_cli PRIVATE
    FSL_TOOL_PATH="$<TARGET_FILE:fsl>"
    FSL_CRASH_WRITER_PATH="$<TARGET_FILE:fsl_example_crash_writer>")
  add_dependencies(test_cli fsl fsl_example_crash_writer)
endif()

# The multiprocess suite drives real operating-system processes; it needs the
# crash writer to prove committed-prefix semantics across a killed writer.
if(FSL_BUILD_EXAMPLES)
  target_compile_definitions(test_multiprocess PRIVATE
    FSL_CRASH_WRITER_PATH="$<TARGET_FILE:fsl_example_crash_writer>")
  add_dependencies(test_multiprocess fsl_example_crash_writer)
endif()

# The downstream consumer is configured, built and run against a fresh install
# of this build tree, from outside the source tree. It is the distributability
# proof: an in-tree build is not evidence that the package works.
add_test(NAME downstream_consumer
         COMMAND ${CMAKE_COMMAND}
                 -DFSL_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                 -DFSL_BINARY_DIR=${CMAKE_BINARY_DIR}
                 -DFSL_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/downstream
                 -DFSL_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream-work
                 -DFSL_GENERATOR=${CMAKE_GENERATOR}
                 -DFSL_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                 -DFSL_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/downstream/run_consumer.cmake)
set_tests_properties(downstream_consumer PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

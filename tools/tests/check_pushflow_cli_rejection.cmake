# SPDX-License-Identifier: GPL-3.0-only
if(CASE STREQUAL "invalid")
  set(args --expect-outputs 2)
  set(expected "--expect-outputs requires 1 or 4")
elseif(CASE STREQUAL "missing")
  set(args --expect-outputs)
  set(expected "invalid PushFlow fixture arguments")
else()
  message(FATAL_ERROR "Unknown PushFlow CLI case: ${CASE}")
endif()

execute_process(COMMAND "${PROGRAM}" ${args}
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
string(STRIP "${error}" error)
if(NOT "${status}" STREQUAL "2" OR NOT "${error}" STREQUAL "${expected}" OR NOT "${output}" STREQUAL "")
  message(FATAL_ERROR "Expected parser rejection (exit 2) for ${CASE}; got ${status}\n${output}${error}")
endif()
message(STATUS "PushFlow parser ${CASE}: exit ${status}; ${error}")

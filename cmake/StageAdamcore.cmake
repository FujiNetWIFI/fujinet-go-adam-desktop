# Stage the adamcore sources into core/adamcore-generated at configure time
# when the staged tree is missing, or on demand with -DADAMCORE_RESTAGE=ON.
# The staging script honors ADAMCORE_SRC= for out-of-tree adamcore checkouts.

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")

option(ADAMCORE_RESTAGE "Re-stage adamcore sources from the source checkout" OFF)

if(ADAMCORE_RESTAGE OR NOT EXISTS "${ADAMCORE_GEN}/src/machine.c")
  message(STATUS "Staging adamcore sources (tools/adamcore/stage-adamcore.sh)")
  execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/tools/adamcore/stage-adamcore.sh"
    RESULT_VARIABLE _adamcore_stage_rc
  )
  if(NOT _adamcore_stage_rc EQUAL 0)
    message(FATAL_ERROR "adamcore staging failed; set ADAMCORE_SRC to the adamcore checkout")
  endif()
endif()

if(EXISTS "${ADAMCORE_GEN}/.source-info")
  file(READ "${ADAMCORE_GEN}/.source-info" _adamcore_commit)
  string(STRIP "${_adamcore_commit}" _adamcore_commit)
  message(STATUS "adamcore staged at commit ${_adamcore_commit}")
endif()

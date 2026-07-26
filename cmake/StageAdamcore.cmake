# Provide the adamcore sources (see cmake/Dependencies.cmake) and stage them
# into core/adamcore-generated, which is what core/CMakeLists.txt compiles.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source checkout has moved to a different commit, or on demand with
# -DADAMCORE_RESTAGE=ON (which is also how to pick up uncommitted edits in a
# working checkout pointed at by ADAMCORE_SRC).

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")

option(ADAMCORE_RESTAGE "Re-stage adamcore sources from the source checkout" OFF)

adam_provide_dependency(
  NAME adamcore
  PATH third_party/adamcore
  URL "${ADAMCORE_URL}"
  COMMIT "${ADAMCORE_COMMIT}"
  SENTINEL src/machine.c
  OVERRIDE ADAMCORE_SRC
  RESULT ADAMCORE_DIR)

# Restage when the staged tree does not match the checkout's current commit.
set(_adamcore_stale OFF)
if(EXISTS "${ADAMCORE_GEN}/src/machine.c")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${ADAMCORE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _adamcore_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  set(_adamcore_staged "")
  if(EXISTS "${ADAMCORE_GEN}/.source-info")
    file(READ "${ADAMCORE_GEN}/.source-info" _adamcore_staged)
    string(STRIP "${_adamcore_staged}" _adamcore_staged)
  endif()
  if(_adamcore_head AND NOT _adamcore_head STREQUAL _adamcore_staged)
    message(STATUS "adamcore: staged tree is ${_adamcore_staged}, checkout is "
                   "${_adamcore_head} -- restaging")
    set(_adamcore_stale ON)
  endif()
else()
  set(_adamcore_stale ON)
endif()

if(ADAMCORE_RESTAGE OR _adamcore_stale)
  message(STATUS "Staging adamcore sources (tools/adamcore/stage-adamcore.sh)")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "ADAMCORE_SRC=${ADAMCORE_DIR}"
            "${CMAKE_SOURCE_DIR}/tools/adamcore/stage-adamcore.sh"
    RESULT_VARIABLE _adamcore_stage_rc
  )
  if(NOT _adamcore_stage_rc EQUAL 0)
    message(FATAL_ERROR "adamcore staging failed (source: ${ADAMCORE_DIR})")
  endif()
endif()

if(EXISTS "${ADAMCORE_GEN}/.source-info")
  file(READ "${ADAMCORE_GEN}/.source-info" _adamcore_commit)
  string(STRIP "${_adamcore_commit}" _adamcore_commit)
  message(STATUS "adamcore staged at commit ${_adamcore_commit}")
endif()

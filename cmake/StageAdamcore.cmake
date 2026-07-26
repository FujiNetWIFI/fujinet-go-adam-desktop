# Provide the adamcore sources (see cmake/Dependencies.cmake) and stage them
# into core/adamcore-generated, which is what core/CMakeLists.txt compiles.
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate
# file type or format"), and staging is a directory copy plus a marker file.
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

# What the staged tree was made from: the source commit, or the source path
# for a checkout with no git metadata.
set(_adamcore_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${ADAMCORE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _adamcore_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _adamcore_head)
  set(_adamcore_head "${ADAMCORE_DIR}")
endif()

set(_adamcore_staged "")
if(EXISTS "${ADAMCORE_GEN}/.source-info")
  file(READ "${ADAMCORE_GEN}/.source-info" _adamcore_staged)
  string(STRIP "${_adamcore_staged}" _adamcore_staged)
endif()

if(ADAMCORE_RESTAGE OR NOT EXISTS "${ADAMCORE_GEN}/src/machine.c"
   OR NOT _adamcore_staged STREQUAL _adamcore_head)
  message(STATUS "Staging adamcore sources from ${ADAMCORE_DIR}")
  file(REMOVE_RECURSE "${ADAMCORE_GEN}")
  file(MAKE_DIRECTORY "${ADAMCORE_GEN}")
  file(COPY "${ADAMCORE_DIR}/include" "${ADAMCORE_DIR}/src"
       DESTINATION "${ADAMCORE_GEN}")
  if(NOT EXISTS "${ADAMCORE_GEN}/src/machine.c")
    message(FATAL_ERROR "adamcore staging failed (source: ${ADAMCORE_DIR})")
  endif()
  file(WRITE "${ADAMCORE_GEN}/.source-info" "${_adamcore_head}\n")
endif()

message(STATUS "adamcore staged at ${_adamcore_head}")

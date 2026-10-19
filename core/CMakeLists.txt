# libadamsession: the toolkit-agnostic session core. Compiles the staged
# adamcore sources directly (like the Android app's cpp-generated tree) plus
# the desktop session/audio/gamepad/FujiNet/debugger layers, and embeds the
# ADAM system ROMs generated from tools/adamcore/roms.

include(GNUInstallDirs)

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")

find_package(SDL3 CONFIG REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- embedded ROMs ---------------------------------------------------------
# Without the (never-redistributed) ROM images, zero-filled placeholders are
# embedded instead so CI and ROM-less checkouts still build; boot-dependent
# tests skip themselves in that configuration.
set(ROM_DIR "${CMAKE_SOURCE_DIR}/tools/adamcore/roms")
set(ROMS_EMBEDDED_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
if(EXISTS "${ROM_DIR}/OS7.rom" AND EXISTS "${ROM_DIR}/EOS.rom"
   AND EXISTS "${ROM_DIR}/WP.rom")
  add_custom_command(
    OUTPUT "${ROMS_EMBEDDED_C}"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
            "${ROMS_EMBEDDED_C}"
            "${ROM_DIR}/OS7.rom" "${ROM_DIR}/EOS.rom" "${ROM_DIR}/WP.rom"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
            "${ROM_DIR}/OS7.rom" "${ROM_DIR}/EOS.rom" "${ROM_DIR}/WP.rom"
    COMMENT "Embedding ADAM system ROMs"
  )
else()
  message(WARNING "No system ROMs in ${ROM_DIR}; embedding zero-filled "
                  "placeholders (the machine will not boot)")
  add_custom_command(
    OUTPUT "${ROMS_EMBEDDED_C}"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
            --placeholder "${ROMS_EMBEDDED_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
    COMMENT "Embedding placeholder ROMs (none found)"
  )
endif()

# The staged adamcore translation units. adamcore ships the list itself, so
# a source added upstream cannot silently break the link here, and
# adamcore_sources() picks the socket backend (net.h) for the platform.
if(NOT EXISTS "${ADAMCORE_GEN}/adamcore-sources.cmake")
  message(FATAL_ERROR
    "The staged adamcore has no adamcore-sources.cmake: it predates the "
    "source list. Re-stage from a newer checkout with -DADAMCORE_RESTAGE=ON.")
endif()
include("${ADAMCORE_GEN}/adamcore-sources.cmake")
adamcore_sources(ADAMCORE_SOURCES ROOT "${ADAMCORE_GEN}")

add_library(adamsession STATIC
  ${ADAMCORE_SOURCES}
  # session layer
  src/session.c
  src/settings.c
  src/paths.c
  src/input_map.c
  src/audio_sdl.c
  src/gamepad_sdl.c
  src/fujinet_runtime.c
  debugger/debugger.c
  debugger/fujitrace.c
  debugger/z80dasm.c
  debugger/symbols.c
  debugger/symbols_builtin.c
  debugger/vdpview.c
  ${ROMS_EMBEDDED_C}
)

target_include_directories(adamsession
  PUBLIC include "${ADAMCORE_GEN}/include"
  PRIVATE src "${ADAMCORE_GEN}/src"
)
# -Wno-format-truncation: the path layer composes ADAM_PATH_MAX-sized
# buffers from other max-sized buffers; snprintf truncation is the intended
# clamp there, not a bug.
target_compile_options(adamsession PRIVATE -Wall -Wextra -Wno-format-truncation)
target_compile_definitions(adamsession PRIVATE
  _GNU_SOURCE
  ADAM_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  ADAM_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-adam"
  ADAM_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-adam"
)
# pthread everywhere (winpthreads on MinGW); dl/m are POSIX-only. On
# Windows the FujiNet library is loaded via LoadLibrary (see dynlib.h) and
# the math functions live in the CRT.
target_link_libraries(adamsession PUBLIC SDL3::SDL3 Threads::Threads)
if(WIN32)
  target_link_libraries(adamsession PUBLIC ws2_32)
else()
  target_link_libraries(adamsession PUBLIC dl m)
endif()
set_property(TARGET adamsession PROPERTY POSITION_INDEPENDENT_CODE ON)

# ---- tests -----------------------------------------------------------------
add_executable(input_test tests/input_test.c)
target_link_libraries(input_test adamsession)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke adamsession)
target_include_directories(boot_smoke PRIVATE src)

add_executable(dasm_test tests/dasm_test.c)
target_link_libraries(dasm_test adamsession)
target_include_directories(dasm_test PRIVATE debugger)

add_executable(debug_boot_test tests/debug_boot_test.c)
target_link_libraries(debug_boot_test adamsession)
target_include_directories(debug_boot_test PRIVATE src)

# Hardware probe utility (not registered as a test; needs a real pad).
add_executable(gamepad_probe tests/gamepad_probe.c)
target_link_libraries(gamepad_probe SDL3::SDL3)

add_executable(gamepad_map_test tests/gamepad_map_test.c)
target_link_libraries(gamepad_map_test adamsession)
target_include_directories(gamepad_map_test PRIVATE src)

add_executable(vdp_decode_test tests/vdp_decode_test.c)
target_link_libraries(vdp_decode_test adamsession)

add_executable(fujitrace_test tests/fujitrace_test.c)
target_link_libraries(fujitrace_test adamsession)
target_include_directories(fujitrace_test PRIVATE debugger)

add_test(NAME input COMMAND input_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME dasm COMMAND dasm_test)
add_test(NAME debug_boot COMMAND debug_boot_test)
add_test(NAME gamepad_map COMMAND gamepad_map_test)
add_test(NAME vdp_decode COMMAND vdp_decode_test)
add_test(NAME fujitrace COMMAND fujitrace_test)
set_tests_properties(boot_smoke debug_boot gamepad_map
                     PROPERTIES SKIP_RETURN_CODE 77)

# On Windows the tests that start threads otherwise need libwinpthread-1.dll
# beside them, which only happens to be true inside an MSYS2 shell. Link the
# runtime statically (as the frontend does) so the test exes run anywhere --
# on a bare Windows box, or under wine when cross-compiling.
if(WIN32)
  foreach(_t input_test boot_smoke dasm_test debug_boot_test
             gamepad_probe gamepad_map_test vdp_decode_test fujitrace_test)
    target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
  endforeach()
endif()

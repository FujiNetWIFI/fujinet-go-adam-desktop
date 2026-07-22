# libadamsession: the toolkit-agnostic session core. Compiles the staged
# adamcore sources directly (like the Android app's cpp-generated tree) plus
# the desktop session/audio/gamepad/FujiNet/debugger layers, and embeds the
# ADAM system ROMs generated from tools/adamcore/roms.

include(GNUInstallDirs)

set(ADAMCORE_GEN "${CMAKE_SOURCE_DIR}/core/adamcore-generated")

find_package(SDL3 CONFIG REQUIRED)

# ---- embedded ROMs ---------------------------------------------------------
set(ROM_DIR "${CMAKE_SOURCE_DIR}/tools/adamcore/roms")
set(ROMS_EMBEDDED_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
add_custom_command(
  OUTPUT "${ROMS_EMBEDDED_C}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
          "${ROMS_EMBEDDED_C}"
          "${ROM_DIR}/OS7.rom" "${ROM_DIR}/EOS.rom" "${ROM_DIR}/WP.rom"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/adamcore/embed-roms.py"
          "${ROM_DIR}/OS7.rom" "${ROM_DIR}/EOS.rom" "${ROM_DIR}/WP.rom"
  COMMENT "Embedding ADAM system ROMs"
)

add_library(adamsession STATIC
  # staged adamcore
  ${ADAMCORE_GEN}/src/z80.c
  ${ADAMCORE_GEN}/src/tms9928a.c
  ${ADAMCORE_GEN}/src/sn76489.c
  ${ADAMCORE_GEN}/src/machine.c
  ${ADAMCORE_GEN}/src/cart.c
  ${ADAMCORE_GEN}/src/adamnet.c
  ${ADAMCORE_GEN}/src/boip.c
  ${ADAMCORE_GEN}/src/net_posix.c
  ${ADAMCORE_GEN}/src/palette.c
  ${ADAMCORE_GEN}/src/debug.c
  # session layer
  src/session.c
  src/settings.c
  src/paths.c
  src/input_map.c
  src/audio_sdl.c
  src/gamepad_sdl.c
  src/fujinet_runtime.c
  debugger/debugger.c
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
target_link_libraries(adamsession PUBLIC SDL3::SDL3 pthread dl m)
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

add_test(NAME input COMMAND input_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME dasm COMMAND dasm_test)
add_test(NAME debug_boot COMMAND debug_boot_test)
add_test(NAME gamepad_map COMMAND gamepad_map_test)
set_tests_properties(gamepad_map PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME vdp_decode COMMAND vdp_decode_test)

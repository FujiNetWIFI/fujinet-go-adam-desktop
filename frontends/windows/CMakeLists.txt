# Windows frontend: native Win32 (Direct3D 11, GDI fallback) over the shared
# session core. Built only on Windows hosts; verified by the Windows CI job
# (MSYS2/MinGW-w64) and, before that, by cross-compiling with
# cmake/toolchains/mingw-w64.cmake and running under wine.
# WIN32_EXECUTABLE => WinMain, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

# The manifest carries the app version in its assembly identity, so it is
# generated from PROJECT_VERSION rather than repeating it. resource.rc picks
# the generated copy up through the RC compiler's include path.
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)

add_executable(fujinet-go-adam-windows WIN32
  main.c
  display.c
  keys.c
  keypad/keypad_window.c
  debugger/dbg_window.c
  resource.rc
)
target_link_libraries(fujinet-go-adam-windows PRIVATE
  adamsession
  # d3d11: the picture (display.c; d3dcompiler_47 and avrt are loaded, not
  # linked). dwmapi: DwmFlush, the GDI fallback's frame clock. uxtheme: the
  # keypad's themed buttons.
  d3d11 gdi32 user32 comdlg32 dwmapi shell32 comctl32 uxtheme
)
# The debugger window talks to the shared engine (core/include/adamdebug.h),
# which pulls in adamcore's debug header from the staged tree.
target_include_directories(fujinet-go-adam-windows PRIVATE
  "${CMAKE_SOURCE_DIR}/core/adamcore-generated/include"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(fujinet-go-adam-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps.
target_link_options(fujinet-go-adam-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-adam-windows RUNTIME DESTINATION bin)

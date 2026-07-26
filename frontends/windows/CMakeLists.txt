# Windows frontend: native Win32 (GDI + DwmFlush vsync) over the shared
# session core. Built only on Windows hosts; verified by the Windows CI job
# (MSYS2/MinGW-w64). WIN32_EXECUTABLE => WinMain, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

add_executable(fujinet-go-adam-windows WIN32
  main.c
  debugger/dbg_window.c
  resource.rc
)
target_link_libraries(fujinet-go-adam-windows PRIVATE
  adamsession
  gdi32 user32 comdlg32 dwmapi shell32 comctl32
)
# The debugger window talks to the shared engine (core/include/adamdebug.h),
# which pulls in adamcore's debug header from the staged tree.
target_include_directories(fujinet-go-adam-windows PRIVATE
  "${CMAKE_SOURCE_DIR}/core/adamcore-generated/include")
target_compile_options(fujinet-go-adam-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps.
target_link_options(fujinet-go-adam-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-adam-windows RUNTIME DESTINATION bin)

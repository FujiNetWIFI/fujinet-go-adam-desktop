# GNOME frontend: GTK4 + libadwaita, with an embedded WebKitGTK view of the
# FujiNet web admin when available (WITH_WEBVIEW). Found-or-skipped so lean
# machines can still build the core / KDE side.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_SOURCES
  main.c
  window.c
  display.c
  prefs.c
  webview.c
  debugger/dbg_window.c
)

add_executable(fujinet-go-adam-gnome ${GNOME_SOURCES})
target_link_libraries(fujinet-go-adam-gnome PRIVATE adamsession PkgConfig::ADW)
target_compile_options(fujinet-go-adam-gnome PRIVATE -Wall -Wextra)

if(WITH_WEBVIEW)
  pkg_check_modules(WEBKIT IMPORTED_TARGET webkitgtk-6.0)
  if(WEBKIT_FOUND)
    target_link_libraries(fujinet-go-adam-gnome PRIVATE PkgConfig::WEBKIT)
    target_compile_definitions(fujinet-go-adam-gnome PRIVATE HAVE_WEBKIT)
  else()
    message(STATUS "GNOME frontend: webkitgtk-6.0 not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-adam-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/online.fujinet.go.adam.gnome.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/online.fujinet.go.adam.gnome.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

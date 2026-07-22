cmake_minimum_required(VERSION 3.20)
project(fujinet-go-adam-desktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the other one (or just the core + tests).
set(FRONTEND "all" CACHE STRING "Frontend to build: gnome, kde, all, none")
option(WITH_WEBVIEW "Embed the FujiNet web UI (WebKitGTK / QtWebEngine)" ON)
option(ADAM_USE_SDL2 "Use SDL2 instead of SDL3 for audio/gamepad" OFF)

include(cmake/StageAdamcore.cmake)

enable_testing()
add_subdirectory(core)

if(FRONTEND STREQUAL "gnome" OR FRONTEND STREQUAL "all")
  add_subdirectory(frontends/gnome)
endif()
if(FRONTEND STREQUAL "kde" OR FRONTEND STREQUAL "all")
  add_subdirectory(frontends/kde)
endif()
if(FRONTEND STREQUAL "macos" OR FRONTEND STREQUAL "all")
  add_subdirectory(frontends/macos)
endif()

# Install the FujiNet runtime when it has been built
# (tools/fujinet/build-fujinet-desktop.sh); the session provisions the
# user's runtime tree from the installed copy on first start.
include(GNUInstallDirs)
set(FUJINET_OUT "${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
if(EXISTS "${FUJINET_OUT}/libfujinet.so")
  install(FILES "${FUJINET_OUT}/libfujinet.so"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/fujinet-go-adam)
  install(FILES "${FUJINET_OUT}/fnconfig.ini"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-adam/fujinet)
  install(DIRECTORY "${FUJINET_OUT}/data" "${FUJINET_OUT}/SD"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-adam/fujinet)
else()
  message(STATUS "FujiNet runtime not built yet; run tools/fujinet/build-fujinet-desktop.sh for the full experience")
endif()

# Application icon (from the Android app's launcher art), installed under
# both desktop-entry ids.
install(FILES data/icons/fujinet-go-adam.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/192x192/apps
        RENAME online.fujinet.go.adam.gnome.png)
install(FILES data/icons/fujinet-go-adam.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/192x192/apps
        RENAME online.fujinet.go.adam.kde.png)

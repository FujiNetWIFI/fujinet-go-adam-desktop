# KDE/Qt frontend: plain Qt6 Widgets (picks up Breeze via the platform
# theme; no hard KDE Frameworks dependency, which keeps the future Windows
# port clean), with an embedded QtWebEngine view of the FujiNet web admin
# when available (WITH_WEBVIEW). Found-or-skipped.

find_package(Qt6 6.4 COMPONENTS Widgets OpenGLWidgets QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

add_executable(fujinet-go-adam-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-adam-kde PRIVATE
  adamsession Qt6::Widgets Qt6::OpenGLWidgets)
target_compile_options(fujinet-go-adam-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-adam-kde PRIVATE
  ADAM_ICON_FILE="${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-adam.png")

if(WITH_WEBVIEW)
  find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-adam-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-adam-kde PRIVATE HAVE_WEBENGINE)
  else()
    message(STATUS "KDE frontend: Qt6 WebEngine not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-adam-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/online.fujinet.go.adam.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

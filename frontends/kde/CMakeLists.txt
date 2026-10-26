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
  KeyForward.cpp
  keypad/KeypadWindow.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-adam-kde PRIVATE
  adamsession Qt6::Widgets Qt6::OpenGLWidgets)
target_compile_options(fujinet-go-adam-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-adam-kde PRIVATE
  ADAM_ICON_FILE="${CMAKE_SOURCE_DIR}/data/icons/hicolor/256x256/apps/fujinet-go-adam.png")

if(WITH_WEBVIEW)
  find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-adam-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-adam-kde PRIVATE HAVE_WEBENGINE)
  else()
    message(STATUS "KDE frontend: Qt6 WebEngine not found -- FujiNet config opens in the browser")
  endif()
endif()

# The key translation and the Controllers window, on the offscreen platform.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(keypad_test keypad_test.cpp KeyForward.cpp
                 keypad/KeypadWindow.cpp)
  target_link_libraries(keypad_test PRIVATE adamsession Qt6::Widgets Qt6::Test)
  target_include_directories(keypad_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(keypad_test PRIVATE -Wall -Wextra)
  add_test(NAME keypad_kde COMMAND keypad_test)
  set_tests_properties(keypad_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# The Debugger window (and F12 from the main window), on the offscreen
# platform, against a running ColecoVision session with a hand-assembled
# cartridge.
if(Qt6Test_FOUND)
  add_executable(debugger_test debugger_test.cpp FujiNetWindows.cpp
                 debugger/DebuggerWindow.cpp MainWindow.cpp DisplayWidget.cpp
                 SettingsDialog.cpp KeyForward.cpp keypad/KeypadWindow.cpp)
  target_link_libraries(debugger_test PRIVATE adamsession Qt6::Widgets
                        Qt6::OpenGLWidgets Qt6::Test)
  target_include_directories(debugger_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(debugger_test PRIVATE -Wall -Wextra)
  add_test(NAME debugger_kde COMMAND debugger_test)
  set_tests_properties(debugger_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

set(KDE_APP_ID online.fujinet.go.adam.kde)

install(TARGETS fujinet-go-adam-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${KDE_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${KDE_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.metainfo.xml")
endif()

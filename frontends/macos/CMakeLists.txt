# macOS frontend: native AppKit (Objective-C) over the shared session core.
# Built only on Apple hosts; verified by the macOS CI job.

if(NOT APPLE)
  return()
endif()

enable_language(OBJC)

add_executable(fujinet-go-adam-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  debugger/DebuggerWindow.m
)
set_target_properties(fujinet-go-adam-macos PROPERTIES
  OUTPUT_NAME "FujiNet Go Adam"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)
target_link_libraries(fujinet-go-adam-macos PRIVATE
  adamsession
  "-framework Cocoa"
  "-framework CoreVideo"
  "-framework QuartzCore"
  "-framework UniformTypeIdentifiers"
  "-framework WebKit"
)
target_compile_options(fujinet-go-adam-macos PRIVATE -Wall -Wextra -fobjc-arc)

# When the FujiNet runtime has been built (tools/fujinet/
# build-fujinet-desktop.sh on a Mac, as CI does), pack it into the bundle:
# the dylib into Contents/Frameworks, the pristine runtime tree into
# Contents/Resources/fujinet (main.m hands both paths to the session).
set(FUJINET_OUT "${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
if(EXISTS "${FUJINET_OUT}/libfujinet.dylib")
  add_custom_command(TARGET fujinet-go-adam-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-adam-macos>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/libfujinet.dylib"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-adam-macos>/Frameworks/"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/fnconfig.ini"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-adam-macos>/Resources/fujinet/fnconfig.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-adam-macos>/Resources/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/SD"
            "$<TARGET_BUNDLE_CONTENT_DIR:fujinet-go-adam-macos>/Resources/fujinet/SD"
    COMMENT "Bundling the FujiNet runtime into the app"
  )
else()
  message(STATUS "macOS frontend: no libfujinet.dylib in tools/fujinet/work/out; the app will run without FujiNet")
endif()

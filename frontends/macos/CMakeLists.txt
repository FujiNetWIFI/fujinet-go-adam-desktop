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

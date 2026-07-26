# Cross-compile the Windows frontend from Linux with mingw-w64.
#
# The maintainer has no Windows machine, so this is how Windows changes get
# compiled and linked before CI (which builds natively under MSYS2/UCRT64)
# ever sees them. Wine can then run the result for a smoke test.
#
#   # once: SDL3 for the cross target
#   git clone --depth 1 --branch release-3.4.12 \
#       https://github.com/libsdl-org/SDL.git /tmp/sdl3-src
#   cmake -S /tmp/sdl3-src -B /tmp/sdl3-src/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_INSTALL_PREFIX=/tmp/sdl3-mingw \
#       -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
#   cmake --build /tmp/sdl3-src/build && cmake --install /tmp/sdl3-src/build
#
#   cmake -B build-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_PREFIX_PATH=/tmp/sdl3-mingw
#   cmake --build build-win

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

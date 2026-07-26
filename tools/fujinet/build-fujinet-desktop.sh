#!/usr/bin/env bash
#
# Build the FujiNet ADAM runtime as a native Linux shared library
# (libfujinet.so) plus its default runtime assets (fnconfig.ini / data / SD)
# for the desktop apps.
#
# Sibling of the Android app's tools/fujinet/build-fujinet.sh, drastically
# simpler because this is a native build: no NDK, no cross-compiled mbedTLS,
# no bionic workarounds. System deps (expat, cjson bundled, smb2/ssh/nfs
# bundled, mongoose bundled) come from the stock fujinet-pc build.
#
# The staged copy gets three adaptations, keyed on -DFUJINET_EMBEDDED=ON:
#   * fujinet_pc.cmake builds a SHARED library including the desktop entry
#     wrapper (support/fujinet_desktop_entry.cpp) instead of an executable.
#   * fnSystem gains clear_shutdown_request() and the reboot() path asks the
#     service loop to stop instead of calling exit() (which would kill the
#     host app).
#   * pc_rtos worker threads are named after their FreeRTOS task for
#     debuggable core dumps (optional patch).
#
# fnconfig.ini is forced to [BOIP] enabled=1 host=127.0.0.1 port=65216 so the
# in-process runtime connects to the in-process emulator on loopback.
#
# The build system runs this itself (cmake/FujiNetRuntime.cmake); running it
# by hand is only needed to refresh an existing tree. Environment knobs:
#   FUJINET_SRC=/path   build from that checkout instead of the submodule
#   FN_REFRESH=1        re-stage + re-patch even if the staged tree is current
#   FN_CLEAN=1          throw the FujiNet build directory away first
#   FN_PATCH_ONLY=1     stage and patch, but do not build

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/../.." &>/dev/null && pwd)
SUPPORT_DIR="${SCRIPT_DIR}/support"
WORK_ROOT="${SCRIPT_DIR}/work"
CLONE_DIR="${WORK_ROOT}/fujinet-firmware"
OUT_DIR="${WORK_ROOT}/out"
STAGE_STAMP="${WORK_ROOT}/.staged-from"

# FujiNet sources: the third_party/fujinet-firmware submodule, pinned in
# cmake/Dependencies.cmake (override with FUJINET_SRC=/path to build from a
# working checkout).
FUJINET_SRC="${FUJINET_SRC:-${ROOT_DIR}/third_party/fujinet-firmware}"
PC_TARGET="ADAM"

# The shared library's name follows the host platform (the same script
# builds libfujinet.dylib on macOS, e.g. in the CI job that assembles the
# Mac app bundle).
case "$(uname -s)" in
    Darwin) LIBNAME="libfujinet.dylib" ;;
    *)      LIBNAME="libfujinet.so" ;;
esac

# FujiNet needs mbedTLS 3.x (Homebrew's formula moved to 4.x, and mbedTLS 4
# drops the legacy mbedtls/md5.h fujinet includes). Use a system install when
# it is a usable 3.x -- Linux distributions ship one -- and otherwise build
# the same pinned 3.6.5 the Android app uses, with pthread threading enabled
# because fujinet drives TLS from several threads. MBEDTLS_ROOT_DIR from the
# caller always wins (that is how the flatpak passes /app).
MBEDTLS_TAG="mbedtls-3.6.5"
MBEDTLS_SOURCE_DIR="${WORK_ROOT}/mbedtls-src"
MBEDTLS_BUILD_DIR="${WORK_ROOT}/mbedtls-build"
MBEDTLS_INSTALL_DIR="${WORK_ROOT}/mbedtls-install"

# A system mbedTLS is usable when it is 3.x (still has mbedtls/md5.h) and
# ships the static libraries fujinet links against.
system_mbedtls_usable() {
    local inc lib
    for inc in /usr/include /usr/local/include; do
        [[ -f "${inc}/mbedtls/build_info.h" ]] || continue
        grep -q '#define MBEDTLS_VERSION_MAJOR *3' "${inc}/mbedtls/build_info.h" \
            || continue
        [[ -f "${inc}/mbedtls/md5.h" ]] || continue
        for lib in /usr/lib /usr/lib64 /usr/local/lib "/usr/lib/$(uname -m)-linux-gnu"; do
            [[ -f "${lib}/libmbedtls.a" ]] && return 0
        done
    done
    return 1
}

ensure_mbedtls() {
    if [[ -n "${MBEDTLS_ROOT_DIR:-}" ]]; then
        echo "Using caller-provided MBEDTLS_ROOT_DIR=${MBEDTLS_ROOT_DIR}"
        return 0
    fi
    if [[ "$(uname -s)" != "Darwin" ]] && system_mbedtls_usable; then
        echo "Using the system mbedTLS 3.x"
        return 0
    fi
    if [[ ! -f "${MBEDTLS_INSTALL_DIR}/lib/libmbedtls.a" ]]; then
        echo "Building pinned mbedTLS ${MBEDTLS_TAG}"
        rm -rf "${MBEDTLS_SOURCE_DIR}" "${MBEDTLS_BUILD_DIR}" "${MBEDTLS_INSTALL_DIR}"
        git clone --depth 1 --branch "${MBEDTLS_TAG}" --recurse-submodules \
            --shallow-submodules https://github.com/Mbed-TLS/mbedtls.git \
            "${MBEDTLS_SOURCE_DIR}"
        python3 - "${MBEDTLS_SOURCE_DIR}/include/mbedtls/mbedtls_config.h" <<'PY'
from pathlib import Path
import sys
config_h = Path(sys.argv[1])
text = config_h.read_text()
for old, new in (
    ('//#define MBEDTLS_THREADING_C\n', '#define MBEDTLS_THREADING_C\n'),
    ('//#define MBEDTLS_THREADING_PTHREAD\n', '#define MBEDTLS_THREADING_PTHREAD\n'),
):
    if old in text:
        text = text.replace(old, new)
config_h.write_text(text)
PY
        cmake -S "${MBEDTLS_SOURCE_DIR}" -B "${MBEDTLS_BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${MBEDTLS_INSTALL_DIR}" \
            -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
            -DMBEDTLS_FATAL_WARNINGS=OFF \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
            -DLINK_WITH_PTHREAD=ON
        cmake --build "${MBEDTLS_BUILD_DIR}" --parallel
        cmake --install "${MBEDTLS_BUILD_DIR}"
    fi
    export MBEDTLS_ROOT_DIR="${MBEDTLS_INSTALL_DIR}"
}

fail() {
    echo "build-fujinet-desktop.sh: $*" >&2
    exit 1
}

# Identity of what is staged: the source commit (or, for a dirty working
# checkout, its content-free "unknown" marker) plus this script and the entry
# wrapper, since both rewrite the staged tree.
stage_identity() {
    local commit
    commit="$(git -C "${FUJINET_SRC}" rev-parse HEAD 2>/dev/null || echo local)"
    if [[ -n "$(git -C "${FUJINET_SRC}" status --porcelain 2>/dev/null)" ]]; then
        commit="${commit}-dirty-$(date +%s)"
    fi
    printf '%s %s %s\n' "${FUJINET_SRC}" "${commit}" \
        "$(cat "${BASH_SOURCE[0]}" "${SUPPORT_DIR}/fujinet_desktop_entry.cpp" \
           | cksum | cut -d' ' -f1)"
}

staging_current() {
    [[ -f "${CLONE_DIR}/build.sh" && -f "${STAGE_STAMP}" ]] || return 1
    [[ "${FN_REFRESH:-0}" -eq 1 ]] && return 1
    [[ "$(cat "${STAGE_STAMP}")" == "$(stage_identity)" ]]
}

stage_local_source() {
    [[ -d "${FUJINET_SRC}" ]] || fail \
        "FujiNet sources not found at ${FUJINET_SRC}
       run: git submodule update --init third_party/fujinet-firmware
       (or set FUJINET_SRC=/path/to/fujinet-firmware)"
    [[ -f "${FUJINET_SRC}/build.sh" ]] || fail "build.sh missing under ${FUJINET_SRC}"

    mkdir -p "${WORK_ROOT}"
    # The FujiNet build directory lives inside the staged tree and is excluded
    # from the sync (and so protected from --delete), which is what keeps
    # rebuilds incremental.
    #
    # .git is left behind deliberately. A submodule checkout's .git is a
    # gitlink *file* holding a path relative to the superproject, which points
    # nowhere once copied here -- and the fujinet build's version step treats
    # a failing `git describe` as fatal. The version is passed in through the
    # environment instead (see fujinet_version_env), so the staged tree needs
    # no git metadata at all.
    if command -v rsync >/dev/null 2>&1; then
        rsync -a --delete \
            --exclude 'build/' --exclude 'dist/' --exclude '.git' \
            "${FUJINET_SRC}/" "${CLONE_DIR}/"
    else
        # No rsync (the flatpak SDK, for one): copy through tar, preserving
        # the existing build directory.
        mkdir -p "${CLONE_DIR}"
        tar -C "${FUJINET_SRC}" --exclude=./build --exclude=./dist \
            --exclude=./.git -cf - . | tar -C "${CLONE_DIR}" -xf -
    fi
    rm -rf "${CLONE_DIR}/.git"
}

# The staged copy has no git metadata, so hand the fujinet version scripts
# the description of the real source checkout (they prefer these over
# calling git; see the version_common.py patch below).
fujinet_version_env() {
    export FUJINET_BUILD_DESCRIBE="$(
        git -C "${FUJINET_SRC}" describe --always HEAD 2>/dev/null || true)"
    export FUJINET_BUILD_SHA="$(
        git -C "${FUJINET_SRC}" rev-parse HEAD 2>/dev/null || true)"
    if [[ -n "$(git -C "${FUJINET_SRC}" status --porcelain 2>/dev/null)" ]]; then
        export FUJINET_BUILD_DIRTY=1
    else
        export FUJINET_BUILD_DIRTY=0
    fi
}

apply_desktop_patches() {
    python3 - "${CLONE_DIR}" "${SUPPORT_DIR}" <<'PY'
from pathlib import Path
import sys

clone_dir = Path(sys.argv[1])
support_dir = Path(sys.argv[2])

def write_if_changed(path, text):
    """Leave the mtime alone when nothing moved, so the FujiNet build stays
    incremental across re-stagings."""
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)


def patch(rel, transforms, required=True):
    p = clone_dir / rel
    if not p.exists():
        if required:
            sys.exit(f"build-fujinet-desktop.sh: expected file missing: {rel}")
        return
    text = p.read_text()
    for old, new, *opt in transforms:
        count = opt[0] if opt else 1
        if old not in text:
            # Idempotent: skip when the result is already present.
            if new in text:
                continue
            sys.exit(f"build-fujinet-desktop.sh: patch anchor not found in {rel}:\n---\n{old[:200]}\n---")
        text = text.replace(old, new, count)
    write_if_changed(p, text)

# --- build.sh: inject the embedded-build cmake args (getopts rejects any
# --- pass-through -D flags, so they ride an environment variable) ---------
patch("build.sh", [
    (
        '  export PROJECT_CONFIG=$INI_FILE\n  GEN_CMD=""\n',
        '  export PROJECT_CONFIG=$INI_FILE\n'
        '  CMAKE_EXTRA_ARGS=()\n'
        '  if [ -n "${FUJINET_EMBEDDED:-}" ] ; then\n'
        '    # WITH_SYMBOL_VERSIONING=OFF: libssh otherwise attaches its version\n'
        '    # script (local: *) to the shared library link, which would hide the\n'
        '    # fujinet_desktop_* entry points from dlsym.\n'
        '    CMAKE_EXTRA_ARGS+=("-DFUJINET_EMBEDDED=ON" "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" "-DWITH_SYMBOL_VERSIONING=OFF")\n'
        '    if [ "$(uname -s)" = "Darwin" ] ; then\n'
        '      # Static libcrypto keeps the shipped dylib free of Homebrew\n'
        '      # paths; Linux stays on the system OpenSSL shared library.\n'
        '      CMAKE_EXTRA_ARGS+=("-DOPENSSL_USE_STATIC_LIBS=ON")\n'
        '    fi\n'
        '  fi\n'
        '  GEN_CMD=""\n',
    ),
    (
        '    cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
        '    cmake .. "${CMAKE_EXTRA_ARGS[@]}" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
    ),
    # PC_BUILD is never assigned anywhere in build.sh -- the guard was meant
    # to be PC_TARGET -- so a PC build always tries to pip-install
    # PlatformIO, which it does not need and which fails outright in a build
    # sandbox with no network.
    (
        'if [ -z "${PC_BUILD}" ] ; then\n',
        'if [ -z "${PC_TARGET}" ] ; then\n',
    ),
    (
        '    cmake "$GEN_CMD" .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
        '    cmake "$GEN_CMD" .. "${CMAKE_EXTRA_ARGS[@]}" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
    ),
    (
        '    cmake .. -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE "$@"\n',
        '    cmake .. "${CMAKE_EXTRA_ARGS[@]}" -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE "$@"\n',
    ),
    (
        '    cmake "$GEN_CMD" .. -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE "$@"\n',
        '    cmake "$GEN_CMD" .. "${CMAKE_EXTRA_ARGS[@]}" -DFUJINET_TARGET=$PC_TARGET -DCMAKE_BUILD_TYPE=$BUILD_TYPE "$@"\n',
    ),
])

# --- version_common.py: work without git ----------------------------------
# The staged tree carries no .git (see stage_local_source), and a submodule
# checkout could not provide usable metadata here anyway. Two of the helpers
# let a git failure escape and abort the build; make them fall back, and let
# the caller pass the real description through the environment so the version
# banner still names the upstream commit.
patch("version_common.py", [
    (
        'import re\n'
        'import subprocess\n',
        'import os\n'
        'import re\n'
        'import subprocess\n',
    ),
    (
        'def run_command(cmd):\n'
        '  return subprocess.check_output(cmd, universal_newlines=True).strip()\n',
        'def run_command(cmd):\n'
        '  return subprocess.check_output(cmd, universal_newlines=True).strip()\n'
        '\n'
        'def _try_command(cmd, default=""):\n'
        '  """[fujinet-go-adam-desktop] git-optional variant: the desktop build\n'
        '  compiles a staged copy of this tree that has no git metadata."""\n'
        '  try:\n'
        '    return run_command(cmd)\n'
        '  except (FileNotFoundError, subprocess.CalledProcessError):\n'
        '    return default\n',
    ),
    (
        '  files = run_command(["git", "diff", "--name-only"]).split("\\n")\n',
        '  # Without .git here, plain git would answer for whatever repository\n'
        '  # encloses the staged tree; the caller reports the real state.\n'
        '  dirty = os.environ.get("FUJINET_BUILD_DIRTY")\n'
        '  if dirty is not None:\n'
        '    return ["(source checkout)"] if dirty == "1" else []\n'
        '  files = _try_command(["git", "diff", "--name-only"]).split("\\n")\n',
    ),
    (
        '  """raw output of `git describe --always HEAD`"""\n'
        '  return run_command(["git", "describe", "--always", "HEAD"])\n',
        '  """raw output of `git describe --always HEAD`"""\n'
        '  env = os.environ.get("FUJINET_BUILD_DESCRIBE")\n'
        '  if env:\n'
        '    return env\n'
        '  # "0000000" parses as the no-reachable-tag case rather than\n'
        '  # dropping an unparsable string into the version string.\n'
        '  return _try_command(["git", "describe", "--always", "HEAD"],\n'
        '                      "0000000")\n',
    ),
    (
        'def get_commit_sha(short=False):\n'
        '  cmd = ["git", "rev-parse", "HEAD"]\n',
        'def get_commit_sha(short=False):\n'
        '  env = os.environ.get("FUJINET_BUILD_SHA")\n'
        '  if env:\n'
        '    return env[:7] if short else env\n'
        '  cmd = ["git", "rev-parse", "HEAD"]\n',
    ),
])

# --- fujinet_pc.cmake: SHARED target with the desktop entry wrapper -------
# Export control per linker: on Linux a version script exports only the
# fujinet_* entry points and makes every other symbol local/non-preemptible
# (required both for a clean dlsym surface and because the system's static
# mbedtls is built without -fPIC: its PC32 relocations only link when the
# referenced symbols are local). On macOS the equivalent is an
# -exported_symbols_list (Mach-O is always PIC, so only the API surface
# matters there).
patch("fujinet_pc.cmake", [
    (
        'add_executable(fujinet ${SOURCES})\n',
        'if(FUJINET_EMBEDDED)\n'
        '    add_library(fujinet SHARED ${SOURCES} desktop/fujinet_desktop_entry.cpp)\n'
        '    set_target_properties(fujinet PROPERTIES OUTPUT_NAME "fujinet")\n'
        '    target_compile_definitions(fujinet PRIVATE FUJINET_EMBEDDED=1)\n'
        '    if(APPLE)\n'
        '        target_link_options(fujinet PRIVATE\n'
        '            "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/desktop/fujinet_embedded.exp")\n'
        '    else()\n'
        '        target_link_options(fujinet PRIVATE\n'
        '            "-Wl,--version-script=${CMAKE_SOURCE_DIR}/desktop/fujinet_embedded.map")\n'
        '    endif()\n'
        'else()\n'
        '    add_executable(fujinet ${SOURCES})\n'
        'endif()\n',
    ),
])

# --- mbedTLS resolution: honor MBEDTLS_ROOT_DIR explicitly ----------------
# find_library does not descend into <root>/lib from a HINTS directory, so
# a caller-provided root (the pinned 3.6.5 the macOS build makes) must be
# resolved by explicit paths -- the same approach the Android build uses.
# Without the env var (Linux: system mbedtls) the stock search runs.
patch("fujinet_pc.cmake", [
    (
        'set(_MBEDTLS_ROOT_HINTS $ENV{MBEDTLS_ROOT_DIR} ${MBEDTLS_ROOT_DIR})\n'
        'set(_MBEDTLS_ROOT_PATHS "$ENV{PROGRAMFILES}/libmbedtls")\n'
        'set(_MBEDTLS_ROOT_HINTS_AND_PATHS HINTS ${_MBEDTLS_ROOT_HINTS} PATHS ${_MBEDTLS_ROOT_PATHS})\n'
        'find_library(MBEDTLS_STATIC_LIB libmbedtls.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        'find_library(MBEDX509_STATIC_LIB libmbedx509.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        'find_library(MBEDCRYPTO_STATIC_LIB libmbedcrypto.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        'find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS} PATH_SUFFIXES include)\n',
        'if(DEFINED ENV{MBEDTLS_ROOT_DIR} AND EXISTS "$ENV{MBEDTLS_ROOT_DIR}/include/mbedtls/ssl.h")\n'
        '    # The library directory under the root is whatever GNUInstallDirs\n'
        '    # chose when mbedTLS was installed there: lib, lib64 (flatpak\n'
        '    # runtime, Fedora) or a multiarch directory.\n'
        '    set(_MBEDTLS_LIBDIR "")\n'
        '    foreach(_d lib lib64 lib/${CMAKE_LIBRARY_ARCHITECTURE})\n'
        '        if(EXISTS "$ENV{MBEDTLS_ROOT_DIR}/${_d}/libmbedtls.a")\n'
        '            set(_MBEDTLS_LIBDIR "$ENV{MBEDTLS_ROOT_DIR}/${_d}")\n'
        '            break()\n'
        '        endif()\n'
        '    endforeach()\n'
        '    if(NOT _MBEDTLS_LIBDIR)\n'
        '        message(FATAL_ERROR "no libmbedtls.a under $ENV{MBEDTLS_ROOT_DIR}")\n'
        '    endif()\n'
        '    set(MBEDTLS_STATIC_LIB "${_MBEDTLS_LIBDIR}/libmbedtls.a")\n'
        '    set(MBEDX509_STATIC_LIB "${_MBEDTLS_LIBDIR}/libmbedx509.a")\n'
        '    set(MBEDCRYPTO_STATIC_LIB "${_MBEDTLS_LIBDIR}/libmbedcrypto.a")\n'
        '    set(MBEDTLS_INCLUDE_DIR "$ENV{MBEDTLS_ROOT_DIR}/include")\n'
        'else()\n'
        '    set(_MBEDTLS_ROOT_HINTS $ENV{MBEDTLS_ROOT_DIR} ${MBEDTLS_ROOT_DIR})\n'
        '    set(_MBEDTLS_ROOT_PATHS "$ENV{PROGRAMFILES}/libmbedtls")\n'
        '    set(_MBEDTLS_ROOT_HINTS_AND_PATHS HINTS ${_MBEDTLS_ROOT_HINTS} PATHS ${_MBEDTLS_ROOT_PATHS})\n'
        '    find_library(MBEDTLS_STATIC_LIB libmbedtls.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        '    find_library(MBEDX509_STATIC_LIB libmbedx509.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        '    find_library(MBEDCRYPTO_STATIC_LIB libmbedcrypto.a HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS})\n'
        '    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h HINTS ${_MBEDTLS_ROOT_HINTS_AND_PATHS} PATH_SUFFIXES include)\n'
        'endif()\n',
    ),
])
patch("components_pc/libssh/cmake/Modules/FindMbedTLS.cmake", [
    (
        'find_path(MBEDTLS_INCLUDE_DIR\n',
        '# [fujinet-go-adam-desktop] Resolve a caller-pinned mbedTLS directly;\n'
        '# see the matching block in fujinet_pc.cmake (same lib/lib64 dance).\n'
        'if(DEFINED ENV{MBEDTLS_ROOT_DIR} AND EXISTS "$ENV{MBEDTLS_ROOT_DIR}/include/mbedtls/ssl.h")\n'
        '    set(_MBEDTLS_LIBDIR "")\n'
        '    foreach(_d lib lib64 lib/${CMAKE_LIBRARY_ARCHITECTURE})\n'
        '        if(EXISTS "$ENV{MBEDTLS_ROOT_DIR}/${_d}/libmbedtls.a")\n'
        '            set(_MBEDTLS_LIBDIR "$ENV{MBEDTLS_ROOT_DIR}/${_d}")\n'
        '            break()\n'
        '        endif()\n'
        '    endforeach()\n'
        'endif()\n'
        'if(_MBEDTLS_LIBDIR)\n'
        '    set(MBEDTLS_INCLUDE_DIR "$ENV{MBEDTLS_ROOT_DIR}/include" CACHE PATH "" FORCE)\n'
        '    set(MBEDTLS_SSL_LIBRARY "${_MBEDTLS_LIBDIR}/libmbedtls.a" CACHE FILEPATH "" FORCE)\n'
        '    set(MBEDTLS_CRYPTO_LIBRARY "${_MBEDTLS_LIBDIR}/libmbedcrypto.a" CACHE FILEPATH "" FORCE)\n'
        '    set(MBEDTLS_X509_LIBRARY "${_MBEDTLS_LIBDIR}/libmbedx509.a" CACHE FILEPATH "" FORCE)\n'
        'endif()\n'
        'find_path(MBEDTLS_INCLUDE_DIR\n',
    ),
])

# --- src/main.cpp: no signal handlers / atexit when embedded --------------
# The standalone executable owns its process; in-process those signal()
# calls hijack the host app's SIGINT/SIGTERM so a plain kill shuts down the
# FujiNet service loop instead of the app, leaving a wedged half-alive
# process. The entry wrapper sequences shutdown itself.
patch("src/main.cpp", [
    (
        '    atexit(main_shutdown_handler);\n'
        '    signal(SIGINT, sighandler);\n'
        '    signal(SIGTERM, sighandler);\n',
        '#if !defined(FUJINET_EMBEDDED)\n'
        '    atexit(main_shutdown_handler);\n'
        '    signal(SIGINT, sighandler);\n'
        '    signal(SIGTERM, sighandler);\n',
    ),
    (
        '    signal(SIGHUP, sighandler);\n'
        '    signal(SIGUSR1, sighandler);\n'
        '  #endif\n',
        '    signal(SIGHUP, sighandler);\n'
        '    signal(SIGUSR1, sighandler);\n'
        '  #endif\n'
        '#endif /* !FUJINET_EMBEDDED */\n',
    ),
])

# --- fnSystem: clear_shutdown_request() + embedded reboot guard -----------
patch("lib/hardware/fnSystem.h", [
    (
        '    int request_for_shutdown();\n'
        '    int check_for_shutdown();\n',
        '    int request_for_shutdown();\n'
        '    int check_for_shutdown();\n'
        '    void clear_shutdown_request();\n',
    ),
])
patch("lib/hardware/fnSystem.cpp", [
    (
        'int SystemManager::check_for_shutdown()\n'
        '{\n'
        '    return _shutdown_requests;\n'
        '}\n',
        'int SystemManager::check_for_shutdown()\n'
        '{\n'
        '    return _shutdown_requests;\n'
        '}\n'
        'void SystemManager::clear_shutdown_request()\n'
        '{\n'
        '    _shutdown_requests = 0;\n'
        '}\n',
    ),
    (
        '        // do cleanup and exit\n'
        '        Debug_println("SystemManager::reboot - exiting ...");\n'
        '        // FN will be restarted if ended with EXIT_AND_RESTART (75)\n'
        '        exit(_reboot_code);\n',
        '        // do cleanup and exit\n'
        '        Debug_println("SystemManager::reboot - exiting ...");\n'
        '#if defined(FUJINET_EMBEDDED)\n'
        '        // The desktop app embeds FujiNet in-process; exit() would kill\n'
        '        // the app. Ask the service loop to stop so the host restarts it.\n'
        '        request_for_shutdown();\n'
        '        return;\n'
        '#else\n'
        '        // FN will be restarted if ended with EXIT_AND_RESTART (75)\n'
        '        exit(_reboot_code);\n'
        '#endif\n',
    ),
])

# --- pc_rtos task shim: name worker threads after their FreeRTOS task ------
patch("lib/compat/pc_rtos/pc_rtos.cpp", [
    (
        '#include <mutex>\n'
        '#include <thread>\n',
        '#include <mutex>\n'
        '#include <pthread.h>\n'
        '#include <thread>\n',
    ),
    (
        'static BaseType_t pc_task_create(TaskFunction_t fn, void *arg, TaskHandle_t *out_handle)\n'
        '{\n'
        '    std::thread t([fn, arg] { fn(arg); });\n'
        '    t.detach();\n',
        'static BaseType_t pc_task_create(TaskFunction_t fn, const char *name, void *arg, TaskHandle_t *out_handle)\n'
        '{\n'
        '    std::thread t([fn, arg, name] {\n'
        '        if (name && *name) {\n'
        '            char tn[16];\n'
        '            strncpy(tn, name, sizeof(tn) - 1);\n'
        '            tn[sizeof(tn) - 1] = 0;\n'
        '#if defined(__APPLE__)\n'
        '            pthread_setname_np(tn);\n'
        '#else\n'
        '            pthread_setname_np(pthread_self(), tn);\n'
        '#endif\n'
        '        }\n'
        '        fn(arg);\n'
        '    });\n'
        '    t.detach();\n',
    ),
    (
        'extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char *, uint32_t, void *arg,\n'
        '                                  UBaseType_t, TaskHandle_t *out_handle)\n'
        '{\n'
        '    return pc_task_create(fn, arg, out_handle);\n'
        '}\n',
        'extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t, void *arg,\n'
        '                                  UBaseType_t, TaskHandle_t *out_handle)\n'
        '{\n'
        '    return pc_task_create(fn, name, arg, out_handle);\n'
        '}\n',
    ),
    (
        'extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *arg,\n'
        '                                              UBaseType_t, TaskHandle_t *out_handle, BaseType_t)\n'
        '{\n'
        '    return pc_task_create(fn, arg, out_handle);\n'
        '}\n',
        'extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t, void *arg,\n'
        '                                              UBaseType_t, TaskHandle_t *out_handle, BaseType_t)\n'
        '{\n'
        '    return pc_task_create(fn, name, arg, out_handle);\n'
        '}\n',
    ),
], required=False)

# --- Drop in the desktop entry-point wrapper + export map -----------------
desktop_dir = clone_dir / "desktop"
desktop_dir.mkdir(exist_ok=True)
write_if_changed(
    desktop_dir / "fujinet_desktop_entry.cpp",
    (support_dir / "fujinet_desktop_entry.cpp").read_text(),
)
write_if_changed(
    desktop_dir / "fujinet_embedded.map",
    "{\n"
    "  global:\n"
    "    fujinet_desktop_*;\n"
    "    fujinet_android_*;\n"
    "  local: *;\n"
    "};\n",
)
# macOS equivalent (C symbols carry a leading underscore in Mach-O).
write_if_changed(
    desktop_dir / "fujinet_embedded.exp",
    "_fujinet_desktop_*\n"
    "_fujinet_android_*\n",
)
PY
}

apply_local_patch_files() {
    local patch_dir="${SCRIPT_DIR}/patches"
    [[ -d "${patch_dir}" ]] || return 0
    local patch_file
    while IFS= read -r patch_file; do
        patch -d "${CLONE_DIR}" -p1 < "${patch_file}"
    done < <(find "${patch_dir}" -maxdepth 1 -type f -name '*.patch' | sort)
}

force_boip_config() {
    python3 - "${OUT_DIR}/fnconfig.ini" <<'PY'
from pathlib import Path
import sys, re
ini = Path(sys.argv[1])
text = ini.read_text() if ini.exists() else ""
section = "[BOIP]\nenabled=1\nhost=127.0.0.1\nport=65216\n"
if re.search(r'(?im)^\[BOIP\]', text):
    text = re.sub(r'(?ims)^\[BOIP\].*?(?=^\[|\Z)', section, text)
else:
    if text and not text.endswith("\n"):
        text += "\n"
    text += "\n" + section
ini.write_text(text)
PY
}

collect_outputs() {
    local dist_dir="${CLONE_DIR}/build/dist"
    [[ -f "${dist_dir}/${LIBNAME}" ]] || fail "Expected shared library at ${dist_dir}/${LIBNAME}"
    [[ -d "${dist_dir}/data" ]] || fail "Expected FujiNet data directory at ${dist_dir}/data"
    [[ -d "${dist_dir}/SD" ]] || fail "Expected FujiNet SD directory at ${dist_dir}/SD"
    [[ -f "${dist_dir}/fnconfig.ini" ]] || fail "Expected FujiNet config at ${dist_dir}/fnconfig.ini"

    rm -rf "${OUT_DIR}"
    mkdir -p "${OUT_DIR}"
    cp "${dist_dir}/${LIBNAME}" "${OUT_DIR}/${LIBNAME}"
    cp -R "${dist_dir}/data" "${OUT_DIR}/data"
    cp -R "${dist_dir}/SD" "${OUT_DIR}/SD"
    cp "${dist_dir}/fnconfig.ini" "${OUT_DIR}/fnconfig.ini"
    force_boip_config
    printf '%s (%s)\n' "${PC_TARGET}" "$(git -C "${FUJINET_SRC}" rev-parse --short HEAD 2>/dev/null || echo local)" \
        > "${OUT_DIR}/upstream-commit.txt"
}

# The FujiNet build wants pyyaml and jinja2 in a virtualenv, and installs
# them with pip when they are missing -- which needs the network, and fails
# inside a build sandbox. Creating the venv here with --system-site-packages
# lets distribution-provided modules satisfy the check; pip only runs when
# the modules really are absent.
prepare_python_env() {
    export VENV_ROOT="${VENV_ROOT:-${WORK_ROOT}/venv}"
    if [[ ! -f "${VENV_ROOT}/bin/activate" ]]; then
        python3 -m venv --system-site-packages "${VENV_ROOT}" \
            || fail "could not create the python virtualenv at ${VENV_ROOT}"
    fi
}

# --------------------------------------------------------------------------
mkdir -p "${WORK_ROOT}"
if staging_current; then
    echo "FujiNet sources already staged from ${FUJINET_SRC} (FN_REFRESH=1 to re-stage)"
else
    rm -f "${STAGE_STAMP}"
    stage_local_source
    apply_desktop_patches
    apply_local_patch_files
    stage_identity > "${STAGE_STAMP}"
fi

if [[ "${FN_PATCH_ONLY:-0}" -eq 1 ]]; then
    echo "FN_PATCH_ONLY: staged and patched ${CLONE_DIR}; skipping build."
    exit 0
fi

ensure_mbedtls
prepare_python_env
fujinet_version_env

# Ninja and a parallel build where available: the stock invocation is a
# single-threaded make, which turns this into a coffee break.
if [[ -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null 2>&1; then
    export CMAKE_GENERATOR="Ninja"
fi
if [[ -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    export CMAKE_BUILD_PARALLEL_LEVEL="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

# -c (clean) only on request: the staged build directory is preserved across
# runs so rebuilds are incremental.
BUILD_FLAGS="-p"
[[ "${FN_CLEAN:-0}" -eq 1 ]] && BUILD_FLAGS="-cp"

# ... except a build directory made by a different generator, which cmake
# refuses to reuse.
FN_CACHE="${CLONE_DIR}/build/CMakeCache.txt"
if [[ -f "${FN_CACHE}" ]]; then
    have_gen="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${FN_CACHE}")"
    if [[ "${have_gen}" != "${CMAKE_GENERATOR:-Unix Makefiles}" ]]; then
        echo "FujiNet build directory was made with '${have_gen}'; rebuilding clean"
        BUILD_FLAGS="-cp"
    fi
fi

(
    cd "${CLONE_DIR}"
    FUJINET_EMBEDDED=1 bash ./build.sh "${BUILD_FLAGS}" "${PC_TARGET}"
)

collect_outputs

echo "FujiNet ADAM desktop runtime outputs:"
echo "  ${OUT_DIR}/${LIBNAME}"
echo "  ${OUT_DIR}/{fnconfig.ini,data,SD}"

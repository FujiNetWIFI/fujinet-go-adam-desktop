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

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
SUPPORT_DIR="${SCRIPT_DIR}/support"
WORK_ROOT="${SCRIPT_DIR}/work"
CLONE_DIR="${WORK_ROOT}/fujinet-firmware"
OUT_DIR="${WORK_ROOT}/out"

# Local fujinet-pc-adam checkout (override with FUJINET_SRC=/path ...).
FUJINET_SRC="${FUJINET_SRC:-${HOME}/Workspace/fujinet-pc-adam}"
PC_TARGET="ADAM"

# The shared library's name follows the host platform (the same script
# builds libfujinet.dylib on macOS, e.g. in the CI job that assembles the
# Mac app bundle).
case "$(uname -s)" in
    Darwin) LIBNAME="libfujinet.dylib" ;;
    *)      LIBNAME="libfujinet.so" ;;
esac

fail() {
    echo "build-fujinet-desktop.sh: $*" >&2
    exit 1
}

stage_local_source() {
    [[ -d "${FUJINET_SRC}" ]] || fail "fujinet-pc-adam source not found at ${FUJINET_SRC} (set FUJINET_SRC)"
    [[ -f "${FUJINET_SRC}/build.sh" ]] || fail "build.sh missing under ${FUJINET_SRC}"

    rm -rf "${CLONE_DIR}"
    mkdir -p "${WORK_ROOT}"
    # Keep .git: the fujinet build derives build_version.h from git describe
    # (and unlike the Android script's python, the native path treats a git
    # failure as fatal).
    rsync -a --delete \
        --exclude 'build/' --exclude 'dist/' \
        "${FUJINET_SRC}/" "${CLONE_DIR}/"
}

apply_desktop_patches() {
    python3 - "${CLONE_DIR}" "${SUPPORT_DIR}" <<'PY'
from pathlib import Path
import sys

clone_dir = Path(sys.argv[1])
support_dir = Path(sys.argv[2])

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
    p.write_text(text)

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
        '  fi\n'
        '  GEN_CMD=""\n',
    ),
    (
        '    cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
        '    cmake .. "${CMAKE_EXTRA_ARGS[@]}" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DFUJINET_TARGET=$PC_TARGET "$@"\n',
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
(desktop_dir / "fujinet_desktop_entry.cpp").write_text(
    (support_dir / "fujinet_desktop_entry.cpp").read_text()
)
(desktop_dir / "fujinet_embedded.map").write_text(
    "{\n"
    "  global:\n"
    "    fujinet_desktop_*;\n"
    "    fujinet_android_*;\n"
    "  local: *;\n"
    "};\n"
)
# macOS equivalent (C symbols carry a leading underscore in Mach-O).
(desktop_dir / "fujinet_embedded.exp").write_text(
    "_fujinet_desktop_*\n"
    "_fujinet_android_*\n"
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

# --------------------------------------------------------------------------
mkdir -p "${WORK_ROOT}"
stage_local_source
apply_desktop_patches
apply_local_patch_files

if [[ "${FN_PATCH_ONLY:-0}" -eq 1 ]]; then
    echo "FN_PATCH_ONLY: staged and patched ${CLONE_DIR}; skipping build."
    exit 0
fi

(
    cd "${CLONE_DIR}"
    FUJINET_EMBEDDED=1 bash ./build.sh -cp "${PC_TARGET}"
)

collect_outputs

echo "FujiNet ADAM desktop runtime outputs:"
echo "  ${OUT_DIR}/${LIBNAME}"
echo "  ${OUT_DIR}/{fnconfig.ini,data,SD}"

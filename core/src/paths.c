/*
 * adamsession path layout: XDG config/data directories, ROM resolution, and
 * first-run provisioning of the FujiNet runtime tree (fnconfig.ini, data/,
 * SD/) from the installed share directory or the development build output.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

/* Compiled-in development fallbacks (set by CMake to paths in the source /
 * build trees) so a git checkout runs without an install step. */
#ifndef ADAM_DEV_FUJINET_OUT
#define ADAM_DEV_FUJINET_OUT ""
#endif
#ifndef ADAM_INSTALL_DATADIR
#define ADAM_INSTALL_DATADIR ""
#endif
#ifndef ADAM_INSTALL_LIBDIR
#define ADAM_INSTALL_LIBDIR ""
#endif

int mkdir_p(const char *path)
{
    char buf[ADAM_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char from[ADAM_PATH_MAX], to[ADAM_PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            rc |= copy_tree(from, to);
        else if (S_ISREG(st.st_mode))
            rc |= copy_file(from, to);
    }
    closedir(d);
    return rc;
}

static void xdg_dir(char *dst, size_t dstsz, const char *env, const char *fallback_suffix)
{
    const char *v = getenv(env);
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-adam", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-adam", home ? home : ".",
                 fallback_suffix);
    }
}

int paths_init(adamsession *s, const adamsession_paths *p)
{
    if (p && p->config_dir && *p->config_dir)
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", p->config_dir);
    else
        xdg_dir(s->config_dir, sizeof(s->config_dir), "XDG_CONFIG_HOME", ".config");

    if (p && p->data_dir && *p->data_dir)
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", p->data_dir);
    else
        xdg_dir(s->data_dir, sizeof(s->data_dir), "XDG_DATA_HOME", ".local/share");

    if (mkdir_p(s->config_dir) != 0 || mkdir_p(s->data_dir) != 0)
        return -1;

    snprintf(s->settings_file, sizeof(s->settings_file), "%s/settings.ini",
             s->config_dir);
    snprintf(s->cart_dir, sizeof(s->cart_dir), "%s/carts", s->data_dir);
    mkdir_p(s->cart_dir);

    if (p && p->fujinet_lib) /* may be "" = explicitly disabled */
        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", p->fujinet_lib);
    else
        s->fujinet_lib[0] = '\0'; /* resolved in paths_provision_fujinet */

    snprintf(s->webui_url, sizeof(s->webui_url), "http://127.0.0.1:%d/",
             ADAMSESSION_WEBUI_PORT);
    return 0;
}

/* Locate libfujinet.so and provision the runtime tree on first run. The
 * runtime tree (fnconfig.ini + data/ + SD/) is copied from the newest
 * available source: the installed share dir or the dev build output. */
int paths_provision_fujinet(adamsession *s)
{
    const char *env;
    char src_root[ADAM_PATH_MAX];
    char probe[ADAM_PATH_MAX];

    snprintf(s->fujinet_root, sizeof(s->fujinet_root), "%s/fujinet",
             s->data_dir);
    snprintf(s->fujinet_config, sizeof(s->fujinet_config), "%s/fnconfig.ini",
             s->fujinet_root);
    snprintf(s->fujinet_sd, sizeof(s->fujinet_sd), "%s/SD", s->fujinet_root);
    snprintf(s->fujinet_data, sizeof(s->fujinet_data), "%s/data",
             s->fujinet_root);

    /* Resolve the shared library unless the caller pinned/disabled it. */
    if (!s->fujinet_lib[0]) {
        env = getenv("FUJINET_LIB");
        if (env && is_file(env)) {
            snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", env);
        } else {
            snprintf(probe, sizeof(probe), "%s/libfujinet.so",
                     ADAM_INSTALL_LIBDIR);
            if (is_file(probe)) {
                snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", probe);
            } else {
                snprintf(probe, sizeof(probe), "%s/libfujinet.so",
                         ADAM_DEV_FUJINET_OUT);
                if (is_file(probe))
                    snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s",
                             probe);
            }
        }
    }
    if (!s->fujinet_lib[0])
        return -1; /* no runtime available; session runs without FujiNet */

    /* Provision the runtime tree once (keep user data on later runs). */
    if (!is_file(s->fujinet_config)) {
        src_root[0] = '\0';
        snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                 ADAM_INSTALL_DATADIR);
        if (is_file(probe)) {
            snprintf(src_root, sizeof(src_root), "%s/fujinet",
                     ADAM_INSTALL_DATADIR);
        } else {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini",
                     ADAM_DEV_FUJINET_OUT);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s",
                         ADAM_DEV_FUJINET_OUT);
        }
        if (!src_root[0]) {
            session_set_error(s, "FujiNet runtime data not found (looked in "
                              "%s and %s)", ADAM_INSTALL_DATADIR,
                              ADAM_DEV_FUJINET_OUT);
            s->fujinet_lib[0] = '\0';
            return -1;
        }
        mkdir_p(s->fujinet_root);
        snprintf(probe, sizeof(probe), "%s/fnconfig.ini", src_root);
        copy_file(probe, s->fujinet_config);
        snprintf(probe, sizeof(probe), "%s/data", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_data);
        snprintf(probe, sizeof(probe), "%s/SD", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_sd);
    }
    mkdir_p(s->fujinet_sd);
    mkdir_p(s->fujinet_data);
    return 0;
}

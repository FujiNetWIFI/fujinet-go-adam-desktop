/*
 * adamcore's socket helpers (net.h) for Windows, over Winsock2.
 *
 * adamcore itself ships only the POSIX backend today; when a net_win32.c
 * lands in the emulator core, core/CMakeLists.txt prefers that one and this
 * file drops out of the build.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string.h>

/* adamcore's API passes sockets around as int. Winsock's SOCKET is an
 * opaque UINT_PTR, but the values Windows hands out are small kernel handle
 * indices, so the round trip through int is safe in practice -- and it is
 * how the same code is written on the POSIX side. */
#define SOCK(fd) ((SOCKET)(intptr_t)(fd))

/* net_listen() is the first net_* call any session makes (boip_init()), so
 * starting Winsock lazily here covers every caller. */
static int ensure_winsock(void)
{
    static int started;
    WSADATA wsa;

    if (started)
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
    started = 1;
    return 0;
}

static void set_nonblock(SOCKET s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

int net_listen(int port)
{
    struct sockaddr_in sa;
    SOCKET s;
    BOOL one = TRUE;

    if (ensure_winsock() != 0)
        return -1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return -1;
    /* Not SO_REUSEADDR: on Windows that permits two live binds to the same
     * port (the POSIX meaning is SO_EXCLUSIVEADDRUSE's absence), which would
     * let a second instance silently steal the BoIP port. */
    (void)one;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR ||
        listen(s, 1) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    set_nonblock(s);
    return (int)s;
}

int net_accept(int listen_fd)
{
    SOCKET s = accept(SOCK(listen_fd), NULL, NULL);
    BOOL one = TRUE;

    if (s == INVALID_SOCKET)
        return -1;
    set_nonblock(s);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return (int)s;
}

int net_read(int fd, void *buf, int n)
{
    int r = recv(SOCK(fd), (char *)buf, n, 0);

    if (r > 0)
        return r;
    if (r == 0)
        return -1; /* peer closed */
    return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
}

int net_write(int fd, const void *buf, int n)
{
    /* Loopback with TCP_NODELAY: send the packet in one call so the peer
     * never observes an inter-byte gap. Brief WSAEWOULDBLOCK retries keep
     * the write whole if the socket buffer is momentarily full. */
    const char *p = buf;
    int left = n;

    while (left > 0) {
        int w = send(SOCK(fd), p, left, 0);
        if (w > 0) {
            p += w;
            left -= w;
            continue;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        return -1;
    }
    return n;
}

void net_close(int fd)
{
    if (fd >= 0)
        closesocket(SOCK(fd));
}

uint64_t net_now_ms(void)
{
    /* Monotonic since boot, unaffected by clock changes -- the Windows
     * analog of CLOCK_MONOTONIC. */
    return (uint64_t)GetTickCount64();
}

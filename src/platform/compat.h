/*
 * compat.h - Thin portability layer: sockets, threads, locks, time, paths.
 *
 * The server targets Windows (MinGW-w64 / llvm-mingw); the POSIX branch
 * exists so the whole program can be built and tested on Linux CI too.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_COMPAT_H
#define SP410_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET             sock_t;
#  define SOCK_INVALID     INVALID_SOCKET
typedef CRITICAL_SECTION   mutex_t;
typedef CONDITION_VARIABLE cond_t;
typedef HANDLE             thread_t;
#  define PATH_SEP         '\\'
#  define strcasecmp       _stricmp
#  define strncasecmp      _strnicmp
#  define strtok_r         strtok_s
#else
#  include <pthread.h>
#  include <strings.h>
typedef int                sock_t;
#  define SOCK_INVALID     (-1)
typedef pthread_mutex_t    mutex_t;
typedef pthread_cond_t     cond_t;
typedef pthread_t          thread_t;
#  define PATH_SEP         '/'
#endif

/* sockets */
int     net_init(void);
void    net_close(sock_t s);
/* Half-close, discard what the peer is still sending (up to ms), then close.
 * Lets a client read an error response sent before its upload finished. */
void    net_linger_close(sock_t s, int ms);
int     net_send_all(sock_t s, const void *buf, size_t len);
/* recv with timeout: returns bytes (>0), 0 on orderly close, -1 error/timeout */
int     net_recv(sock_t s, void *buf, size_t len, int timeout_ms);
/* listen on addr:port (port 0 = ephemeral; *bound_port receives the real one) */
sock_t  net_listen(const char *addr, int port, int *bound_port);
/* wait up to timeout_ms for a connection; SOCK_INVALID on timeout/error */
sock_t  net_accept(sock_t ls, int timeout_ms, char *peer, size_t peerlen);
sock_t  net_connect(const char *host, int port, int timeout_ms);
/* wait until readable: >0 readable, 0 timeout, <0 error */
int     net_wait(sock_t s, int timeout_ms);
const char *net_strerror(void);

/* threads and locks */
typedef void (*thread_fn)(void *arg);
int     thread_start(thread_t *t, thread_fn fn, void *arg);
int     thread_start_detached(thread_fn fn, void *arg);
void    thread_join(thread_t t);
void    mutex_init(mutex_t *m);
void    mutex_lock(mutex_t *m);
void    mutex_unlock(mutex_t *m);
void    mutex_destroy(mutex_t *m);
void    cond_init(cond_t *c);
void    cond_signal(cond_t *c);
void    cond_broadcast(cond_t *c);
/* wait with timeout; returns 0 if signalled, 1 on timeout */
int     cond_timedwait(cond_t *c, mutex_t *m, int timeout_ms);
void    cond_destroy(cond_t *c);

/* atomic flags (GCC/Clang builtins; work on MinGW and POSIX) */
#define FLAG_GET(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define FLAG_SET(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* time */
int64_t time_ms(void);          /* monotonic milliseconds */
void    sleep_ms(int ms);

/* files */
int     read_file(const char *path, uint8_t **data, size_t *len);
/* Directory for config and logs: %ProgramData%\SP410 or /etc/sp410 */
const char *default_data_dir(void);
int     make_dir(const char *path);

#endif /* SP410_COMPAT_H */

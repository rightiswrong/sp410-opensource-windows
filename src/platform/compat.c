/*
 * compat.c - Portability layer implementation (Win32 and POSIX).
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <process.h>
#  include <direct.h>
#  include <shlobj.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
#endif

/* ---- sockets ------------------------------------------------------------ */

int
net_init(void)
{
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) ? -1 : 0;
#else
  signal(SIGPIPE, SIG_IGN);
  return 0;
#endif
}

void
net_close(sock_t s)
{
  if (s == SOCK_INVALID)
    return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

void
net_linger_close(sock_t s, int ms)
{
  char    junk[16384];
  int64_t end;

  if (s == SOCK_INVALID)
    return;
#ifdef _WIN32
  shutdown(s, SD_SEND);
#else
  shutdown(s, SHUT_WR);
#endif
  end = time_ms() + ms;
  while (time_ms() < end && net_recv(s, junk, sizeof(junk), 200) > 0)
    ;
  net_close(s);
}

const char *
net_strerror(void)
{
#ifdef _WIN32
  static __thread char buf[64];
  snprintf(buf, sizeof(buf), "winsock error %d", WSAGetLastError());
  return buf;
#else
  return strerror(errno);
#endif
}

static int
wait_fd(sock_t s, int for_write, int timeout_ms)
{
#ifdef _WIN32
  fd_set         fds;
  struct timeval tv;

  FD_ZERO(&fds);
  FD_SET(s, &fds);
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return select(0, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL,
                timeout_ms < 0 ? NULL : &tv);
#else
  struct pollfd pfd;

  pfd.fd      = s;
  pfd.events  = for_write ? POLLOUT : POLLIN;
  pfd.revents = 0;
  for (;;)
  {
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0 && errno == EINTR)
      continue;
    return r;
  }
#endif
}

int
net_wait(sock_t s, int timeout_ms)
{
  return wait_fd(s, 0, timeout_ms);
}

int
net_send_all(sock_t s, const void *buf, size_t len)
{
  const char *p = buf;

  while (len)
  {
    int chunk = len > 65536 ? 65536 : (int)len;
    int n;

    if (wait_fd(s, 1, 30000) <= 0)
      return -1;
    n = send(s, p, chunk, 0);
    if (n <= 0)
    {
#ifndef _WIN32
      if (n < 0 && errno == EINTR)
        continue;
#endif
      return -1;
    }
    p   += n;
    len -= (size_t)n;
  }
  return 0;
}

int
net_recv(sock_t s, void *buf, size_t len, int timeout_ms)
{
  int n;

  if (wait_fd(s, 0, timeout_ms) <= 0)
    return -1;
  do
    n = recv(s, buf, len > 65536 ? 65536 : (int)len, 0);
#ifndef _WIN32
  while (n < 0 && errno == EINTR);
#else
  while (0);
#endif
  return n < 0 ? -1 : n;
}

sock_t
net_listen(const char *addr, int port, int *bound_port)
{
  struct addrinfo hints, *res = NULL, *ai;
  char            portstr[16];
  sock_t          s = SOCK_INVALID;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags    = AI_PASSIVE;
  snprintf(portstr, sizeof(portstr), "%d", port);

  if (getaddrinfo(addr && *addr ? addr : NULL, portstr, &hints, &res))
    return SOCK_INVALID;

  for (ai = res; ai; ai = ai->ai_next)
  {
    int on = 1;

    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == SOCK_INVALID)
      continue;
#ifdef _WIN32
    /* Refuse to share the port with another process. */
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 && listen(s, 16) == 0)
      break;
    net_close(s);
    s = SOCK_INVALID;
  }
  freeaddrinfo(res);

  if (s != SOCK_INVALID && bound_port)
  {
    struct sockaddr_storage ss;
    socklen_t               sl = sizeof(ss);

    *bound_port = port;
    if (getsockname(s, (struct sockaddr *)&ss, &sl) == 0)
    {
      if (ss.ss_family == AF_INET)
        *bound_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
      else if (ss.ss_family == AF_INET6)
        *bound_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
  }
  return s;
}

sock_t
net_accept(sock_t ls, int timeout_ms, char *peer, size_t peerlen)
{
  struct sockaddr_storage ss;
  socklen_t               sl = sizeof(ss);
  sock_t                  s;
  int                     on = 1;

  if (wait_fd(ls, 0, timeout_ms) <= 0)
    return SOCK_INVALID;
  s = accept(ls, (struct sockaddr *)&ss, &sl);
  if (s == SOCK_INVALID)
    return SOCK_INVALID;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
  if (peer && peerlen)
  {
    peer[0] = '\0';
    getnameinfo((struct sockaddr *)&ss, sl, peer, (socklen_t)peerlen, NULL, 0, NI_NUMERICHOST);
  }
  return s;
}

sock_t
net_connect(const char *host, int port, int timeout_ms)
{
  struct addrinfo hints, *res = NULL, *ai;
  char            portstr[16];
  sock_t          s = SOCK_INVALID;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", port);
  if (getaddrinfo(host, portstr, &hints, &res))
    return SOCK_INVALID;

  for (ai = res; ai; ai = ai->ai_next)
  {
    int rc;

    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == SOCK_INVALID)
      continue;
#ifdef _WIN32
    {
      u_long nb = 1;
      ioctlsocket(s, FIONBIO, &nb);
      rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
      if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK && wait_fd(s, 1, timeout_ms) > 0)
      {
        int err = 0, el = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        rc = err ? -1 : 0;
      }
      nb = 0;
      ioctlsocket(s, FIONBIO, &nb);
    }
#else
    {
      int fl = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, fl | O_NONBLOCK);
      rc = connect(s, ai->ai_addr, ai->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS && wait_fd(s, 1, timeout_ms) > 0)
      {
        int       err = 0;
        socklen_t el  = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        rc = err ? -1 : 0;
      }
      fcntl(s, F_SETFL, fl);
    }
#endif
    if (rc == 0)
      break;
    net_close(s);
    s = SOCK_INVALID;
  }
  freeaddrinfo(res);
  return s;
}

/* ---- threads ------------------------------------------------------------- */

typedef struct { thread_fn fn; void *arg; } tstart_t;

#ifdef _WIN32
static unsigned __stdcall
trampoline(void *p)
{
  tstart_t ts = *(tstart_t *)p;
  free(p);
  ts.fn(ts.arg);
  return 0;
}
#else
static void *
trampoline(void *p)
{
  tstart_t ts = *(tstart_t *)p;
  free(p);
  ts.fn(ts.arg);
  return NULL;
}
#endif

int
thread_start(thread_t *t, thread_fn fn, void *arg)
{
  tstart_t *ts = malloc(sizeof(*ts));

  if (!ts)
    return -1;
  ts->fn  = fn;
  ts->arg = arg;
#ifdef _WIN32
  *t = (HANDLE)_beginthreadex(NULL, 0, trampoline, ts, 0, NULL);
  if (!*t)
  {
    free(ts);
    return -1;
  }
  return 0;
#else
  if (pthread_create(t, NULL, trampoline, ts))
  {
    free(ts);
    return -1;
  }
  return 0;
#endif
}

int
thread_start_detached(thread_fn fn, void *arg)
{
  thread_t t;

  if (thread_start(&t, fn, arg))
    return -1;
#ifdef _WIN32
  CloseHandle(t);
#else
  pthread_detach(t);
#endif
  return 0;
}

void
thread_join(thread_t t)
{
#ifdef _WIN32
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
#else
  pthread_join(t, NULL);
#endif
}

#ifdef _WIN32
void mutex_init(mutex_t *m)    { InitializeCriticalSection(m); }
void mutex_lock(mutex_t *m)    { EnterCriticalSection(m); }
void mutex_unlock(mutex_t *m)  { LeaveCriticalSection(m); }
void mutex_destroy(mutex_t *m) { DeleteCriticalSection(m); }
void cond_init(cond_t *c)      { InitializeConditionVariable(c); }
void cond_signal(cond_t *c)    { WakeConditionVariable(c); }
void cond_broadcast(cond_t *c) { WakeAllConditionVariable(c); }
void cond_destroy(cond_t *c)   { (void)c; }
int
cond_timedwait(cond_t *c, mutex_t *m, int timeout_ms)
{
  return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 0 : 1;
}
#else
void mutex_init(mutex_t *m)    { pthread_mutex_init(m, NULL); }
void mutex_lock(mutex_t *m)    { pthread_mutex_lock(m); }
void mutex_unlock(mutex_t *m)  { pthread_mutex_unlock(m); }
void mutex_destroy(mutex_t *m) { pthread_mutex_destroy(m); }
void cond_init(cond_t *c)
{
  pthread_condattr_t a;
  pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(c, &a);
  pthread_condattr_destroy(&a);
}
void cond_signal(cond_t *c)    { pthread_cond_signal(c); }
void cond_broadcast(cond_t *c) { pthread_cond_broadcast(c); }
void cond_destroy(cond_t *c)   { pthread_cond_destroy(c); }
int
cond_timedwait(cond_t *c, mutex_t *m, int timeout_ms)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec  += timeout_ms / 1000;
  ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
  {
    ts.tv_sec ++;
    ts.tv_nsec -= 1000000000L;
  }
  return pthread_cond_timedwait(c, m, &ts) ? 1 : 0;
}
#endif

/* ---- time ---------------------------------------------------------------- */

int64_t
time_ms(void)
{
#ifdef _WIN32
  return (int64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void
sleep_ms(int ms)
{
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  while (nanosleep(&ts, &ts) && errno == EINTR)
    ;
#endif
}

/* ---- files ---------------------------------------------------------------- */

int
read_file(const char *path, uint8_t **data, size_t *len)
{
  FILE    *fp = fopen(path, "rb");
  uint8_t *buf = NULL;
  size_t   cap = 0, n = 0, r;

  *data = NULL;
  *len  = 0;
  if (!fp)
    return -1;
  do
  {
    if (n == cap)
    {
      uint8_t *nb;
      cap = cap ? cap * 2 : 65536;
      if ((nb = realloc(buf, cap)) == NULL)
      {
        free(buf);
        fclose(fp);
        return -1;
      }
      buf = nb;
    }
    r  = fread(buf + n, 1, cap - n, fp);
    n += r;
  }
  while (r > 0);
  fclose(fp);
  *data = buf;
  *len  = n;
  return 0;
}

const char *
default_data_dir(void)
{
#ifdef _WIN32
  static char dir[MAX_PATH + 16];
  if (!dir[0])
  {
    char base[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, base) != S_OK)
      strcpy(base, "C:\\ProgramData");
    snprintf(dir, sizeof(dir), "%s\\SP410", base);
  }
  return dir;
#else
  return "/etc/sp410";
#endif
}

int
make_dir(const char *path)
{
#ifdef _WIN32
  return (_mkdir(path) == 0 || errno == EEXIST) ? 0 : -1;
#else
  return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

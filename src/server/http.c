/*
 * http.c - Minimal HTTP/1.1 server side.
 *
 * Handles what IPP clients (Windows' IPP Class Driver, CUPS, ipptool) use:
 * POST with Content-Length or chunked Transfer-Encoding, "Expect:
 * 100-continue", persistent connections, and simple GET pages.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_TIMEOUT_MS 30000

static int
fill(http_conn_t *c, int timeout_ms)
{
  int n;

  if (c->off && c->off == c->have)
    c->off = c->have = 0;
  if (c->have == sizeof(c->buf))
  {
    if (!c->off)
      return -1;
    memmove(c->buf, c->buf + c->off, c->have - c->off);
    c->have -= c->off;
    c->off   = 0;
  }
  n = net_recv(c->sock, c->buf + c->have, sizeof(c->buf) - c->have, timeout_ms);
  if (n <= 0)
    return n == 0 ? 0 : -1;
  c->have += (size_t)n;
  return n;
}

/* Read a CRLF-terminated line (without the terminator). */
static int
read_line(http_conn_t *c, char *line, size_t size, int timeout_ms)
{
  for (;;)
  {
    char *start = c->buf + c->off;
    char *nl    = memchr(start, '\n', c->have - c->off);

    if (nl)
    {
      size_t n = (size_t)(nl - start);

      if (n && start[n - 1] == '\r')
        n --;
      if (n >= size)
        return -1;
      memcpy(line, start, n);
      line[n] = '\0';
      c->off = (size_t)(nl - c->buf) + 1;
      return 0;
    }
    if (c->have - c->off >= size)
      return -1;
    if (fill(c, timeout_ms) <= 0)
      return -2;
  }
}

static int
read_exact(http_conn_t *c, uint8_t *dst, size_t n)
{
  while (n)
  {
    size_t avail = c->have - c->off;

    if (!avail)
    {
      if (fill(c, IO_TIMEOUT_MS) <= 0)
        return -1;
      continue;
    }
    if (avail > n)
      avail = n;
    if (dst)
    {
      memcpy(dst, c->buf + c->off, avail);
      dst += avail;
    }
    c->off += avail;
    n      -= avail;
  }
  return 0;
}

static int
grow(http_req_t *req, size_t *cap, size_t need, size_t max_body)
{
  uint8_t *nb;
  size_t   nc = *cap ? *cap : 65536;

  if (need > max_body)
    return HTTP_TOO_LARGE;
  while (nc < need + 1)
    nc *= 2;
  if (nc > max_body + 1)
    nc = max_body + 1;
  if (nc <= *cap)
    return 0;
  if ((nb = realloc(req->body, nc)) == NULL)
    return HTTP_ERROR;
  req->body = nb;
  *cap      = nc;
  return 0;
}

static void
trim(char *s)
{
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
}

int
http_read_request(http_conn_t *c, http_req_t *req, size_t max_body, int idle_ms)
{
  char   line[8192], version[16];
  long long content_length = -1;
  int    chunked = 0, expect_continue = 0, conn_close = 0, conn_keep = 0, rc, headers = 0;
  size_t cap = 0;

  memset(req, 0, sizeof(*req));

  /* request line (skip stray CRLFs between pipelined requests) */
  do
  {
    rc = read_line(c, line, sizeof(line), idle_ms);
    if (rc == -2)
      return HTTP_CLOSED;
    if (rc)
      return HTTP_BAD;
  }
  while (!line[0]);

  if (sscanf(line, "%15s %511s %15s", req->method, req->path, version) != 3 ||
      strncmp(version, "HTTP/1.", 7))
    return HTTP_BAD;
  req->http_minor = version[7] == '0' ? 0 : 1;

  /* headers */
  for (;;)
  {
    char *colon, *val;

    if (read_line(c, line, sizeof(line), IO_TIMEOUT_MS))
      return HTTP_BAD;
    if (!line[0])
      break;
    if (++ headers > 100 || (colon = strchr(line, ':')) == NULL)
      return HTTP_BAD;
    *colon = '\0';
    val = colon + 1;
    while (*val == ' ' || *val == '\t')
      val ++;
    trim(val);

    if (!strcasecmp(line, "Content-Length"))
    {
      char *end;
      content_length = strtoll(val, &end, 10);
      if (*end || content_length < 0)
        return HTTP_BAD;
    }
    else if (!strcasecmp(line, "Transfer-Encoding"))
      chunked = strstr(val, "chunked") != NULL;
    else if (!strcasecmp(line, "Expect"))
      expect_continue = !strcasecmp(val, "100-continue");
    else if (!strcasecmp(line, "Connection"))
    {
      conn_close = strstr(val, "close") != NULL || strstr(val, "Close") != NULL;
      conn_keep  = strstr(val, "keep-alive") != NULL || strstr(val, "Keep-Alive") != NULL;
    }
    else if (!strcasecmp(line, "Host"))
      snprintf(req->host, sizeof(req->host), "%s", val);
    else if (!strcasecmp(line, "Content-Type"))
      snprintf(req->content_type, sizeof(req->content_type), "%s", val);
  }

  req->keep_alive = req->http_minor ? !conn_close : conn_keep;

  if (expect_continue && (chunked || content_length > 0))
  {
    if ((chunked ? 0 : (size_t)content_length > max_body))
    {
      req->keep_alive = 0;
      return HTTP_TOO_LARGE;
    }
    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (net_send_all(c->sock, cont, sizeof(cont) - 1))
      return HTTP_ERROR;
  }

  /* body */
  if (chunked)
  {
    for (;;)
    {
      unsigned long long sz;
      char *end;

      if (read_line(c, line, sizeof(line), IO_TIMEOUT_MS))
        return HTTP_BAD;
      sz = strtoull(line, &end, 16);
      if (end == line)
        return HTTP_BAD;
      if (sz == 0)
        break;
      if ((rc = grow(req, &cap, req->body_len + sz, max_body)) != 0)
      {
        req->keep_alive = 0;
        return rc;
      }
      if (read_exact(c, req->body + req->body_len, (size_t)sz) ||
          read_line(c, line, sizeof(line), IO_TIMEOUT_MS) || line[0])
        return HTTP_BAD;
      req->body_len += (size_t)sz;
    }
    /* trailers */
    do
      if (read_line(c, line, sizeof(line), IO_TIMEOUT_MS))
        return HTTP_BAD;
    while (line[0]);
  }
  else if (content_length > 0)
  {
    if ((rc = grow(req, &cap, (size_t)content_length, max_body)) != 0)
    {
      req->keep_alive = 0;
      return rc;
    }
    if (read_exact(c, req->body, (size_t)content_length))
      return HTTP_ERROR;
    req->body_len = (size_t)content_length;
  }

  if (req->body)
    req->body[req->body_len] = 0;
  return HTTP_OK;
}

void
http_req_free(http_req_t *req)
{
  free(req->body);
  req->body = NULL;
}

int
http_respond(http_conn_t *c, int status, const char *reason, const char *ctype,
             const void *body, size_t len, int keep_alive)
{
  char hdr[512];
  int  n;

  n = snprintf(hdr, sizeof(hdr),
               "HTTP/1.1 %d %s\r\n"
               "Server: sp410-ippd\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %zu\r\n"
               "Cache-Control: no-store\r\n"
               "Connection: %s\r\n\r\n",
               status, reason, ctype ? ctype : "text/plain", len,
               keep_alive ? "keep-alive" : "close");
  if (net_send_all(c->sock, hdr, (size_t)n))
    return -1;
  if (len && net_send_all(c->sock, body, len))
    return -1;
  return 0;
}

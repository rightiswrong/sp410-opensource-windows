/*
 * http.h - Just enough HTTP/1.1 server for IPP (RFC 8010 section 4).
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_HTTP_H
#define SP410_HTTP_H

#include "../platform/compat.h"

typedef struct
{
  sock_t  sock;
  char    buf[16384];       /* read-ahead buffer */
  size_t  have, off;
} http_conn_t;

typedef struct
{
  char     method[16];
  char     path[512];
  char     host[256];
  char     content_type[128];
  int      http_minor;      /* 0 for HTTP/1.0, 1 for HTTP/1.1 */
  int      keep_alive;
  uint8_t *body;            /* NUL-terminated for convenience */
  size_t   body_len;
} http_req_t;

enum { HTTP_OK = 0, HTTP_CLOSED = 1, HTTP_ERROR = -1, HTTP_TOO_LARGE = -2, HTTP_BAD = -3 };

/*
 * Read one request (headers + body).  Waits up to idle_ms for the request to
 * start.  Returns HTTP_OK, HTTP_CLOSED (peer closed / idle timeout),
 * HTTP_TOO_LARGE (body over max_body; body skipped) or HTTP_BAD/HTTP_ERROR.
 */
int  http_read_request(http_conn_t *c, http_req_t *req, size_t max_body, int idle_ms);
void http_req_free(http_req_t *req);

int  http_respond(http_conn_t *c, int status, const char *reason, const char *ctype,
                  const void *body, size_t len, int keep_alive);

#endif /* SP410_HTTP_H */

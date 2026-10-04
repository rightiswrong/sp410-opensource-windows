/*
 * server.c - IPP Everywhere printer service for the iDPRT SP410.
 *
 * Windows adds this as a printer through its in-box "Microsoft IPP Class
 * Driver".  Windows renders each page to PWG Raster at the resolution and
 * size advertised here (203 dpi, label media), sends it with Print-Job (or
 * Create-Job + Send-Document), and this service converts it to TSPL with the
 * same code the Linux CUPS driver uses, then writes it to the printer.
 *
 * Threads: one per HTTP connection, plus one worker that prints jobs in
 * order.  All job and printer state is guarded by s->lock.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "server.h"
#include "http.h"
#include "ipp.h"
#include "media.h"
#include "../core/log.h"
#include "../core/render.h"
#include "../platform/compat.h"
#include "../platform/device.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SP410_VERSION
#  define SP410_VERSION "0.1.0"
#endif

#define MAX_JOBS          100
#define IDLE_TIMEOUT_MS   60000
#define CREATE_JOB_TTL_MS 300000

enum { JOB_PENDING = 3, JOB_HELD = 4, JOB_PROCESSING = 5, JOB_STOPPED = 6,
       JOB_CANCELED = 7, JOB_ABORTED = 8, JOB_COMPLETED = 9 };
enum { PRINTER_IDLE = 3, PRINTER_PROCESSING = 4, PRINTER_STOPPED = 5 };

typedef struct
{
  int       id;
  int       state;
  char      reasons[64];
  char      message[256];
  char      name[128];
  char      user[64];
  uint8_t  *data;
  size_t    len;
  int       ready;            /* document complete, may be printed */
  int       copies;
  int       media_type;       /* -1 unset, 0 labels, 1 continuous */
  int       darkness_adj;     /* INT_MIN = unset, else -100..100 */
  int       cancel;
  int       impressions;
  int64_t   t_created, t_processing, t_completed;   /* ms since server start */
} job_t;

struct server_s
{
  config_t  cfg;
  sock_t    ls;
  int       port;
  mutex_t   lock;
  cond_t    cond;
  thread_t  worker;
  int       stop;             /* FLAG_GET/FLAG_SET only */
  job_t    *jobs[MAX_JOBS];
  int       njobs;
  int       next_id;
  int       state;
  char      reasons[128];
  char      message[256];
  int64_t   start_ms;
  char      uuid[64];
  int       conns;            /* live connection threads */
};

typedef struct
{
  server_t *s;
  sock_t    sock;
  char      peer[64];
} conn_t;

/* ---- helpers -------------------------------------------------------------- */

static int64_t
uptime_s(const server_t *s)
{
  return (time_ms() - s->start_ms) / 1000 + 1;
}

static void
make_uuid(server_t *s)
{
  uint32_t h[4] = { 2166136261u, 0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u };
  char     seed[256];

  snprintf(seed, sizeof(seed), "sp410|%s|%d", s->cfg.name, s->cfg.port);
  for (const char *p = seed; *p; p ++)
    for (int i = 0; i < 4; i ++)
      h[i] = (h[i] ^ (uint8_t)*p) * (16777619u + 2u * (uint32_t)i);
  snprintf(s->uuid, sizeof(s->uuid), "urn:uuid:%08x-%04x-4%03x-8%03x-%04x%08x",
           h[0], h[1] >> 16, h[1] & 0xfff, h[2] >> 20, h[2] & 0xffff, h[3]);
}

static void
host_for(const server_t *s, const char *host_hdr, char *out, size_t size)
{
  if (host_hdr && *host_hdr)
    snprintf(out, size, "%s", host_hdr);
  else
    snprintf(out, size, "localhost:%d", s->port);
}

static void
set_printer_state(server_t *s, int state, const char *reasons, const char *message)
{
  s->state = state;
  snprintf(s->reasons, sizeof(s->reasons), "%s", reasons ? reasons : "none");
  snprintf(s->message, sizeof(s->message), "%s", message ? message : "");
}

static job_t *
find_job(server_t *s, int id)
{
  for (int i = 0; i < s->njobs; i ++)
    if (s->jobs[i]->id == id)
      return s->jobs[i];
  return NULL;
}

static int
job_active(const job_t *j)
{
  return j->state == JOB_PENDING || j->state == JOB_HELD || j->state == JOB_PROCESSING;
}

static int
active_count(const server_t *s)
{
  int n = 0;
  for (int i = 0; i < s->njobs; i ++)
    n += job_active(s->jobs[i]);
  return n;
}

static void
job_free(job_t *j)
{
  free(j->data);
  free(j);
}

/* Caller holds the lock. Returns NULL if the queue is full of active jobs. */
static job_t *
new_job(server_t *s)
{
  job_t *j;

  if (s->njobs == MAX_JOBS)
  {
    int victim = -1;
    for (int i = 0; i < s->njobs; i ++)
      if (!job_active(s->jobs[i]))
      {
        victim = i;
        break;
      }
    if (victim < 0)
      return NULL;
    job_free(s->jobs[victim]);
    memmove(s->jobs + victim, s->jobs + victim + 1,
            sizeof(job_t *) * (size_t)(s->njobs - victim - 1));
    s->njobs --;
  }
  if ((j = calloc(1, sizeof(*j))) == NULL)
    return NULL;
  j->id           = s->next_id ++;
  j->state        = JOB_PENDING;
  j->copies       = 1;
  j->media_type   = -1;
  j->darkness_adj = INT_MIN;
  j->t_created    = uptime_s(s);
  strcpy(j->reasons, "none");
  s->jobs[s->njobs ++] = j;
  return j;
}

static void
finish_job(server_t *s, job_t *j, int state, const char *reasons, const char *message)
{
  j->state       = state;
  j->t_completed = uptime_s(s);
  snprintf(j->reasons, sizeof(j->reasons), "%s", reasons);
  snprintf(j->message, sizeof(j->message), "%s", message ? message : "");
  free(j->data);
  j->data = NULL;
  j->len  = 0;
}

/* ---- requested-attributes filtering -------------------------------------- */

typedef struct
{
  const ipp_attr_t *req;    /* requested-attributes, NULL = defaults */
  int               all;
} want_t;

static int
want(const want_t *w, const char *name, const char *group)
{
  if (w->all || !w->req)
    return 1;
  if (ipp_has_str(w->req, name))
    return 1;
  return group && ipp_has_str(w->req, group);
}

#define PD "printer-description"
#define JT "job-template"

/* ---- printer attributes --------------------------------------------------- */

static void
write_media_col(ipp_buf_t *b, const char *name, int width, int length,
                const char *pwg, const char *type)
{
  ipp_w_coll_begin(b, name);
  if (pwg)
  {
    ipp_w_member(b, "media-key");
    ipp_w_str(b, IPP_TAG_KEYWORD, NULL, pwg);
  }
  ipp_w_member(b, "media-size");
  ipp_w_coll_begin(b, NULL);
  ipp_w_member(b, "x-dimension");
  if (width < 0)
    ipp_w_range(b, NULL, MEDIA_MIN_WIDTH, MEDIA_MAX_WIDTH);
  else
    ipp_w_int(b, IPP_TAG_INTEGER, NULL, width);
  ipp_w_member(b, "y-dimension");
  if (length < 0)
    ipp_w_range(b, NULL, MEDIA_MIN_LENGTH, MEDIA_MAX_LENGTH);
  else
    ipp_w_int(b, IPP_TAG_INTEGER, NULL, length);
  ipp_w_coll_end(b);
  ipp_w_member(b, "media-bottom-margin"); ipp_w_int(b, IPP_TAG_INTEGER, NULL, 0);
  ipp_w_member(b, "media-left-margin");   ipp_w_int(b, IPP_TAG_INTEGER, NULL, 0);
  ipp_w_member(b, "media-right-margin");  ipp_w_int(b, IPP_TAG_INTEGER, NULL, 0);
  ipp_w_member(b, "media-top-margin");    ipp_w_int(b, IPP_TAG_INTEGER, NULL, 0);
  if (type)
  {
    ipp_w_member(b, "media-type");
    ipp_w_str(b, IPP_TAG_KEYWORD, NULL, type);
  }
  ipp_w_coll_end(b);
}

static void
write_keywords(ipp_buf_t *b, const char *name, const char *spaced)
{
  char  tmp[256], *tok, *save = NULL;
  int   first = 1;

  snprintf(tmp, sizeof(tmp), "%s", spaced && *spaced ? spaced : "none");
  for (tok = strtok_r(tmp, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
  {
    ipp_w_str(b, IPP_TAG_KEYWORD, first ? name : NULL, tok);
    first = 0;
  }
}

static void
write_printer_attrs(server_t *s, ipp_buf_t *b, const want_t *w, const char *host)
{
  static const int ops[] =
  {
    IPP_OP_PRINT_JOB, IPP_OP_VALIDATE_JOB, IPP_OP_CREATE_JOB, IPP_OP_SEND_DOCUMENT,
    IPP_OP_CANCEL_JOB, IPP_OP_GET_JOB_ATTRIBUTES, IPP_OP_GET_JOBS,
    IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_OP_CANCEL_MY_JOBS, IPP_OP_CLOSE_JOB
  };
  static const int orient[]   = { 3, 4, 5, 6 };
  static const int quality[]  = { 3, 4, 5 };
  static const char *const formats[]  = { "image/pwg-raster" };
  static const char *const versions[] = { "1.1", "2.0" };
  static const char *const charsets[] = { "utf-8", "us-ascii" };
  static const char *const pwgtypes[] = { "black_1", "sgray_8" };
  static const char *const colormodes[] = { "monochrome", "auto" };
  static const char *const mtypes[]   = { "labels", "continuous" };
  static const char *const jcattrs[]  =
  {
    "copies", "media", "media-col", "media-type", "orientation-requested",
    "print-color-mode", "print-darkness", "print-quality", "printer-resolution", "sides"
  };
  static const char *const kinds[]    = { "label", "roll" };
  char  uri[400], more[400], devid[160], make[96];
  const media_size_t *def = media_find(s->cfg.default_media);

  if (!def)
    def = &media_sizes[0];

  snprintf(uri, sizeof(uri), "ipp://%s/ipp/print", host);
  snprintf(more, sizeof(more), "http://%s/", host);
  snprintf(make, sizeof(make), "iDPRT %s", s->cfg.model);
  snprintf(devid, sizeof(devid), "MFG:iDPRT;MDL:%s;CMD:TSPL,PWGRaster;CLS:PRINTER;", s->cfg.model);

  ipp_w_group(b, IPP_TAG_PRINTER);

  if (want(w, "charset-configured", PD))     ipp_w_str(b, IPP_TAG_CHARSET, "charset-configured", "utf-8");
  if (want(w, "charset-supported", PD))      ipp_w_strs(b, IPP_TAG_CHARSET, "charset-supported", 2, charsets);
  if (want(w, "color-supported", PD))        ipp_w_bool(b, "color-supported", 0);
  if (want(w, "compression-supported", PD))  ipp_w_str(b, IPP_TAG_KEYWORD, "compression-supported", "none");
  if (want(w, "copies-default", JT))         ipp_w_int(b, IPP_TAG_INTEGER, "copies-default", 1);
  if (want(w, "copies-supported", JT))       ipp_w_range(b, "copies-supported", 1, 999);
  if (want(w, "document-format-default", PD)) ipp_w_str(b, IPP_TAG_MIMETYPE, "document-format-default", formats[0]);
  if (want(w, "document-format-supported", PD)) ipp_w_strs(b, IPP_TAG_MIMETYPE, "document-format-supported", 1, formats);
  if (want(w, "generated-natural-language-supported", PD)) ipp_w_str(b, IPP_TAG_LANGUAGE, "generated-natural-language-supported", "en");
  if (want(w, "ipp-features-supported", PD)) ipp_w_str(b, IPP_TAG_KEYWORD, "ipp-features-supported", "ipp-everywhere");
  if (want(w, "ipp-versions-supported", PD)) ipp_w_strs(b, IPP_TAG_KEYWORD, "ipp-versions-supported", 2, versions);
  if (want(w, "job-creation-attributes-supported", PD))
    ipp_w_strs(b, IPP_TAG_KEYWORD, "job-creation-attributes-supported",
               (int)(sizeof(jcattrs) / sizeof(jcattrs[0])), jcattrs);
  if (want(w, "job-ids-supported", PD))      ipp_w_bool(b, "job-ids-supported", 1);
  if (want(w, "landscape-orientation-requested-preferred", PD)) ipp_w_int(b, IPP_TAG_ENUM, "landscape-orientation-requested-preferred", 4);

  if (want(w, "media-bottom-margin-supported", JT)) ipp_w_int(b, IPP_TAG_INTEGER, "media-bottom-margin-supported", 0);
  if (want(w, "media-left-margin-supported", JT))   ipp_w_int(b, IPP_TAG_INTEGER, "media-left-margin-supported", 0);
  if (want(w, "media-right-margin-supported", JT))  ipp_w_int(b, IPP_TAG_INTEGER, "media-right-margin-supported", 0);
  if (want(w, "media-top-margin-supported", JT))    ipp_w_int(b, IPP_TAG_INTEGER, "media-top-margin-supported", 0);

  if (want(w, "media-col-database", NULL))
  {
    for (int i = 0; i < media_count; i ++)
      write_media_col(b, i ? NULL : "media-col-database", media_sizes[i].width,
                      media_sizes[i].length, media_sizes[i].pwg, NULL);
    write_media_col(b, NULL, -1, -1, NULL, NULL);
  }
  if (want(w, "media-col-default", JT))
    write_media_col(b, "media-col-default", def->width, def->length, def->pwg, "labels");
  if (want(w, "media-col-ready", PD))
    write_media_col(b, "media-col-ready", def->width, def->length, def->pwg, "labels");
  if (want(w, "media-col-supported", JT))
  {
    static const char *const mc[] = { "media-bottom-margin", "media-left-margin",
      "media-right-margin", "media-size", "media-top-margin", "media-type" };
    ipp_w_strs(b, IPP_TAG_KEYWORD, "media-col-supported", 6, mc);
  }
  if (want(w, "media-default", JT))          ipp_w_str(b, IPP_TAG_KEYWORD, "media-default", def->pwg);
  if (want(w, "media-ready", PD))            ipp_w_str(b, IPP_TAG_KEYWORD, "media-ready", def->pwg);
  if (want(w, "media-size-supported", JT))
  {
    for (int i = 0; i < media_count; i ++)
    {
      ipp_w_coll_begin(b, i ? NULL : "media-size-supported");
      ipp_w_member(b, "x-dimension"); ipp_w_int(b, IPP_TAG_INTEGER, NULL, media_sizes[i].width);
      ipp_w_member(b, "y-dimension"); ipp_w_int(b, IPP_TAG_INTEGER, NULL, media_sizes[i].length);
      ipp_w_coll_end(b);
    }
    ipp_w_coll_begin(b, NULL);
    ipp_w_member(b, "x-dimension"); ipp_w_range(b, NULL, MEDIA_MIN_WIDTH, MEDIA_MAX_WIDTH);
    ipp_w_member(b, "y-dimension"); ipp_w_range(b, NULL, MEDIA_MIN_LENGTH, MEDIA_MAX_LENGTH);
    ipp_w_coll_end(b);
  }
  if (want(w, "media-source-supported", JT)) ipp_w_str(b, IPP_TAG_KEYWORD, "media-source-supported", "main-roll");
  if (want(w, "media-supported", JT))
  {
    char custmin[64], custmax[64];
    for (int i = 0; i < media_count; i ++)
      ipp_w_str(b, IPP_TAG_KEYWORD, i ? NULL : "media-supported", media_sizes[i].pwg);
    snprintf(custmin, sizeof(custmin), "custom_min_%dx%dmm", MEDIA_MIN_WIDTH / 100, MEDIA_MIN_LENGTH / 100);
    snprintf(custmax, sizeof(custmax), "custom_max_%dx%dmm", MEDIA_MAX_WIDTH / 100, MEDIA_MAX_LENGTH / 100);
    ipp_w_str(b, IPP_TAG_KEYWORD, NULL, custmin);
    ipp_w_str(b, IPP_TAG_KEYWORD, NULL, custmax);
  }
  if (want(w, "media-type-default", JT))     ipp_w_str(b, IPP_TAG_KEYWORD, "media-type-default", "labels");
  if (want(w, "media-type-supported", JT))   ipp_w_strs(b, IPP_TAG_KEYWORD, "media-type-supported", 2, mtypes);
  if (want(w, "multiple-document-jobs-supported", PD)) ipp_w_bool(b, "multiple-document-jobs-supported", 0);
  if (want(w, "multiple-operation-time-out", PD)) ipp_w_int(b, IPP_TAG_INTEGER, "multiple-operation-time-out", CREATE_JOB_TTL_MS / 1000);
  if (want(w, "natural-language-configured", PD)) ipp_w_str(b, IPP_TAG_LANGUAGE, "natural-language-configured", "en");
  if (want(w, "operations-supported", PD))
    ipp_w_ints(b, IPP_TAG_ENUM, "operations-supported", (int)(sizeof(ops) / sizeof(ops[0])), ops);
  if (want(w, "orientation-requested-default", JT)) ipp_w_int(b, IPP_TAG_ENUM, "orientation-requested-default", 3);
  if (want(w, "orientation-requested-supported", JT)) ipp_w_ints(b, IPP_TAG_ENUM, "orientation-requested-supported", 4, orient);
  if (want(w, "output-bin-default", JT))     ipp_w_str(b, IPP_TAG_KEYWORD, "output-bin-default", "face-up");
  if (want(w, "output-bin-supported", JT))   ipp_w_str(b, IPP_TAG_KEYWORD, "output-bin-supported", "face-up");
  if (want(w, "pdl-override-supported", PD)) ipp_w_str(b, IPP_TAG_KEYWORD, "pdl-override-supported", "attempted");
  if (want(w, "print-color-mode-default", JT)) ipp_w_str(b, IPP_TAG_KEYWORD, "print-color-mode-default", "monochrome");
  if (want(w, "print-color-mode-supported", JT)) ipp_w_strs(b, IPP_TAG_KEYWORD, "print-color-mode-supported", 2, colormodes);
  if (want(w, "print-darkness-default", JT)) ipp_w_int(b, IPP_TAG_INTEGER, "print-darkness-default", 0);
  if (want(w, "print-darkness-supported", JT)) ipp_w_int(b, IPP_TAG_INTEGER, "print-darkness-supported", 16);
  if (want(w, "print-quality-default", JT))  ipp_w_int(b, IPP_TAG_ENUM, "print-quality-default", 4);
  if (want(w, "print-quality-supported", JT)) ipp_w_ints(b, IPP_TAG_ENUM, "print-quality-supported", 3, quality);
  if (want(w, "printer-device-id", PD))      ipp_w_str(b, IPP_TAG_TEXT, "printer-device-id", devid);
  if (want(w, "printer-dns-sd-name", PD))    ipp_w_str(b, IPP_TAG_NAME, "printer-dns-sd-name", s->cfg.name);
  if (want(w, "printer-firmware-string-version", PD)) ipp_w_str(b, IPP_TAG_TEXT, "printer-firmware-string-version", SP410_VERSION);
  if (want(w, "printer-info", PD))           ipp_w_str(b, IPP_TAG_TEXT, "printer-info", s->cfg.name);
  if (want(w, "printer-is-accepting-jobs", PD)) ipp_w_bool(b, "printer-is-accepting-jobs", 1);
  if (want(w, "printer-kind", PD))           ipp_w_strs(b, IPP_TAG_KEYWORD, "printer-kind", 2, kinds);
  if (want(w, "printer-location", PD))       ipp_w_str(b, IPP_TAG_TEXT, "printer-location", s->cfg.location);
  if (want(w, "printer-make-and-model", PD)) ipp_w_str(b, IPP_TAG_TEXT, "printer-make-and-model", make);
  if (want(w, "printer-more-info", PD))      ipp_w_str(b, IPP_TAG_URI, "printer-more-info", more);
  if (want(w, "printer-name", PD))           ipp_w_str(b, IPP_TAG_NAME, "printer-name", s->cfg.name);
  if (want(w, "printer-resolution-default", JT)) ipp_w_res(b, "printer-resolution-default", 203, 203);
  if (want(w, "printer-resolution-supported", JT)) ipp_w_res(b, "printer-resolution-supported", 203, 203);
  if (want(w, "printer-state", PD))          ipp_w_int(b, IPP_TAG_ENUM, "printer-state", s->state);
  if (want(w, "printer-state-message", PD))  ipp_w_str(b, IPP_TAG_TEXT, "printer-state-message", s->message);
  if (want(w, "printer-state-reasons", PD))  write_keywords(b, "printer-state-reasons", s->reasons);
  if (want(w, "printer-up-time", PD))        ipp_w_int(b, IPP_TAG_INTEGER, "printer-up-time", (int)uptime_s(s));
  if (want(w, "printer-uri-supported", PD))  ipp_w_str(b, IPP_TAG_URI, "printer-uri-supported", uri);
  if (want(w, "printer-uuid", PD))           ipp_w_str(b, IPP_TAG_URI, "printer-uuid", s->uuid);
  if (want(w, "pwg-raster-document-resolution-supported", PD)) ipp_w_res(b, "pwg-raster-document-resolution-supported", 203, 203);
  if (want(w, "pwg-raster-document-sheet-back", PD)) ipp_w_str(b, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", "normal");
  if (want(w, "pwg-raster-document-type-supported", PD)) ipp_w_strs(b, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported", 2, pwgtypes);
  if (want(w, "queued-job-count", PD))       ipp_w_int(b, IPP_TAG_INTEGER, "queued-job-count", active_count(s));
  if (want(w, "sides-default", JT))          ipp_w_str(b, IPP_TAG_KEYWORD, "sides-default", "one-sided");
  if (want(w, "sides-supported", JT))        ipp_w_str(b, IPP_TAG_KEYWORD, "sides-supported", "one-sided");
  if (want(w, "uri-authentication-supported", PD)) ipp_w_str(b, IPP_TAG_KEYWORD, "uri-authentication-supported", "none");
  if (want(w, "uri-security-supported", PD)) ipp_w_str(b, IPP_TAG_KEYWORD, "uri-security-supported", "none");
}

/* ---- job attributes ------------------------------------------------------- */

static const char *
job_state_reason_default(const job_t *j)
{
  switch (j->state)
  {
    case JOB_PENDING:    return j->ready ? "none" : "job-incoming";
    case JOB_PROCESSING: return "job-printing";
    case JOB_CANCELED:   return "job-canceled-by-user";
    case JOB_ABORTED:    return "aborted-by-system";
    case JOB_COMPLETED:  return "job-completed-successfully";
    default:             return "none";
  }
}

static void
write_job_attrs(server_t *s, ipp_buf_t *b, const job_t *j, const want_t *w, const char *host)
{
  char uri[440], puri[400];

  snprintf(puri, sizeof(puri), "ipp://%s/ipp/print", host);
  snprintf(uri, sizeof(uri), "%s/%d", puri, j->id);

  if (want(w, "job-id", "job-description"))      ipp_w_int(b, IPP_TAG_INTEGER, "job-id", j->id);
  if (want(w, "job-uri", "job-description"))     ipp_w_str(b, IPP_TAG_URI, "job-uri", uri);
  if (want(w, "job-printer-uri", "job-description")) ipp_w_str(b, IPP_TAG_URI, "job-printer-uri", puri);
  if (want(w, "job-state", "job-description"))   ipp_w_int(b, IPP_TAG_ENUM, "job-state", j->state);
  if (want(w, "job-state-reasons", "job-description"))
    write_keywords(b, "job-state-reasons",
                   strcmp(j->reasons, "none") ? j->reasons : job_state_reason_default(j));
  if (want(w, "job-state-message", "job-description")) ipp_w_str(b, IPP_TAG_TEXT, "job-state-message", j->message);
  if (want(w, "job-name", "job-description"))    ipp_w_str(b, IPP_TAG_NAME, "job-name", j->name[0] ? j->name : "Untitled");
  if (want(w, "job-originating-user-name", "job-description"))
    ipp_w_str(b, IPP_TAG_NAME, "job-originating-user-name", j->user[0] ? j->user : "anonymous");
  if (want(w, "job-impressions-completed", "job-description")) ipp_w_int(b, IPP_TAG_INTEGER, "job-impressions-completed", j->impressions);
  if (want(w, "job-k-octets", "job-description")) ipp_w_int(b, IPP_TAG_INTEGER, "job-k-octets", (int)((j->len + 1023) / 1024));
  if (want(w, "job-printer-up-time", "job-description")) ipp_w_int(b, IPP_TAG_INTEGER, "job-printer-up-time", (int)uptime_s(s));
  if (want(w, "time-at-creation", "job-description")) ipp_w_int(b, IPP_TAG_INTEGER, "time-at-creation", (int)j->t_created);
  if (want(w, "time-at-processing", "job-description"))
  {
    if (j->t_processing) ipp_w_int(b, IPP_TAG_INTEGER, "time-at-processing", (int)j->t_processing);
    else ipp_w_novalue(b, IPP_TAG_NOVALUE, "time-at-processing");
  }
  if (want(w, "time-at-completed", "job-description"))
  {
    if (j->t_completed) ipp_w_int(b, IPP_TAG_INTEGER, "time-at-completed", (int)j->t_completed);
    else ipp_w_novalue(b, IPP_TAG_NOVALUE, "time-at-completed");
  }
  if (want(w, "copies", JT)) ipp_w_int(b, IPP_TAG_INTEGER, "copies", j->copies);
}

/* ---- request handling ----------------------------------------------------- */

static void
begin_response(ipp_buf_t *b, const ipp_t *req, int status, const char *message)
{
  ipp_w_header(b, req->major == 2 ? 2 : 1, req->major == 2 ? req->minor : 1, status,
               req->request_id);
  ipp_w_group(b, IPP_TAG_OPERATION);
  ipp_w_str(b, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
  ipp_w_str(b, IPP_TAG_LANGUAGE, "attributes-natural-language", "en");
  if (message && *message)
    ipp_w_str(b, IPP_TAG_TEXT, "status-message", message);
}

static int
job_id_from_request(const ipp_t *req)
{
  ipp_attr_t *a = ipp_find(req, IPP_TAG_OPERATION, "job-id");
  const char *uri, *slash;

  if (a)
    return ipp_int(a, 0, -1);
  if ((uri = ipp_str(ipp_find(req, IPP_TAG_OPERATION, "job-uri"), 0)) != NULL &&
      (slash = strrchr(uri, '/')) != NULL)
    return atoi(slash + 1);
  return -1;
}

/* Check document-format; returns 0 if acceptable. */
static int
check_format(const ipp_t *req, const uint8_t *doc, size_t len, char *msg, size_t msglen)
{
  const char *fmt = ipp_str(ipp_find(req, IPP_TAG_OPERATION, "document-format"), 0);

  if (!fmt || !strcmp(fmt, "image/pwg-raster"))
    return 0;
  if (!strcmp(fmt, "application/octet-stream") &&
      (!doc || (len >= 4 && !memcmp(doc, "RaS2", 4))))
    return 0;
  snprintf(msg, msglen, "document-format \"%s\" is not supported; send image/pwg-raster", fmt);
  return -1;
}

/* Copy job-template attributes that affect output into the job. */
static void
apply_job_template(job_t *j, const ipp_t *req)
{
  ipp_attr_t *a;
  const char *mt = NULL;

  if ((a = ipp_find(req, IPP_TAG_JOB, "copies")) != NULL)
  {
    int c = ipp_int(a, 0, 1);
    j->copies = c < 1 ? 1 : c > 999 ? 999 : c;
  }
  if ((a = ipp_find(req, IPP_TAG_JOB, "media-type")) != NULL)
    mt = ipp_str(a, 0);
  else if ((a = ipp_find(req, IPP_TAG_JOB, "media-col")) != NULL && a->num_values)
    mt = ipp_str(ipp_member(&a->values[0], "media-type"), 0);
  if (mt)
    j->media_type = !strcmp(mt, "continuous") || !strncmp(mt, "continuous-", 11) ? 1 : 0;
  if ((a = ipp_find(req, IPP_TAG_JOB, "print-darkness")) != NULL)
  {
    int d = ipp_int(a, 0, 0);
    j->darkness_adj = d < -100 ? -100 : d > 100 ? 100 : d;
  }
}

static void
read_job_description(job_t *j, const ipp_t *req)
{
  const char *v;

  if ((v = ipp_str(ipp_find(req, IPP_TAG_OPERATION, "job-name"), 0)) != NULL)
    snprintf(j->name, sizeof(j->name), "%s", v);
  if ((v = ipp_str(ipp_find(req, IPP_TAG_OPERATION, "requesting-user-name"), 0)) != NULL)
    snprintf(j->user, sizeof(j->user), "%s", v);
}

/* Append a document; drops the sync word of second and later PWG streams. */
static int
append_document(job_t *j, const uint8_t *doc, size_t len)
{
  uint8_t *nd;

  if (j->len && len >= 4 && !memcmp(doc, "RaS2", 4))
  {
    doc += 4;
    len -= 4;
  }
  if (!len)
    return 0;
  if ((nd = realloc(j->data, j->len + len)) == NULL)
    return -1;
  memcpy(nd + j->len, doc, len);
  j->data = nd;
  j->len += len;
  return 0;
}

static void
handle_ipp(server_t *s, const http_req_t *http, ipp_buf_t *out)
{
  ipp_t       req;
  size_t      off = 0;
  char        err[160], host[300], msg[200] = "";
  const uint8_t *doc;
  size_t      doclen;
  want_t      w = { NULL, 0 };

  host_for(s, http->host, host, sizeof(host));

  if (ipp_parse(http->body, http->body_len, &req, &off, err, sizeof(err)))
  {
    ipp_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.major = 1;
    dummy.request_id = http->body_len >= 8 ?
      ((uint32_t)http->body[4] << 24 | (uint32_t)http->body[5] << 16 |
       (uint32_t)http->body[6] << 8 | http->body[7]) : 0;
    log_msg(LOG_WARN, "bad IPP request: %s", err);
    begin_response(out, &dummy, IPP_BAD_REQUEST, err);
    ipp_w_end(out);
    return;
  }
  doc    = http->body + off;
  doclen = http->body_len - off;

  if (req.major < 1 || req.major > 2)
  {
    ipp_t v = req;
    v.major = 1;
    begin_response(out, &v, IPP_VERSION_NOT_SUPPORTED, "IPP version not supported");
    ipp_w_end(out);
    ipp_free(&req);
    return;
  }

  w.req = ipp_find(&req, IPP_TAG_OPERATION, "requested-attributes");
  w.all = w.req && ipp_has_str(w.req, "all");

  log_msg(LOG_DEBUG, "IPP op 0x%04x, request %u, %zu document bytes",
          req.op, req.request_id, doclen);

  mutex_lock(&s->lock);

  switch (req.op)
  {
    case IPP_OP_GET_PRINTER_ATTRIBUTES:
      begin_response(out, &req, IPP_OK, NULL);
      write_printer_attrs(s, out, &w, host);
      break;

    case IPP_OP_VALIDATE_JOB:
      if (check_format(&req, NULL, 0, msg, sizeof(msg)))
        begin_response(out, &req, IPP_DOCUMENT_FORMAT_NOT_SUPPORTED, msg);
      else
        begin_response(out, &req, IPP_OK, NULL);
      break;

    case IPP_OP_PRINT_JOB:
    case IPP_OP_CREATE_JOB:
    {
      job_t *j;
      want_t jw = { NULL, 0 };
      static const char *const ret[] = { "job-id", "job-uri", "job-state", "job-state-reasons" };

      if (req.op == IPP_OP_PRINT_JOB && check_format(&req, doc, doclen, msg, sizeof(msg)))
      {
        begin_response(out, &req, IPP_DOCUMENT_FORMAT_NOT_SUPPORTED, msg);
        break;
      }
      if (req.op == IPP_OP_PRINT_JOB && (doclen < 4 || memcmp(doc, "RaS2", 4)))
      {
        begin_response(out, &req, doclen ? IPP_DOCUMENT_FORMAT_NOT_SUPPORTED : IPP_BAD_REQUEST,
                       doclen ? "document is not PWG raster" : "no document data");
        break;
      }
      if ((j = new_job(s)) == NULL)
      {
        begin_response(out, &req, IPP_BUSY, "too many active jobs");
        break;
      }
      read_job_description(j, &req);
      apply_job_template(j, &req);
      if (req.op == IPP_OP_PRINT_JOB)
      {
        if (append_document(j, doc, doclen))
        {
          finish_job(s, j, JOB_ABORTED, "aborted-by-system", "out of memory");
          begin_response(out, &req, IPP_INTERNAL_ERROR, "out of memory");
          break;
        }
        j->ready = 1;
        cond_broadcast(&s->cond);
      }
      log_msg(LOG_INFO, "job %d %s by %s: \"%s\" (%zu bytes)", j->id,
              req.op == IPP_OP_PRINT_JOB ? "received" : "created",
              j->user[0] ? j->user : "anonymous", j->name, j->len);

      begin_response(out, &req, IPP_OK, NULL);
      ipp_w_group(out, IPP_TAG_JOB);
      {
        /* Return the minimal job description set. */
        ipp_attr_t  r  = { 0, (char *)"requested-attributes", 4, NULL, NULL };
        ipp_value_t vals[4];
        for (int i = 0; i < 4; i ++)
        {
          vals[i].tag = IPP_TAG_KEYWORD;
          vals[i].len = strlen(ret[i]);
          vals[i].data = (uint8_t *)ret[i];
          vals[i].members = NULL;
        }
        r.values = vals;
        jw.req   = &r;
        write_job_attrs(s, out, j, &jw, host);
      }
      break;
    }

    case IPP_OP_SEND_DOCUMENT:
    {
      int    id = job_id_from_request(&req);
      job_t *j  = find_job(s, id);
      int    last = ipp_int(ipp_find(&req, IPP_TAG_OPERATION, "last-document"), 0, 1);

      if (!j)
      {
        begin_response(out, &req, IPP_NOT_FOUND, "job not found");
        break;
      }
      if (j->ready || !job_active(j))
      {
        begin_response(out, &req, IPP_NOT_POSSIBLE, "job is not waiting for a document");
        break;
      }
      if (check_format(&req, doc, doclen, msg, sizeof(msg)) ||
          (doclen && (doclen < 4 || memcmp(doc, "RaS2", 4))))
      {
        begin_response(out, &req, IPP_DOCUMENT_FORMAT_NOT_SUPPORTED,
                       msg[0] ? msg : "document is not PWG raster");
        break;
      }
      apply_job_template(j, &req);
      if (append_document(j, doc, doclen))
      {
        finish_job(s, j, JOB_ABORTED, "aborted-by-system", "out of memory");
        begin_response(out, &req, IPP_INTERNAL_ERROR, "out of memory");
        break;
      }
      if (last)
      {
        if (j->len)
          j->ready = 1;
        else
          finish_job(s, j, JOB_ABORTED, "aborted-by-system", "job has no document");
        cond_broadcast(&s->cond);
      }
      begin_response(out, &req, IPP_OK, NULL);
      ipp_w_group(out, IPP_TAG_JOB);
      ipp_w_int(out, IPP_TAG_INTEGER, "job-id", j->id);
      ipp_w_int(out, IPP_TAG_ENUM, "job-state", j->state);
      write_keywords(out, "job-state-reasons", job_state_reason_default(j));
      break;
    }

    case IPP_OP_CLOSE_JOB:
    {
      job_t *j = find_job(s, job_id_from_request(&req));

      if (!j)
        begin_response(out, &req, IPP_NOT_FOUND, "job not found");
      else
      {
        if (job_active(j) && !j->ready)
        {
          if (j->len)
            j->ready = 1;
          else
            finish_job(s, j, JOB_ABORTED, "aborted-by-system", "job has no document");
          cond_broadcast(&s->cond);
        }
        begin_response(out, &req, IPP_OK, NULL);
      }
      break;
    }

    case IPP_OP_CANCEL_JOB:
    {
      job_t *j = find_job(s, job_id_from_request(&req));

      if (!j)
        begin_response(out, &req, IPP_NOT_FOUND, "job not found");
      else if (!job_active(j))
        begin_response(out, &req, IPP_NOT_POSSIBLE, "job is already finished");
      else
      {
        FLAG_SET(&j->cancel, 1);
        if (j->state != JOB_PROCESSING)
          finish_job(s, j, JOB_CANCELED, "job-canceled-by-user", "canceled");
        log_msg(LOG_INFO, "job %d canceled", j->id);
        cond_broadcast(&s->cond);
        begin_response(out, &req, IPP_OK, NULL);
      }
      break;
    }

    case IPP_OP_CANCEL_MY_JOBS:
      for (int i = 0; i < s->njobs; i ++)
      {
        job_t *j = s->jobs[i];
        if (!job_active(j))
          continue;
        FLAG_SET(&j->cancel, 1);
        if (j->state != JOB_PROCESSING)
          finish_job(s, j, JOB_CANCELED, "job-canceled-by-user", "canceled");
      }
      cond_broadcast(&s->cond);
      begin_response(out, &req, IPP_OK, NULL);
      break;

    case IPP_OP_GET_JOB_ATTRIBUTES:
    {
      job_t *j = find_job(s, job_id_from_request(&req));

      if (!j)
        begin_response(out, &req, IPP_NOT_FOUND, "job not found");
      else
      {
        begin_response(out, &req, IPP_OK, NULL);
        ipp_w_group(out, IPP_TAG_JOB);
        write_job_attrs(s, out, j, &w, host);
      }
      break;
    }

    case IPP_OP_GET_JOBS:
    {
      const char *which = ipp_str(ipp_find(&req, IPP_TAG_OPERATION, "which-jobs"), 0);
      int         limit = ipp_int(ipp_find(&req, IPP_TAG_OPERATION, "limit"), 0, INT_MAX);
      int         want_done = which && !strcmp(which, "completed");
      int         want_all  = which && !strcmp(which, "all");
      int         count = 0;
      want_t      gw = w;
      ipp_attr_t  r  = { 0, (char *)"requested-attributes", 2, NULL, NULL };
      ipp_value_t vals[2] = { { IPP_TAG_KEYWORD, 6, (uint8_t *)"job-id", NULL },
                              { IPP_TAG_KEYWORD, 7, (uint8_t *)"job-uri", NULL } };

      if (which && !want_done && !want_all && strcmp(which, "not-completed"))
      {
        begin_response(out, &req, IPP_ATTRIBUTES_OR_VALUES, "unsupported which-jobs value");
        break;
      }
      if (!gw.req)
      {
        r.values = vals;
        gw.req   = &r;
      }
      begin_response(out, &req, IPP_OK, NULL);
      for (int i = s->njobs - 1; i >= 0 && count < limit; i --)
      {
        job_t *j = s->jobs[i];
        int    active = job_active(j);

        if (!want_all && (want_done ? active : !active))
          continue;
        ipp_w_group(out, IPP_TAG_JOB);
        write_job_attrs(s, out, j, &gw, host);
        count ++;
      }
      break;
    }

    default:
      begin_response(out, &req, IPP_OPERATION_NOT_SUPPORTED, "operation not supported");
      break;
  }

  mutex_unlock(&s->lock);
  ipp_w_end(out);
  ipp_free(&req);
}

/* ---- status page ---------------------------------------------------------- */

static void
html_escape(char *out, size_t size, const char *in)
{
  size_t o = 0;

  for (; *in && o + 7 < size; in ++)
  {
    const char *rep = NULL;
    switch (*in)
    {
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '&': rep = "&amp;"; break;
      case '"': rep = "&quot;"; break;
    }
    if (rep)
    {
      size_t n = strlen(rep);
      memcpy(out + o, rep, n);
      o += n;
    }
    else
      out[o ++] = *in;
  }
  out[o] = '\0';
}

static char *
status_page(server_t *s, const char *host, size_t *len)
{
  static const char *const jstates[] = { "", "", "", "pending", "held", "printing",
                                         "stopped", "canceled", "aborted", "completed" };
  size_t cap = 16384, n = 0;
  char  *p = malloc(cap), name[300], dev[1100], msg[600];

  if (!p)
    return NULL;
#define EMIT(...) do { if (n < cap) n += (size_t)snprintf(p + n, cap - n, __VA_ARGS__); } while (0)
  mutex_lock(&s->lock);
  html_escape(name, sizeof(name), s->cfg.name);
  html_escape(dev, sizeof(dev), s->cfg.device);
  html_escape(msg, sizeof(msg), s->message);
  EMIT("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\"><meta http-equiv=\"refresh\" content=\"5\">"
      "<title>%s</title><style>body{font:15px/1.5 system-ui,sans-serif;margin:24px;"
      "color:#1b2328;background:#f4f6f7}main{max-width:760px}h1{font-size:22px;margin:0 0 4px}"
      "table{border-collapse:collapse;width:100%%;background:#fff}td,th{text-align:left;"
      "padding:6px 10px;border-bottom:1px solid #dde3e6}code{font:13px ui-monospace,monospace}"
      ".st{display:inline-block;padding:2px 8px;border-radius:10px;background:#dcefe4}"
      "</style></head><body><main><h1>%s</h1>", name, name);
  EMIT("<p><span class=\"st\">%s</span> %s</p>",
      s->state == PRINTER_PROCESSING ? "printing" : s->state == PRINTER_STOPPED ? "stopped" : "ready",
      msg);
  EMIT("<table><tr><th>Printer URL</th><td><code>http://%s/ipp/print</code></td></tr>"
      "<tr><th>Device</th><td><code>%s</code></td></tr>"
      "<tr><th>Settings file</th><td><code>%s</code></td></tr>"
      "<tr><th>Version</th><td>sp410-ippd " SP410_VERSION "</td></tr></table>",
      host, dev, s->cfg.path[0] ? s->cfg.path : "(built-in defaults)");
  EMIT("<h2 style=\"font-size:17px;margin-top:24px\">Recent jobs</h2><table>"
      "<tr><th>Job</th><th>Name</th><th>State</th><th>Labels</th><th>Message</th></tr>");
  for (int i = s->njobs - 1, k = 0; i >= 0 && k < 20; i --, k ++)
  {
    job_t *j = s->jobs[i];
    char   jn[300], jm[600];
    html_escape(jn, sizeof(jn), j->name[0] ? j->name : "Untitled");
    html_escape(jm, sizeof(jm), j->message);
    EMIT("<tr><td>%d</td><td>%s</td><td>%s</td><td>%d</td><td>%s</td></tr>",
        j->id, jn, jstates[j->state], j->impressions, jm);
  }
  if (!s->njobs)
    EMIT("<tr><td colspan=\"5\">No jobs yet.</td></tr>");
  EMIT("</table></main></body></html>");
  mutex_unlock(&s->lock);
#undef EMIT
  if (n >= cap)
    n = cap - 1;
  *len = n;
  return p;
}

/* ---- connections ------------------------------------------------------------ */

static void
conn_thread(void *arg)
{
  conn_t     *c = arg;
  server_t   *s = c->s;
  http_conn_t hc;
  size_t      max_body = (size_t)s->cfg.max_job_mb * 1024u * 1024u + 65536u;

  memset(&hc, 0, sizeof(hc));
  hc.sock = c->sock;

  while (!FLAG_GET(&s->stop))
  {
    http_req_t req;
    int        rc;
    int64_t    idle_start = time_ms();

    /* Wait for the next request in short slices so shutdown is prompt. */
    while (!FLAG_GET(&s->stop) && hc.off == hc.have)
    {
      int r = net_wait(c->sock, 500);
      if (r > 0)
        break;
      if (r < 0 || time_ms() - idle_start > IDLE_TIMEOUT_MS)
        goto done;
    }
    if (FLAG_GET(&s->stop))
      break;

    rc = http_read_request(&hc, &req, max_body, 5000);

    if (rc == HTTP_CLOSED || rc == HTTP_ERROR)
    {
      http_req_free(&req);
      break;
    }
    if (rc == HTTP_TOO_LARGE)
    {
      static const char m[] = "Job is larger than max-job-mb in sp410.ini.\n";
      log_msg(LOG_WARN, "%s: request body over %d MB rejected", c->peer, s->cfg.max_job_mb);
      http_respond(&hc, 413, "Request Entity Too Large", "text/plain", m, sizeof(m) - 1, 0);
      http_req_free(&req);
      net_linger_close(c->sock, 3000);
      c->sock = SOCK_INVALID;
      break;
    }
    if (rc == HTTP_BAD)
    {
      static const char m[] = "Bad request\n";
      http_respond(&hc, 400, "Bad Request", "text/plain", m, sizeof(m) - 1, 0);
      http_req_free(&req);
      net_linger_close(c->sock, 1000);
      c->sock = SOCK_INVALID;
      break;
    }

    if (!strcmp(req.method, "POST"))
    {
      if (strncasecmp(req.content_type, "application/ipp", 15))
      {
        static const char m[] = "Expected Content-Type: application/ipp\n";
        http_respond(&hc, 415, "Unsupported Media Type", "text/plain", m, sizeof(m) - 1, req.keep_alive);
      }
      else
      {
        ipp_buf_t out;
        memset(&out, 0, sizeof(out));
        handle_ipp(s, &req, &out);
        if (out.err)
        {
          static const char m[] = "Out of memory\n";
          http_respond(&hc, 500, "Internal Server Error", "text/plain", m, sizeof(m) - 1, 0);
          req.keep_alive = 0;
        }
        else
          http_respond(&hc, 200, "OK", "application/ipp", out.data, out.len, req.keep_alive);
        ipp_buf_free(&out);
      }
    }
    else if (!strcmp(req.method, "GET") || !strcmp(req.method, "HEAD"))
    {
      if (!strcmp(req.path, "/") || !strcmp(req.path, "/ipp/print"))
      {
        char   host[300];
        size_t len = 0;
        char  *page;

        host_for(s, req.host, host, sizeof(host));
        page = status_page(s, host, &len);
        http_respond(&hc, 200, "OK", "text/html; charset=utf-8", page,
                     strcmp(req.method, "HEAD") ? len : 0, req.keep_alive);
        free(page);
      }
      else
      {
        static const char m[] = "Not found\n";
        http_respond(&hc, 404, "Not Found", "text/plain", m, sizeof(m) - 1, req.keep_alive);
      }
    }
    else
    {
      static const char m[] = "Method not allowed\n";
      http_respond(&hc, 405, "Method Not Allowed", "text/plain", m, sizeof(m) - 1, req.keep_alive);
    }

    if (!req.keep_alive)
    {
      http_req_free(&req);
      break;
    }
    http_req_free(&req);
  }

done:
  net_close(c->sock);
  mutex_lock(&s->lock);
  s->conns --;
  cond_broadcast(&s->cond);
  mutex_unlock(&s->lock);
  free(c);
}

/* ---- printing worker ---------------------------------------------------------- */

typedef struct
{
  server_t *s;
  job_t    *job;
  device_t *dev;
  int       stall_ms;
  char      err[200];
} sinkctx_t;

static int
job_cancelled(void *ctx)
{
  sinkctx_t *k = ctx;
  return FLAG_GET(&k->job->cancel) || FLAG_GET(&k->s->stop);
}

static int
dev_sink_write(void *ctx, const void *data, size_t len)
{
  sinkctx_t *k = ctx;
  int rc = device_write(k->dev, data, len, k->stall_ms, job_cancelled, k, k->err, sizeof(k->err));
  return rc ? -1 : 0;
}

static void
print_job(server_t *s, job_t *j)
{
  config_t         cfg;
  sp410_settings_t st;
  sinkctx_t        k;
  render_stats_t   rs;
  tspl_sink_t      sink;
  int64_t          start = time_ms();
  int              rc, attempt = 0;
  char             err[200] = "";

  memset(&k, 0, sizeof(k));

  /* Settings are re-read for every job so edits to sp410.ini apply at once. */
  cfg = s->cfg;
  if (cfg.path[0])
  {
    config_t fresh;
    config_defaults(&fresh);
    if (config_load(&fresh, cfg.path) == 0)
    {
      /* [server] values stay as started; device/media/print are live. */
      if (!cfg.device_locked)
        memcpy(cfg.device, fresh.device, sizeof(cfg.device));
      cfg.stall_timeout_s = fresh.stall_timeout_s;
      cfg.retry_timeout_s = fresh.retry_timeout_s;
      cfg.num_print = fresh.num_print;
      memcpy(cfg.print_key, fresh.print_key, sizeof(cfg.print_key));
      memcpy(cfg.print_val, fresh.print_val, sizeof(cfg.print_val));
    }
  }
  config_print_settings(&cfg, &st);
  if (j->media_type == 1)
    st.media = MEDIA_CONTINUOUS;
  if (j->darkness_adj != INT_MIN)
  {
    int base = st.darkness < 0 ? 8 : st.darkness;
    int d    = base + (j->darkness_adj * 8 + (j->darkness_adj >= 0 ? 50 : -50)) / 100;
    st.darkness = d < 0 ? 0 : d > 15 ? 15 : d;
  }

  /* Open the printer, waiting if it is switched off or unplugged. */
  for (;;)
  {
    k.dev = device_open(cfg.device, j->id, err, sizeof(err));
    if (k.dev || FLAG_GET(&j->cancel) || FLAG_GET(&s->stop))
      break;
    if (time_ms() - start > (int64_t)cfg.retry_timeout_s * 1000)
      break;
    if (!attempt ++)
      log_msg(LOG_WARN, "job %d: waiting for printer: %s", j->id, err);
    mutex_lock(&s->lock);
    set_printer_state(s, PRINTER_PROCESSING, "offline-report", err);
    snprintf(j->reasons, sizeof(j->reasons), "printer-stopped");
    snprintf(j->message, sizeof(j->message), "Waiting for printer: %s", err);
    cond_timedwait(&s->cond, &s->lock, 2000);
    mutex_unlock(&s->lock);
  }

  mutex_lock(&s->lock);
  set_printer_state(s, PRINTER_PROCESSING, "none", "Printing");
  strcpy(j->reasons, "none");
  j->message[0] = '\0';
  mutex_unlock(&s->lock);

  if (!k.dev)
  {
    mutex_lock(&s->lock);
    if (FLAG_GET(&j->cancel) || FLAG_GET(&s->stop))
      finish_job(s, j, JOB_CANCELED, "job-canceled-by-user", "canceled");
    else
    {
      log_msg(LOG_ERROR, "job %d: printer unavailable: %s", j->id, err);
      finish_job(s, j, JOB_ABORTED, "aborted-by-system", err);
    }
    mutex_unlock(&s->lock);
    return;
  }

  k.s        = s;
  k.job      = j;
  k.stall_ms = cfg.stall_timeout_s * 1000;
  sink.write = dev_sink_write;
  sink.ctx   = &k;

  log_msg(LOG_INFO, "job %d: printing via %s (darkness %d, speed %d, media %s, dither %s)",
          j->id, device_kind(k.dev), st.darkness, st.speed, media_name(st.media),
          dither_name(st.dither));

  rc = render_document(j->data, j->len, &st, j->copies, &sink, job_cancelled, &k, &rs);

  if (rc == 0 && device_close(k.dev))
  {
    rc = -1;
    snprintf(rs.error, sizeof(rs.error), "unable to finish writing to the printer");
  }
  else if (rc != 0)
    device_discard(k.dev);

  mutex_lock(&s->lock);
  j->impressions = rs.labels;
  if (rc == 0)
  {
    char m[96];
    snprintf(m, sizeof(m), "%d label%s printed", rs.labels, rs.labels == 1 ? "" : "s");
    finish_job(s, j, JOB_COMPLETED, "job-completed-successfully", m);
    log_msg(LOG_INFO, "job %d: completed, %d page(s), %d label(s), %zu bytes",
            j->id, rs.pages, rs.labels, rs.bytes);
  }
  else if (rc == 1 || FLAG_GET(&j->cancel))
  {
    finish_job(s, j, JOB_CANCELED, "job-canceled-by-user", "canceled");
    log_msg(LOG_INFO, "job %d: canceled", j->id);
  }
  else
  {
    const char *why = k.err[0] ? k.err : rs.error;
    finish_job(s, j, JOB_ABORTED, "aborted-by-system", why);
    log_msg(LOG_ERROR, "job %d: failed: %s", j->id, why);
  }
  mutex_unlock(&s->lock);
}

static void
worker_thread(void *arg)
{
  server_t *s = arg;

  mutex_lock(&s->lock);
  while (!FLAG_GET(&s->stop))
  {
    job_t  *next = NULL;
    int64_t now  = uptime_s(s);

    for (int i = 0; i < s->njobs; i ++)
    {
      job_t *j = s->jobs[i];

      if (j->state != JOB_PENDING)
        continue;
      if (!j->ready)
      {
        /* Create-Job without a document for too long. */
        if ((now - j->t_created) * 1000 > CREATE_JOB_TTL_MS)
          finish_job(s, j, JOB_ABORTED, "aborted-by-system", "no document received");
        continue;
      }
      next = j;
      break;
    }

    if (!next)
    {
      if (s->state != PRINTER_IDLE || strcmp(s->reasons, "none"))
        set_printer_state(s, PRINTER_IDLE, "none", "");
      cond_timedwait(&s->cond, &s->lock, 1000);
      continue;
    }

    next->state        = JOB_PROCESSING;
    next->t_processing = uptime_s(s);
    set_printer_state(s, PRINTER_PROCESSING, "none", "Printing");
    mutex_unlock(&s->lock);

    print_job(s, next);

    mutex_lock(&s->lock);
  }
  mutex_unlock(&s->lock);
}

/* ---- lifecycle ------------------------------------------------------------- */

server_t *
server_create(const config_t *cfg)
{
  server_t *s = calloc(1, sizeof(*s));

  if (!s)
    return NULL;
  s->cfg      = *cfg;
  s->next_id  = 1;
  s->start_ms = time_ms();
  s->state    = PRINTER_IDLE;
  strcpy(s->reasons, "none");
  mutex_init(&s->lock);
  cond_init(&s->cond);
  make_uuid(s);

  s->ls = net_listen(cfg->listen, cfg->port, &s->port);
  if (s->ls == SOCK_INVALID)
  {
    log_msg(LOG_ERROR, "cannot listen on %s:%d (%s) - is another copy running?",
            cfg->listen, cfg->port, net_strerror());
    mutex_destroy(&s->lock);
    cond_destroy(&s->cond);
    free(s);
    return NULL;
  }
  return s;
}

int
server_port(const server_t *s)
{
  return s->port;
}

int
server_run(server_t *s)
{
  if (thread_start(&s->worker, worker_thread, s))
  {
    log_msg(LOG_ERROR, "cannot start worker thread");
    return -1;
  }
  log_msg(LOG_INFO, "sp410-ippd " SP410_VERSION " ready: http://%s:%d/ipp/print (device %s)",
          strcmp(s->cfg.listen, "0.0.0.0") ? s->cfg.listen : "localhost", s->port, s->cfg.device);

  while (!FLAG_GET(&s->stop))
  {
    char   peer[64];
    sock_t cs = net_accept(s->ls, 500, peer, sizeof(peer));
    conn_t *c;

    if (cs == SOCK_INVALID)
      continue;
    if ((c = calloc(1, sizeof(*c))) == NULL)
    {
      net_close(cs);
      continue;
    }
    c->s    = s;
    c->sock = cs;
    snprintf(c->peer, sizeof(c->peer), "%s", peer);
    mutex_lock(&s->lock);
    s->conns ++;
    mutex_unlock(&s->lock);
    if (thread_start_detached(conn_thread, c))
    {
      mutex_lock(&s->lock);
      s->conns --;
      mutex_unlock(&s->lock);
      net_close(cs);
      free(c);
    }
  }

  mutex_lock(&s->lock);
  cond_broadcast(&s->cond);
  mutex_unlock(&s->lock);
  thread_join(s->worker);
  net_close(s->ls);
  s->ls = SOCK_INVALID;
  log_msg(LOG_INFO, "sp410-ippd stopped");
  return 0;
}

void
server_stop(server_t *s)
{
  FLAG_SET(&s->stop, 1);
}

void
server_destroy(server_t *s)
{
  int64_t deadline = time_ms() + 5000;

  if (!s)
    return;
  /* Let connection threads notice s->stop; a thread stuck mid-request keeps
   * the server object alive (leaked) rather than risk a use-after-free. */
  mutex_lock(&s->lock);
  while (s->conns > 0 && time_ms() < deadline)
    cond_timedwait(&s->cond, &s->lock, 200);
  if (s->conns > 0)
  {
    mutex_unlock(&s->lock);
    log_msg(LOG_WARN, "%d connection(s) still open at shutdown", s->conns);
    return;
  }
  mutex_unlock(&s->lock);
  for (int i = 0; i < s->njobs; i ++)
    job_free(s->jobs[i]);
  mutex_destroy(&s->lock);
  cond_destroy(&s->cond);
  free(s);
}

/*
 * ipp.h - Internet Printing Protocol message encoding (RFC 8010).
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_IPP_H
#define SP410_IPP_H

#include <stddef.h>
#include <stdint.h>

/* delimiter tags (groups) */
enum
{
  IPP_TAG_OPERATION   = 0x01,
  IPP_TAG_JOB         = 0x02,
  IPP_TAG_END         = 0x03,
  IPP_TAG_PRINTER     = 0x04,
  IPP_TAG_UNSUPPORTED_GROUP = 0x05
};

/* value tags */
enum
{
  IPP_TAG_UNSUPPORTED_VALUE = 0x10,
  IPP_TAG_UNKNOWN     = 0x12,
  IPP_TAG_NOVALUE     = 0x13,
  IPP_TAG_INTEGER     = 0x21,
  IPP_TAG_BOOLEAN     = 0x22,
  IPP_TAG_ENUM        = 0x23,
  IPP_TAG_STRING      = 0x30,
  IPP_TAG_DATE        = 0x31,
  IPP_TAG_RESOLUTION  = 0x32,
  IPP_TAG_RANGE       = 0x33,
  IPP_TAG_BEGIN_COLLECTION = 0x34,
  IPP_TAG_TEXTLANG    = 0x35,
  IPP_TAG_NAMELANG    = 0x36,
  IPP_TAG_END_COLLECTION = 0x37,
  IPP_TAG_TEXT        = 0x41,
  IPP_TAG_NAME        = 0x42,
  IPP_TAG_KEYWORD     = 0x44,
  IPP_TAG_URI         = 0x45,
  IPP_TAG_URISCHEME   = 0x46,
  IPP_TAG_CHARSET     = 0x47,
  IPP_TAG_LANGUAGE    = 0x48,
  IPP_TAG_MIMETYPE    = 0x49,
  IPP_TAG_MEMBERNAME  = 0x4A
};

/* operations */
enum
{
  IPP_OP_PRINT_JOB              = 0x0002,
  IPP_OP_VALIDATE_JOB           = 0x0004,
  IPP_OP_CREATE_JOB             = 0x0005,
  IPP_OP_SEND_DOCUMENT          = 0x0006,
  IPP_OP_CANCEL_JOB             = 0x0008,
  IPP_OP_GET_JOB_ATTRIBUTES     = 0x0009,
  IPP_OP_GET_JOBS               = 0x000A,
  IPP_OP_GET_PRINTER_ATTRIBUTES = 0x000B,
  IPP_OP_CANCEL_MY_JOBS         = 0x0039,
  IPP_OP_CLOSE_JOB              = 0x003B,
  IPP_OP_IDENTIFY_PRINTER       = 0x003C
};

/* status codes */
enum
{
  IPP_OK                        = 0x0000,
  IPP_OK_IGNORED_OR_SUBSTITUTED = 0x0001,
  IPP_BAD_REQUEST               = 0x0400,
  IPP_FORBIDDEN                 = 0x0401,
  IPP_NOT_POSSIBLE              = 0x0404,
  IPP_NOT_FOUND                 = 0x0406,
  IPP_REQUEST_ENTITY_TOO_LARGE  = 0x0409,
  IPP_DOCUMENT_FORMAT_NOT_SUPPORTED = 0x040A,
  IPP_ATTRIBUTES_OR_VALUES      = 0x040B,
  IPP_INTERNAL_ERROR            = 0x0500,
  IPP_OPERATION_NOT_SUPPORTED   = 0x0501,
  IPP_VERSION_NOT_SUPPORTED     = 0x0503,
  IPP_BUSY                      = 0x0507
};

typedef struct ipp_attr_s ipp_attr_t;

typedef struct
{
  int         tag;
  size_t      len;
  uint8_t    *data;     /* raw value bytes (NUL-terminated copy for convenience) */
  ipp_attr_t *members;  /* begCollection: member attributes */
} ipp_value_t;

struct ipp_attr_s
{
  int          group;
  char        *name;
  int          num_values;
  ipp_value_t *values;
  ipp_attr_t  *next;
};

typedef struct
{
  int         major, minor;
  int         op;                 /* operation-id (requests) */
  uint32_t    request_id;
  ipp_attr_t *attrs;
} ipp_t;

/*
 * Parse an IPP request from buf.  On success returns 0 and sets *consumed to
 * the offset of the document data following the end-of-attributes tag.
 */
int          ipp_parse(const uint8_t *buf, size_t len, ipp_t *ipp, size_t *consumed,
                       char *err, size_t errlen);
void         ipp_free(ipp_t *ipp);

/* Lookup helpers (group < 0 matches any group). */
ipp_attr_t  *ipp_find(const ipp_t *ipp, int group, const char *name);
ipp_attr_t  *ipp_member(const ipp_value_t *coll, const char *name);
int          ipp_int(const ipp_attr_t *a, int idx, int def);
const char  *ipp_str(const ipp_attr_t *a, int idx);   /* NULL if absent */
int          ipp_has_str(const ipp_attr_t *a, const char *s);

/* ---- response writer ---------------------------------------------------- */

typedef struct
{
  uint8_t *data;
  size_t   len, cap;
  int      err;
} ipp_buf_t;

void ipp_w_header(ipp_buf_t *b, int major, int minor, int status, uint32_t request_id);
void ipp_w_group(ipp_buf_t *b, int tag);
void ipp_w_end(ipp_buf_t *b);
/* An empty or NULL name writes an additional value / collection member value. */
void ipp_w_str(ipp_buf_t *b, int tag, const char *name, const char *value);
void ipp_w_strs(ipp_buf_t *b, int tag, const char *name, int n, const char *const *values);
void ipp_w_int(ipp_buf_t *b, int tag, const char *name, int value);
void ipp_w_ints(ipp_buf_t *b, int tag, const char *name, int n, const int *values);
void ipp_w_bool(ipp_buf_t *b, const char *name, int value);
void ipp_w_range(ipp_buf_t *b, const char *name, int lo, int hi);
void ipp_w_res(ipp_buf_t *b, const char *name, int x, int y);
void ipp_w_novalue(ipp_buf_t *b, int tag, const char *name);
void ipp_w_coll_begin(ipp_buf_t *b, const char *name);
void ipp_w_member(ipp_buf_t *b, const char *member);
void ipp_w_coll_end(ipp_buf_t *b);
void ipp_buf_free(ipp_buf_t *b);

#endif /* SP410_IPP_H */

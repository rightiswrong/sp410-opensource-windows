/*
 * ipp.c - IPP/1.1 and IPP/2.0 message parsing and writing (RFC 8010).
 *
 * Wire format of a request:
 *   version (2 bytes) | operation-id (2) | request-id (4)
 *   { delimiter-tag | { value-tag | name-length(2) name | value-length(2) value }* }*
 *   end-of-attributes-tag (0x03) | document data
 * An attribute with name-length 0 adds a value to the previous attribute.
 * Collections are begCollection ... memberAttrName/value pairs ... endCollection.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ipp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8

typedef struct
{
  const uint8_t *p;
  size_t         len, pos;
  char          *err;
  size_t         errlen;
} rd_t;

static int
fail(rd_t *r, const char *msg)
{
  if (r->err && r->errlen)
    snprintf(r->err, r->errlen, "%s at offset %zu", msg, r->pos);
  return -1;
}

static int
rd_u16(rd_t *r, unsigned *v)
{
  if (r->len - r->pos < 2)
    return fail(r, "truncated IPP message");
  *v = ((unsigned)r->p[r->pos] << 8) | r->p[r->pos + 1];
  r->pos += 2;
  return 0;
}

static int
rd_bytes(rd_t *r, size_t n, const uint8_t **out)
{
  if (r->len - r->pos < n)
    return fail(r, "truncated IPP message");
  *out = r->p + r->pos;
  r->pos += n;
  return 0;
}

static char *
dupn(const uint8_t *s, size_t n)
{
  char *d = malloc(n + 1);
  if (d)
  {
    memcpy(d, s, n);
    d[n] = '\0';
  }
  return d;
}

static void free_attrs(ipp_attr_t *a);

static void
free_value(ipp_value_t *v)
{
  free(v->data);
  free_attrs(v->members);
}

static void
free_attrs(ipp_attr_t *a)
{
  while (a)
  {
    ipp_attr_t *n = a->next;
    for (int i = 0; i < a->num_values; i ++)
      free_value(&a->values[i]);
    free(a->values);
    free(a->name);
    free(a);
    a = n;
  }
}

static ipp_value_t *
add_value(ipp_attr_t *a, int tag, const uint8_t *data, size_t len)
{
  ipp_value_t *nv = realloc(a->values, sizeof(*nv) * (size_t)(a->num_values + 1));
  ipp_value_t *v;

  if (!nv)
    return NULL;
  a->values = nv;
  v = &a->values[a->num_values ++];
  memset(v, 0, sizeof(*v));
  v->tag  = tag;
  v->len  = len;
  v->data = (uint8_t *)dupn(data, len);
  return v->data ? v : NULL;
}

static int parse_collection(rd_t *r, ipp_value_t *v, int depth);

/* Read one value-tag/name/value triple (the tag byte is already consumed). */
static int
read_tvn(rd_t *r, unsigned *name_len, const uint8_t **name, unsigned *val_len,
         const uint8_t **val)
{
  if (rd_u16(r, name_len) || rd_bytes(r, *name_len, name) ||
      rd_u16(r, val_len) || rd_bytes(r, *val_len, val))
    return -1;
  return 0;
}

static int
parse_collection(rd_t *r, ipp_value_t *coll, int depth)
{
  ipp_attr_t *last = NULL, *cur = NULL;

  if (depth > MAX_DEPTH)
    return fail(r, "collections nested too deeply");

  for (;;)
  {
    unsigned       tag, nl, vl;
    const uint8_t *name, *val;

    if (r->pos >= r->len)
      return fail(r, "unterminated collection");
    tag = r->p[r->pos ++];
    if (read_tvn(r, &nl, &name, &vl, &val))
      return -1;

    if (tag == IPP_TAG_END_COLLECTION)
      return 0;

    if (tag == IPP_TAG_MEMBERNAME)
    {
      ipp_attr_t *a = calloc(1, sizeof(*a));

      if (!a || !(a->name = dupn(val, vl)))
      {
        free(a);
        return fail(r, "out of memory");
      }
      if (last)
        last->next = a;
      else
        coll->members = a;
      last = cur = a;
      continue;
    }

    if (!cur)
      return fail(r, "collection value without member name");
    {
      ipp_value_t *v = add_value(cur, (int)tag, val, vl);

      if (!v)
        return fail(r, "out of memory");
      if (tag == IPP_TAG_BEGIN_COLLECTION && parse_collection(r, v, depth + 1))
        return -1;
    }
  }
}

int
ipp_parse(const uint8_t *buf, size_t len, ipp_t *ipp, size_t *consumed,
          char *err, size_t errlen)
{
  rd_t        r = { buf, len, 0, err, errlen };
  ipp_attr_t *last = NULL, *cur = NULL;
  int         group = 0;

  memset(ipp, 0, sizeof(*ipp));
  if (len < 9)
    return fail(&r, "IPP message too short");
  ipp->major      = buf[0];
  ipp->minor      = buf[1];
  ipp->op         = (buf[2] << 8) | buf[3];
  ipp->request_id = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                    ((uint32_t)buf[6] << 8) | buf[7];
  r.pos = 8;

  for (;;)
  {
    unsigned       tag, nl, vl;
    const uint8_t *name, *val;

    if (r.pos >= r.len)
    {
      ipp_free(ipp);
      return fail(&r, "missing end-of-attributes tag");
    }
    tag = buf[r.pos ++];

    if (tag == IPP_TAG_END)
      break;
    if (tag < 0x10)                       /* delimiter: new group */
    {
      group = (int)tag;
      cur   = NULL;
      continue;
    }
    if (read_tvn(&r, &nl, &name, &vl, &val))
    {
      ipp_free(ipp);
      return -1;
    }

    if (nl)
    {
      ipp_attr_t *a = calloc(1, sizeof(*a));

      if (!a || !(a->name = dupn(name, nl)))
      {
        free(a);
        ipp_free(ipp);
        return fail(&r, "out of memory");
      }
      a->group = group;
      if (last)
        last->next = a;
      else
        ipp->attrs = a;
      last = cur = a;
    }
    else if (!cur)
    {
      ipp_free(ipp);
      return fail(&r, "additional value without an attribute");
    }

    {
      ipp_value_t *v = add_value(cur, (int)tag, val, vl);

      if (!v)
      {
        ipp_free(ipp);
        return fail(&r, "out of memory");
      }
      if (tag == IPP_TAG_BEGIN_COLLECTION && parse_collection(&r, v, 1))
      {
        ipp_free(ipp);
        return -1;
      }
    }
  }

  *consumed = r.pos;
  return 0;
}

void
ipp_free(ipp_t *ipp)
{
  free_attrs(ipp->attrs);
  ipp->attrs = NULL;
}

ipp_attr_t *
ipp_find(const ipp_t *ipp, int group, const char *name)
{
  for (ipp_attr_t *a = ipp->attrs; a; a = a->next)
    if ((group < 0 || a->group == group) && !strcmp(a->name, name))
      return a;
  return NULL;
}

ipp_attr_t *
ipp_member(const ipp_value_t *coll, const char *name)
{
  if (!coll)
    return NULL;
  for (ipp_attr_t *a = coll->members; a; a = a->next)
    if (!strcmp(a->name, name))
      return a;
  return NULL;
}

int
ipp_int(const ipp_attr_t *a, int idx, int def)
{
  const ipp_value_t *v;

  if (!a || idx >= a->num_values)
    return def;
  v = &a->values[idx];
  if ((v->tag == IPP_TAG_INTEGER || v->tag == IPP_TAG_ENUM) && v->len == 4)
    return (int)(((uint32_t)v->data[0] << 24) | ((uint32_t)v->data[1] << 16) |
                 ((uint32_t)v->data[2] << 8) | v->data[3]);
  if (v->tag == IPP_TAG_BOOLEAN && v->len == 1)
    return v->data[0];
  return def;
}

const char *
ipp_str(const ipp_attr_t *a, int idx)
{
  const ipp_value_t *v;

  if (!a || idx >= a->num_values)
    return NULL;
  v = &a->values[idx];
  if (v->tag == IPP_TAG_TEXTLANG || v->tag == IPP_TAG_NAMELANG)
  {
    /* lang-length(2) lang text-length(2) text */
    if (v->len >= 4)
    {
      unsigned ll = ((unsigned)v->data[0] << 8) | v->data[1];
      if (ll + 4 <= v->len)
        return (const char *)v->data + ll + 4;
    }
    return NULL;
  }
  if (v->tag < 0x40)
    return NULL;
  return (const char *)v->data;
}

int
ipp_has_str(const ipp_attr_t *a, const char *s)
{
  for (int i = 0; a && i < a->num_values; i ++)
  {
    const char *v = ipp_str(a, i);
    if (v && !strcmp(v, s))
      return 1;
  }
  return 0;
}

/* ---- writer --------------------------------------------------------------- */

static void
put(ipp_buf_t *b, const void *data, size_t len)
{
  if (b->err)
    return;
  if (b->len + len > b->cap)
  {
    size_t   nc = b->cap ? b->cap * 2 : 4096;
    uint8_t *nd;
    while (nc < b->len + len)
      nc *= 2;
    if ((nd = realloc(b->data, nc)) == NULL)
    {
      b->err = 1;
      return;
    }
    b->data = nd;
    b->cap  = nc;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
}

static void
put_u8(ipp_buf_t *b, unsigned v)
{
  uint8_t c = (uint8_t)v;
  put(b, &c, 1);
}

static void
put_u16(ipp_buf_t *b, unsigned v)
{
  uint8_t c[2] = { (uint8_t)(v >> 8), (uint8_t)v };
  put(b, c, 2);
}

static void
put_u32(ipp_buf_t *b, uint32_t v)
{
  uint8_t c[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
  put(b, c, 4);
}

static void
put_name(ipp_buf_t *b, int tag, const char *name)
{
  size_t n = name ? strlen(name) : 0;

  put_u8(b, (unsigned)tag);
  put_u16(b, (unsigned)n);
  if (n)
    put(b, name, n);
}

void
ipp_w_header(ipp_buf_t *b, int major, int minor, int status, uint32_t request_id)
{
  put_u8(b, (unsigned)major);
  put_u8(b, (unsigned)minor);
  put_u16(b, (unsigned)status);
  put_u32(b, request_id);
}

void ipp_w_group(ipp_buf_t *b, int tag) { put_u8(b, (unsigned)tag); }
void ipp_w_end(ipp_buf_t *b)            { put_u8(b, IPP_TAG_END); }

void
ipp_w_str(ipp_buf_t *b, int tag, const char *name, const char *value)
{
  size_t n = value ? strlen(value) : 0;

  if (n > 32767)
    n = 32767;
  put_name(b, tag, name);
  put_u16(b, (unsigned)n);
  put(b, value, n);
}

void
ipp_w_strs(ipp_buf_t *b, int tag, const char *name, int n, const char *const *values)
{
  for (int i = 0; i < n; i ++)
    ipp_w_str(b, tag, i ? NULL : name, values[i]);
}

void
ipp_w_int(ipp_buf_t *b, int tag, const char *name, int value)
{
  put_name(b, tag, name);
  put_u16(b, 4);
  put_u32(b, (uint32_t)value);
}

void
ipp_w_ints(ipp_buf_t *b, int tag, const char *name, int n, const int *values)
{
  for (int i = 0; i < n; i ++)
    ipp_w_int(b, tag, i ? NULL : name, values[i]);
}

void
ipp_w_bool(ipp_buf_t *b, const char *name, int value)
{
  put_name(b, IPP_TAG_BOOLEAN, name);
  put_u16(b, 1);
  put_u8(b, value ? 1 : 0);
}

void
ipp_w_range(ipp_buf_t *b, const char *name, int lo, int hi)
{
  put_name(b, IPP_TAG_RANGE, name);
  put_u16(b, 8);
  put_u32(b, (uint32_t)lo);
  put_u32(b, (uint32_t)hi);
}

void
ipp_w_res(ipp_buf_t *b, const char *name, int x, int y)
{
  put_name(b, IPP_TAG_RESOLUTION, name);
  put_u16(b, 9);
  put_u32(b, (uint32_t)x);
  put_u32(b, (uint32_t)y);
  put_u8(b, 3);                        /* dots per inch */
}

void
ipp_w_novalue(ipp_buf_t *b, int tag, const char *name)
{
  put_name(b, tag, name);
  put_u16(b, 0);
}

void
ipp_w_coll_begin(ipp_buf_t *b, const char *name)
{
  put_name(b, IPP_TAG_BEGIN_COLLECTION, name);
  put_u16(b, 0);
}

void
ipp_w_member(ipp_buf_t *b, const char *member)
{
  put_name(b, IPP_TAG_MEMBERNAME, NULL);
  put_u16(b, (unsigned)strlen(member));
  put(b, member, strlen(member));
}

void
ipp_w_coll_end(ipp_buf_t *b)
{
  put_name(b, IPP_TAG_END_COLLECTION, NULL);
  put_u16(b, 0);
}

void
ipp_buf_free(ipp_buf_t *b)
{
  free(b->data);
  memset(b, 0, sizeof(*b));
}

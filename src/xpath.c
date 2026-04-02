/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE  /* for strdup, strndup */

#include "xpath.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* - Dynamic string buffer ----------------------------------------- */

struct strbuf {
  char   *data;
  size_t  len;
  size_t  cap;
};

static void strbuf_init(struct strbuf *sb)
{
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
}

static int strbuf_grow(struct strbuf *sb, size_t need)
{
  if (sb->len + need + 1 <= sb->cap)
    return 0;
  size_t newcap = sb->cap ? sb->cap * 2 : 128;
  while (newcap < sb->len + need + 1)
    newcap *= 2;
  char *p = realloc(sb->data, newcap);
  if (!p)
    return -1;
  sb->data = p;
  sb->cap = newcap;
  return 0;
}

static int strbuf_append(struct strbuf *sb, const char *s, size_t slen)
{
  if (strbuf_grow(sb, slen) < 0)
    return -1;
  memcpy(sb->data + sb->len, s, slen);
  sb->len += slen;
  sb->data[sb->len] = '\0';
  return 0;
}

static int strbuf_addstr(struct strbuf *sb, const char *s)
{
  return strbuf_append(sb, s, strlen(s));
}

static int strbuf_addch(struct strbuf *sb, char c)
{
  return strbuf_append(sb, &c, 1);
}

static char *strbuf_detach(struct strbuf *sb)
{
  char *r = sb->data;
  sb->data = NULL;
  sb->len = sb->cap = 0;
  return r;
}

/* - gnmi_to_xpath ------------------------------------------------- */

char *gnmi_to_xpath(const Gnmi__Path *path, char **err_msg)
{
  if (!path || path->n_elem == 0)
    return strdup("/*");

  struct strbuf sb;
  strbuf_init(&sb);

  for (size_t i = 0; i < path->n_elem; i++) {
    Gnmi__PathElem *e = path->elem[i];

    /* Reject relative paths */
    if (strcmp(e->name, "..") == 0) {
      if (err_msg)
        *err_msg = strdup("Relative paths not allowed");
      free(sb.data);
      return NULL;
    }

    /* Reject XPath function injection in element names */
    if (strchr(e->name, '(') || strchr(e->name, ')')) {
      if (err_msg)
        *err_msg = strdup(
          "Invalid characters in path element");
      free(sb.data);
      return NULL;
    }

    strbuf_addch(&sb, '/');
    strbuf_addstr(&sb, e->name);

    /* Process key predicates */
    for (size_t k = 0; k < e->n_key; k++) {
      const char *key = e->key[k]->key;
      const char *val = e->key[k]->value;

      /* XPath injection: reject values with both ' and " */
      int has_single = (strchr(val, '\'') != NULL);
      int has_double = (strchr(val, '"') != NULL);
      if (has_single && has_double) {
        if (err_msg)
          *err_msg = strdup(
            "Double and single quotes "
            "in values not allowed");
        free(sb.data);
        return NULL;
      }

      char delim = has_double ? '\'' : '"';
      strbuf_addch(&sb, '[');
      strbuf_addstr(&sb, key);
      strbuf_addch(&sb, '=');
      strbuf_addch(&sb, delim);
      strbuf_addstr(&sb, val);
      strbuf_addch(&sb, delim);
      strbuf_addch(&sb, ']');
    }
  }

  return strbuf_detach(&sb);
}

/* - gnmi_merge_xpath ---------------------------------------------- */

char *gnmi_merge_xpath(const Gnmi__Path *prefix, const Gnmi__Path *path, char **err_msg)
{
  struct strbuf sb;
  strbuf_init(&sb);

  /* Merge prefix if it has elements */
  if (prefix && prefix->n_elem > 0) {
    char *pfx = gnmi_to_xpath(prefix, err_msg);
    if (!pfx) {
      free(sb.data);
      return NULL;
    }
    strbuf_addstr(&sb, pfx);
    free(pfx);
  }

  /* Merge path */
  char *p = gnmi_to_xpath(path, err_msg);
  if (!p) {
    free(sb.data);
    return NULL;
  }
  strbuf_addstr(&sb, p);
  free(p);

  return strbuf_detach(&sb);
}

/* - gnmi_check_origin --------------------------------------------- */

int gnmi_check_origin(const Gnmi__Path *prefix, const Gnmi__Path *path, char **err_msg)
{
  /* Accept empty/NULL origin as default (most gNMI clients don't set it).
   * Only reject if origin is explicitly set to something other than rfc7951.
   * Check both prefix and path - either can carry a bad origin. */
  const struct { const Gnmi__Path *p; const char *label; } checks[] = {
    { prefix, "prefix" },
    { path,   "path" },
  };

  for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
    const char *origin = checks[i].p ? checks[i].p->origin : NULL;
    if (origin && origin[0] != '\0' && strcmp(origin, "rfc7951") != 0) {
      if (err_msg) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s has unsupported origin \"%s\""
           " (expected \"rfc7951\" or empty)",
           checks[i].label, origin);
        *err_msg = strdup(buf);
      }
      return -1;
    }
  }
  return 0;
}

/* - node_to_gnmi_path --------------------------------------------- */

int node_to_gnmi_path(const struct lyd_node *node, Gnmi__Path *out)
{
  if (!node || !out)
    return -1;

  gnmi__path__init(out);
  out->origin = strdup("rfc7951");

  /* Count depth */
  int depth = 0;
  for (const struct lyd_node *n = node; n; n = lyd_parent(n))
    depth++;

  if (depth == 0)
    return 0;

  /* Allocate elem array */
  out->n_elem = depth;
  out->elem = calloc(depth, sizeof(Gnmi__PathElem *));
  if (!out->elem)
    return -1;

  /* Fill bottom-up, store in array top-down */
  int idx = depth - 1;
  const char *prev_mod = NULL;

  for (const struct lyd_node *n = node; n; n = lyd_parent(n), idx--) {
    Gnmi__PathElem *elem = calloc(1, sizeof(*elem));
    gnmi__path_elem__init(elem);

    const char *mod_name = NULL;
    if (n->schema && n->schema->module)
      mod_name = n->schema->module->name;

    /* Build name: "module:name" only if module differs from parent */
    const char *node_name = LYD_NAME(n);
    if (mod_name && (!prev_mod || strcmp(mod_name, prev_mod) != 0)) {
      size_t mlen = strlen(mod_name) + 1 + strlen(node_name) + 1;
      char *full = malloc(mlen);
      snprintf(full, mlen, "%s:%s", mod_name, node_name);
      elem->name = full;
    } else {
      elem->name = strdup(node_name);
    }
    prev_mod = mod_name;

    /* Extract list keys */
    if (n->schema &&
        n->schema->nodetype == LYS_LIST &&
        lyd_child(n)) {
      /* Count keys first */
      size_t nkeys = 0;
      const struct lyd_node *c;
      LY_LIST_FOR(lyd_child(n), c) {
        if (c->schema &&
            c->schema->nodetype == LYS_LEAF &&
            lysc_is_key(c->schema))
          nkeys++;
      }

      if (nkeys > 0) {
        elem->n_key = nkeys;
        elem->key = calloc(nkeys, sizeof(Gnmi__PathElem__KeyEntry *));
        size_t ki = 0;
        LY_LIST_FOR(lyd_child(n), c) {
          if (c->schema &&
              c->schema->nodetype == LYS_LEAF &&
              lysc_is_key(c->schema)) {
            Gnmi__PathElem__KeyEntry *ke =
              calloc(1, sizeof(*ke));
            gnmi__path_elem__key_entry__init(ke);
            ke->key = strdup(LYD_NAME(c));
            ke->value = strdup( lyd_get_value(c));
            elem->key[ki++] = ke;
          }
        }
      }
    }

    out->elem[idx] = elem;
  }

  /* Fix module prefix suppression: walk top-down, suppress if same as parent */
  for (int i = 1; i < depth; i++) {
    /* Check if the module prefix of elem[i] matches elem[i-1] */
    const char *prev_name = out->elem[i - 1]->name;
    const char *cur_name = out->elem[i]->name;
    const char *prev_colon = strchr(prev_name, ':');
    const char *cur_colon = strchr(cur_name, ':');

    if (prev_colon && cur_colon) {
      size_t prev_mlen = prev_colon - prev_name;
      size_t cur_mlen = cur_colon - cur_name;
      if (prev_mlen == cur_mlen &&
          strncmp(prev_name, cur_name, prev_mlen) == 0) {
        /* Same module - strip prefix from child */
        char *stripped = strdup(cur_colon + 1);
        free(out->elem[i]->name);
        out->elem[i]->name = stripped;
      }
    }
  }

  return 0;
}

/* - get_time_nanosec ---------------------------------------------- */

uint64_t get_time_nanosec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* - gnmi_path_free_elems ------------------------------------------ */

/* Check if a pointer is a protobuf-c static default (must not be freed) */
static int is_protobuf_static(const void *ptr)
{
  extern const char protobuf_c_empty_string[];
  return ptr == NULL || ptr == (const void *)protobuf_c_empty_string;
}

void gnmi_path_free_elems(Gnmi__Path *path)
{
  if (!path)
    return;
  for (size_t i = 0; i < path->n_elem; i++) {
    Gnmi__PathElem *e = path->elem[i];
    if (!e)
      continue;
    if (!is_protobuf_static(e->name))
      free(e->name);
    for (size_t k = 0; k < e->n_key; k++) {
      if (e->key[k]) {
        if (!is_protobuf_static(e->key[k]->key))
          free(e->key[k]->key);
        if (!is_protobuf_static(e->key[k]->value))
          free(e->key[k]->value);
        free(e->key[k]);
      }
    }
    free(e->key);
    free(e);
  }
  free(path->elem);
  path->elem = NULL;
  path->n_elem = 0;
  if (!is_protobuf_static(path->origin))
    free(path->origin);
  path->origin = NULL;
}

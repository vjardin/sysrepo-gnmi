/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "session.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

struct gnmi_session_registry {
  struct gnmi_session *head;
  uint32_t             count;
  uint32_t             total_streams;
};

static atomic_uint_least64_t next_id = 1;

gnmi_session_registry_t *gnmi_session_registry_create(void)
{
  gnmi_session_registry_t *reg = calloc(1, sizeof(*reg));
  return reg;
}

void gnmi_session_registry_destroy(gnmi_session_registry_t *reg)
{
  if (!reg)
    return;
  struct gnmi_session *s = reg->head;
  while (s) {
    struct gnmi_session *next = s->next;
    free(s->peer_addr);
    free(s->username);
    free(s);
    s = next;
  }
  free(reg);
}

struct gnmi_session *gnmi_session_get(gnmi_session_registry_t *reg,
    const char *peer_addr, const char *username)
{
  if (!reg || !peer_addr)
    return NULL;

  /* Look up existing session by peer address */
  for (struct gnmi_session *s = reg->head; s; s = s->next) {
    if (strcmp(s->peer_addr, peer_addr) == 0) {
      /* Update username if it changed (different RPC metadata) */
      if (username && username[0] &&
          (!s->username || strcmp(s->username, username) != 0)) {
        free(s->username);
        s->username = strdup(username);
      }
      return s;
    }
  }

  /* Create new session */
  struct gnmi_session *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  s->id = atomic_fetch_add(&next_id, 1);
  s->peer_addr = strdup(peer_addr);
  if (username && username[0])
    s->username = strdup(username);
  clock_gettime(CLOCK_MONOTONIC, &s->connected_at);
  s->last_rpc = s->connected_at;

  /* Prepend to list */
  s->next = reg->head;
  reg->head = s;
  reg->count++;

  gnmi_log(GNMI_LOG_INFO, "Session %lu: new from %s%s%s",
           (unsigned long)s->id, peer_addr,
           s->username ? " user=" : "",
           s->username ? s->username : "");

  return s;
}

void gnmi_session_touch(struct gnmi_session *s)
{
  if (!s)
    return;
  clock_gettime(CLOCK_MONOTONIC, &s->last_rpc);
  s->rpc_count++;
}

void gnmi_session_record_error(struct gnmi_session *s)
{
  if (s)
    s->rpc_errors++;
}

void gnmi_session_stream_add(struct gnmi_session *s)
{
  if (!s)
    return;
  s->active_streams++;
}

void gnmi_session_stream_del(struct gnmi_session *s)
{
  if (!s || s->active_streams <= 0)
    return;
  s->active_streams--;
  if (s->active_streams == 0)
    gnmi_log(GNMI_LOG_DEBUG, "Session %lu: last stream closed (peer %s)",
             (unsigned long)s->id, s->peer_addr);
}

uint32_t gnmi_session_count(const gnmi_session_registry_t *reg)
{
  return reg ? reg->count : 0;
}

uint32_t gnmi_session_stream_total(const gnmi_session_registry_t *reg)
{
  if (!reg)
    return 0;
  uint32_t total = 0;
  for (const struct gnmi_session *s = reg->head; s; s = s->next)
    total += s->active_streams;
  return total;
}

const struct gnmi_session *gnmi_session_first(const gnmi_session_registry_t *reg)
{
  return reg ? reg->head : NULL;
}

int gnmi_session_reap_idle(gnmi_session_registry_t *reg, unsigned int idle_secs)
{
  if (!reg)
    return 0;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  int reaped = 0;
  struct gnmi_session **pp = &reg->head;
  while (*pp) {
    struct gnmi_session *s = *pp;
    /* Don't reap sessions with active streams */
    if (s->active_streams > 0) {
      pp = &s->next;
      continue;
    }
    unsigned int idle = (unsigned int)(now.tv_sec - s->last_rpc.tv_sec);
    if (idle >= idle_secs) {
      gnmi_log(GNMI_LOG_INFO,
               "Session %lu: reaped (idle %us, peer %s, rpcs=%lu, errors=%lu)",
               (unsigned long)s->id, idle, s->peer_addr,
               (unsigned long)s->rpc_count, (unsigned long)s->rpc_errors);
      *pp = s->next;
      reg->count--;
      free(s->peer_addr);
      free(s->username);
      free(s);
      reaped++;
    } else {
      pp = &s->next;
    }
  }
  return reaped;
}

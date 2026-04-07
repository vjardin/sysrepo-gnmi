/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "session.h"
#include "monitoring.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <event2/event.h>

struct gnmi_session_registry {
  struct gnmi_session *head;
  uint32_t             count;
  struct event_base   *evbase;
  int                  max_sessions;
  int                  max_streams_per_session;
};

static atomic_uint_least64_t next_id = 1;

gnmi_session_registry_t *gnmi_session_registry_create(struct event_base *evbase,
    int max_sessions, int max_streams_per_session)
{
  gnmi_session_registry_t *reg = calloc(1, sizeof(*reg));
  if (reg) {
    reg->evbase = evbase;
    reg->max_sessions = max_sessions;
    reg->max_streams_per_session = max_streams_per_session;
  }
  return reg;
}

struct event_base *gnmi_session_registry_evbase(gnmi_session_registry_t *reg)
{
  return reg ? reg->evbase : NULL;
}

void gnmi_session_registry_destroy(gnmi_session_registry_t *reg)
{
  if (!reg)
    return;
  struct gnmi_session *s = reg->head;
  while (s) {
    struct gnmi_session *next = s->next;
    gnmi_session_candidate_release(s);
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

  /* Enforce max sessions limit */
  if (reg->max_sessions > 0 && (int)reg->count >= reg->max_sessions) {
    gnmi_log(GNMI_LOG_WARNING, "Max sessions reached (%d), rejecting %s",
             reg->max_sessions, peer_addr);
    return NULL;
  }

  /* Create new session */
  struct gnmi_session *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  s->id = atomic_fetch_add(&next_id, 1);
  s->peer_addr = strdup(peer_addr);
  if (username && username[0])
    s->username = strdup(username);
  s->evbase = reg->evbase;
  s->registry = reg;
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

  monitoring_notify_session_start(s->id, s->peer_addr, s->username);

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

int gnmi_session_stream_add(struct gnmi_session *s)
{
  if (!s)
    return -1;
  if (s->registry && s->registry->max_streams_per_session > 0 &&
      s->active_streams >= s->registry->max_streams_per_session) {
    gnmi_log(GNMI_LOG_WARNING,
             "Session %lu: max streams reached (%d), rejecting from %s",
             (unsigned long)s->id, s->registry->max_streams_per_session,
             s->peer_addr);
    return -1;
  }
  s->active_streams++;
  return 0;
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
      monitoring_notify_session_end(s->id, s->peer_addr, "idle-timeout", 0);
      *pp = s->next;
      reg->count--;
      gnmi_session_candidate_release(s);
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

/* - Kill session -------------------------------------------------- */

int gnmi_session_kill(gnmi_session_registry_t *reg, uint64_t target_id,
    uint64_t killer_id)
{
  if (!reg)
    return -1;

  struct gnmi_session **pp = &reg->head;
  while (*pp) {
    struct gnmi_session *s = *pp;
    if (s->id == target_id) {
      gnmi_log(GNMI_LOG_INFO,
               "Session %lu: killed by session %lu (peer %s)",
               (unsigned long)s->id, (unsigned long)killer_id,
               s->peer_addr);
      monitoring_notify_session_end(s->id, s->peer_addr, "killed", killer_id);
      *pp = s->next;
      reg->count--;
      gnmi_session_candidate_release(s);
      free(s->peer_addr);
      free(s->username);
      free(s);
      return 0;
    }
    pp = &s->next;
  }
  return -1;
}

/* - Per-session candidate datastore ------------------------------- */

const struct gnmi_session *gnmi_session_candidate_holder(
    const gnmi_session_registry_t *reg)
{
  if (!reg)
    return NULL;
  for (const struct gnmi_session *s = reg->head; s; s = s->next) {
    if (s->candidate_sess)
      return s;
  }
  return NULL;
}

void gnmi_session_candidate_release(struct gnmi_session *s)
{
  if (!s || !s->candidate_sess)
    return;
  if (s->candidate_idle_timer) {
    evtimer_del(s->candidate_idle_timer);
    event_free(s->candidate_idle_timer);
    s->candidate_idle_timer = NULL;
  }
  sr_unlock(s->candidate_sess, NULL);
  sr_session_stop(s->candidate_sess);
  s->candidate_sess = NULL;
  gnmi_log(GNMI_LOG_DEBUG, "Session %lu: candidate unlocked and released",
           (unsigned long)s->id);
}

void gnmi_session_candidate_cleanup_all(gnmi_session_registry_t *reg)
{
  if (!reg)
    return;
  for (struct gnmi_session *s = reg->head; s; s = s->next) {
    if (s->candidate_sess) {
      gnmi_log(GNMI_LOG_INFO, "Session %lu: discarding candidate on shutdown",
               (unsigned long)s->id);
      sr_discard_changes(s->candidate_sess);
      gnmi_session_candidate_release(s);
    }
  }
}

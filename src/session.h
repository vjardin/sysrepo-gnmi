/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdint.h>
#include <time.h>

/*
 * gNMI session tracking.
 *
 * A session represents a unique gRPC client peer (identified by its
 * IP:port).  Sessions are created on first RPC and removed when the
 * idle timeout fires or the server shuts down.
 *
 * Tracked state: session ID, peer address, username, timestamps,
 * active subscribe stream count, and RPC counters.
 */

struct gnmi_session {
  uint64_t              id;
  char                 *peer_addr;     /* from grpc_call_get_peer() */
  char                 *username;      /* from metadata or TLS cert */
  struct timespec       connected_at;
  struct timespec       last_rpc;
  int                   active_streams;
  uint64_t              rpc_count;
  uint64_t              rpc_errors;
  struct gnmi_session  *next;
};

/* Session registry (owned by gnmi_server) */
typedef struct gnmi_session_registry gnmi_session_registry_t;

/* Create / destroy the registry */
gnmi_session_registry_t *gnmi_session_registry_create(void);
void gnmi_session_registry_destroy(gnmi_session_registry_t *reg);

/* Look up or create a session for the given peer address.
 * If a new session is created, username is copied.
 * Returns the session (never NULL on success, NULL on OOM). */
struct gnmi_session *gnmi_session_get(gnmi_session_registry_t *reg,
    const char *peer_addr, const char *username);

/* Update last_rpc timestamp and increment rpc_count */
void gnmi_session_touch(struct gnmi_session *s);

/* Record an RPC error */
void gnmi_session_record_error(struct gnmi_session *s);

/* Increment / decrement active stream count */
void gnmi_session_stream_add(struct gnmi_session *s);
void gnmi_session_stream_del(struct gnmi_session *s);

/* Get total active sessions and streams */
uint32_t gnmi_session_count(const gnmi_session_registry_t *reg);
uint32_t gnmi_session_stream_total(const gnmi_session_registry_t *reg);

/* Iterate all sessions (for monitoring/logging).
 * Returns first session; use s->next to walk the list. */
const struct gnmi_session *gnmi_session_first(const gnmi_session_registry_t *reg);

/* Remove sessions idle for more than idle_secs.
 * Returns number of sessions removed. */
int gnmi_session_reap_idle(gnmi_session_registry_t *reg, unsigned int idle_secs);

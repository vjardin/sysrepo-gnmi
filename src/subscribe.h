/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdbool.h>
#include <event2/event.h>
#include <sysrepo.h>

#include "gnmi_service.h"
#include "gnmi.pb-c.h"

/*
 * Subscribe bidi streaming state machine.
 *
 * STREAM mode lifecycle:
 *   1. Send initial metadata
 *   2. Recv first SubscribeRequest
 *   3. Send initial data snapshot + sync_response
 *   4. Arm timers (SAMPLE) and sr subscriptions (ON_CHANGE)
 *   5. Simultaneously:
 *      - Pending RECV to detect client disconnect
 *      - evtimers fire -> fetch + send
 *      - event_active from sysrepo -> fetch changes + send
 *   6. Client disconnect or server shutdown -> cleanup
 */

/* Forward decl */
struct stream_ctx;

/* Per-subscription entry (one per Subscription in SubscriptionList) */
struct sub_entry {
  struct stream_ctx       *sctx;  /* back-pointer to owning stream */
  char                    *xpath;
  int                      mode;    /* GNMI__SUBSCRIPTION_MODE__* */
  uint64_t                 sample_interval_ns;

  /* SAMPLE: libevent timer on the shared event_base */
  struct event            *ev_timer;

  /* SAMPLE: suppress_redundant -- skip notifications if data unchanged */
  bool                     suppress_redundant;
  char                    *last_json;  /* previous serialized value, or NULL */

  /* ON_CHANGE: sysrepo subscription + libevent wakeup event */
  sr_subscription_ctx_t   *sr_sub;
  struct event            *ev_change;
  sr_session_ctx_t        *change_sess; /* stashed from callback */

  /* ON_CHANGE: heartbeat_interval -- periodic liveness notification */
  uint64_t                 heartbeat_ns;
  struct event            *ev_heartbeat;
};

struct stream_ctx {
  struct call_ctx  base;
  int              method_idx;

  /* Current recv buffer */
  grpc_byte_buffer *recv_msg;

  /* Send queue (simple: one pending send at a time) */
  grpc_byte_buffer *send_msg;

  /* State tracking */
  enum {
    STREAM_INIT,
    STREAM_RECV_FIRST,
    STREAM_SENDING,
    STREAM_RECV_POLL,
    STREAM_ACTIVE,      /* STREAM mode: timers/subs armed */
    STREAM_CLOSING,
    STREAM_DONE,
  } state;

  /* Subscription mode */
  enum {
    SUB_MODE_UNKNOWN,
    SUB_MODE_ONCE,
    SUB_MODE_POLL,
    SUB_MODE_STREAM,
  } mode;

  /* Original SubscribeRequest */
  Gnmi__SubscribeRequest *orig_req;

  /* Messages to send (queue) */
  grpc_byte_buffer **send_queue;
  size_t             send_queue_len;
  size_t             send_queue_idx;

  /* STREAM mode: subscription entries */
  struct sub_entry  *entries;
  size_t             n_entries;
  sr_session_ctx_t  *sr_sess;       /* session for data fetching */

  /* STREAM mode: pending recv for client disconnect detection */
  bool               recv_pending;

  /* allow_aggregation: merge multiple sub_entry samples into one
   * Notification before sending (gNMI 0.7.0 s3.5.1.1). */
  bool               allow_aggregation;
  Gnmi__Update     **agg_updates;     /* pending aggregated updates */
  size_t             agg_n_updates;

  /* Close status */
  grpc_status_code   close_code;
  char              *close_msg;
  int                close_cancelled;
};

/* Create and arm the streaming acceptor for Subscribe */
void subscribe_arm(gnmi_server_t *srv, int method_idx);

/* Step function for the streaming state machine */
void stream_step(struct call_ctx *base, bool success);

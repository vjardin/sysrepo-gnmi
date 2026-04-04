/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <event2/event.h>
#include <sysrepo.h>
#include <libyang/libyang.h>

#include "gnmi_service.h"

/* Confirmed-commit state machine.
 *
 * Flow:
 *   1. Set RPC with confirm_parms -> confirm_state_snapshot()
 *      - Snapshots running config via sr_get_data("/\*")
 *      - Arms evtimer with timeout_secs
 *   2. If Confirm RPC arrives before timeout -> confirm_state_confirm()
 *      - Copies running to startup
 *      - Clears state, cancels timer
 *   3. If timer fires -> confirm_state_restore()
 *      - Restores running config from snapshot via sr_replace_config()
 *      - Clears state
 */

typedef struct confirm_state {
  bool               wait_confirm;
  struct lyd_node   *cfg_snapshot;
  sr_session_ctx_t  *sr_sess;
  uint32_t           timeout_secs;
  uint32_t           min_wait_secs;
  uint64_t           earliest_ns;
  uint64_t           set_txn_id;
  uint64_t           confirmed_txn_id;
  struct event      *ev_timeout;
} confirm_state_t;

confirm_state_t *confirm_state_create(struct event_base *evbase, sr_conn_ctx_t *sr_conn);
void confirm_state_destroy(confirm_state_t *cs);

/* Called by Set RPC when confirm_parms is present */
int confirm_state_snapshot(confirm_state_t *cs, uint32_t timeout_secs, uint32_t *out_min_wait, uint32_t *out_timeout);

/* Called by Confirm RPC */
int confirm_state_confirm(confirm_state_t *cs, sr_conn_ctx_t *sr_conn, char **err_msg);

/* Called on timeout - restores config */
void confirm_state_restore(confirm_state_t *cs);

/* Clear state (cancel timer, free snapshot) */
void confirm_state_clear(confirm_state_t *cs);

bool confirm_state_waiting(confirm_state_t *cs);
uint64_t confirm_state_earliest_ns(confirm_state_t *cs);

/* Confirm RPC handler */
grpc_status_code handle_confirm(sr_conn_ctx_t *sr_conn, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
        char **status_msg);

/* Set the global confirm state (called from server init) */
void confirm_state_set_global(confirm_state_t *cs);
confirm_state_t *confirm_state_get_global(void);

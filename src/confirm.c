/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "confirm.h"
#include "xpath.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include "gnmi.pb-c.h"

#define DEFAULT_TIMEOUT_SECS  300
#define DEFAULT_MIN_WAIT_SECS 0  /* 0 for now; production should use 30 */

/*
 * Global singleton for the confirmed-commit state machine.
 *
 * Thread safety: accessed exclusively from the main libevent event loop
 * (gRPC CQ poll callback and evtimer callbacks). All access is
 * single-threaded. Do NOT access from sysrepo subscription callbacks.
 */
static confirm_state_t *g_confirm_state;

void confirm_state_set_global(confirm_state_t *cs)
{
  g_confirm_state = cs;
}

confirm_state_t *confirm_state_get_global(void)
{
  return g_confirm_state;
}

/* - Timer callback ------------------------------------------------ */

static void confirm_timeout_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  confirm_state_t *cs = arg;

  gnmi_log(GNMI_LOG_WARNING, "Confirmed-commit timeout - restoring config");
  confirm_state_restore(cs);
}

/* - Create / Destroy ---------------------------------------------- */

confirm_state_t *confirm_state_create(struct event_base *evbase, sr_conn_ctx_t *sr_conn)
{
  confirm_state_t *cs = calloc(1, sizeof(*cs));
  if (!cs)
    return NULL;

  cs->timeout_secs = DEFAULT_TIMEOUT_SECS;
  cs->min_wait_secs = DEFAULT_MIN_WAIT_SECS;

  /* Create a session for restore operations */
  int rc = sr_session_start(sr_conn, SR_DS_RUNNING, &cs->sr_sess);
  if (rc != SR_ERR_OK) {
    free(cs);
    return NULL;
  }

  /* Create evtimer (not armed yet) */
  cs->ev_timeout = evtimer_new(evbase, confirm_timeout_cb, cs);

  return cs;
}

void confirm_state_destroy(confirm_state_t *cs)
{
  if (!cs)
    return;
  confirm_state_clear(cs);
  if (cs->ev_timeout) {
    evtimer_del(cs->ev_timeout);
    event_free(cs->ev_timeout);
  }
  if (cs->sr_sess)
    sr_session_stop(cs->sr_sess);
  free(cs);
}

/* - Snapshot (called by Set) -------------------------------------- */

int confirm_state_snapshot(confirm_state_t *cs, uint32_t timeout_secs, uint32_t *out_min_wait, uint32_t *out_timeout)
{
  /* Clear any previous state */
  confirm_state_clear(cs);

  /* Snapshot running config */
  sr_data_t *data = NULL;
  int rc = sr_get_data(cs->sr_sess, "/*", 0, 0, 0, &data);
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_ERROR, "Confirm snapshot: sr_get_data: %s", sr_strerror(rc));
    return -1;
  }

  if (data && data->tree) {
    lyd_dup_siblings(data->tree, NULL, LYD_DUP_RECURSIVE, &cs->cfg_snapshot);
  }
  if (data)
    sr_release_data(data);

  /* Set timeout */
  cs->timeout_secs = timeout_secs ? timeout_secs : DEFAULT_TIMEOUT_SECS;
  cs->wait_confirm = true;
  cs->earliest_ns = get_time_nanosec() + (uint64_t)cs->min_wait_secs * NSEC_PER_SEC;

  /* Arm the timer */
  struct timeval tv = { .tv_sec = cs->timeout_secs, .tv_usec = 0 };
  evtimer_add(cs->ev_timeout, &tv);

  gnmi_log(GNMI_LOG_INFO, "Confirmed-commit: waiting for Confirm (timeout=%us, min_wait=%us)",
     cs->timeout_secs, cs->min_wait_secs);

  *out_min_wait = cs->min_wait_secs;
  *out_timeout = cs->timeout_secs;
  return 0;
}

/* - Confirm (called by Confirm RPC) ------------------------------ */

int confirm_state_confirm(confirm_state_t *cs, sr_conn_ctx_t *sr_conn, char **err_msg)
{
  /* Copy running to startup to persist */
  sr_session_ctx_t *startup_sess = NULL;
  int rc = sr_session_start(sr_conn, SR_DS_STARTUP, &startup_sess);
  if (rc != SR_ERR_OK) {
    if (err_msg)
      *err_msg = strdup("Failed to start startup session");
    return -1;
  }

  rc = sr_copy_config(startup_sess, NULL, SR_DS_RUNNING, 0);
  sr_session_stop(startup_sess);

  if (rc != SR_ERR_OK) {
    if (err_msg)
      *err_msg = strdup(sr_strerror(rc));
    return -1;
  }

  cs->confirmed_txn_id = cs->set_txn_id;
  confirm_state_clear(cs);

  gnmi_log(GNMI_LOG_INFO, "Confirmed-commit: confirmed and persisted");
  return 0;
}

/* - Restore (called on timeout) ----------------------------------- */

void confirm_state_restore(confirm_state_t *cs)
{
  if (!cs->wait_confirm)
    return;

  if (cs->cfg_snapshot) {
    gnmi_log(GNMI_LOG_INFO, "Confirmed-commit: restoring config from snapshot");
    int rc = sr_replace_config(cs->sr_sess, NULL, cs->cfg_snapshot, 0);
    if (rc != SR_ERR_OK) {
      gnmi_log(GNMI_LOG_ERROR, "Confirm restore: sr_replace_config: %s", sr_strerror(rc));
      lyd_free_all(cs->cfg_snapshot);
    }
    /* sr_replace_config takes ownership on success */
    cs->cfg_snapshot = NULL;
  }

  cs->set_txn_id = cs->confirmed_txn_id;
  cs->wait_confirm = false;
  evtimer_del(cs->ev_timeout);
}

/* - Clear --------------------------------------------------------- */

void confirm_state_clear(confirm_state_t *cs)
{
  cs->wait_confirm = false;
  evtimer_del(cs->ev_timeout);
  if (cs->cfg_snapshot) {
    lyd_free_all(cs->cfg_snapshot);
    cs->cfg_snapshot = NULL;
  }
}

bool confirm_state_waiting(confirm_state_t *cs)
{
  return cs && cs->wait_confirm;
}

uint64_t confirm_state_earliest_ns(confirm_state_t *cs)
{
  return cs ? cs->earliest_ns : 0;
}

/* - Confirm RPC handler ------------------------------------------- */

grpc_status_code handle_confirm(sr_conn_ctx_t *sr_conn,
        struct gnmi_session *session,
        grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
        char **status_msg)
{
  (void)session;
  Gnmi__ConfirmRequest *req = NULL;
  Gnmi__ConfirmResponse resp = GNMI__CONFIRM_RESPONSE__INIT;
  confirm_state_t *cs = confirm_state_get_global();
  grpc_status_code ret = GRPC_STATUS_INTERNAL;

  /* Unpack request */
  req = (Gnmi__ConfirmRequest *)gnmi_unpack( &gnmi__confirm_request__descriptor, request_bb);
  /* ConfirmRequest can be empty - that's fine */

  /* Check if we're actually waiting for confirmation */
  if (!confirm_state_waiting(cs)) {
    *status_msg = strdup("Not waiting for confirmation");
    ret = GRPC_STATUS_FAILED_PRECONDITION;
    goto cleanup;
  }

  /* Check min_wait time */
  uint64_t now = get_time_nanosec();
  uint64_t earliest = confirm_state_earliest_ns(cs);
  if (now < earliest) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Confirm too early, wait %lu more ns", (unsigned long)(earliest - now));
    *status_msg = strdup(buf);
    ret = GRPC_STATUS_UNAVAILABLE;
    goto cleanup;
  }

  /* Confirm: copy running to startup, clear state */
  if (confirm_state_confirm(cs, sr_conn, status_msg) < 0) {
    ret = GRPC_STATUS_ABORTED;
    goto cleanup;
  }

  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
  ret = GRPC_STATUS_OK;

cleanup:
  if (req)
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);
  return ret;
}

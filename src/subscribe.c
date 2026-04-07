/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "subscribe.h"
#include "server.h"
#include "xpath.h"
#include "encode.h"
#include "session.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <grpc/grpc.h>
#include <grpc/byte_buffer.h>
#include <grpc/support/alloc.h>
#include <sysrepo.h>
#include <libyang/libyang.h>

#include "gnmi.pb-c.h"

/* - Forward declarations ------------------------------------------ */

static void stream_send_next(struct stream_ctx *sctx);
static void stream_close(struct stream_ctx *sctx, grpc_status_code code, const char *msg);
static void stream_free(struct stream_ctx *sctx);

/* - Helpers ------------------------------------------------------- */

static grpc_call_error
batch_op(struct stream_ctx *sctx, grpc_op *ops, size_t n)
{
  return grpc_call_start_batch(sctx->base.call, ops, n, &sctx->base, NULL);
}

/* Max queued messages per stream before closing (backpressure).
 * Prevents OOM if a client reads slowly while timers keep firing. */
#define SEND_QUEUE_MAX  1024

/* Max subscriptions per SubscribeRequest (resource limit). */
#define MAX_SUBS_PER_STREAM  256

/* Queue a protobuf-c message for sending.
 * Returns 0 on success, -1 on failure (OOM or queue full). */
static int send_queue_push(struct stream_ctx *sctx, grpc_byte_buffer *bb)
{
  if (sctx->send_queue_len >= SEND_QUEUE_MAX) {
    gnmi_log(GNMI_LOG_WARNING, "Subscribe: send queue full (%zu), dropping client",
             sctx->send_queue_len);
    grpc_byte_buffer_destroy(bb);
    return -1;
  }
  grpc_byte_buffer **tmp = realloc(sctx->send_queue, (sctx->send_queue_len + 1) *
           sizeof(grpc_byte_buffer *));
  if (!tmp) {
    grpc_byte_buffer_destroy(bb);
    return -1;
  }
  sctx->send_queue = tmp;
  sctx->send_queue[sctx->send_queue_len++] = bb;
  return 0;
}

/* - Build notifications for current data -------------------------- */

static int
build_subscribe_data(struct stream_ctx *sctx, Gnmi__SubscriptionList *sublist)
{
  sr_conn_ctx_t *sr_conn = gnmi_server_get_sr_conn(sctx->base.srv);
  const char *user = sctx->session ? sctx->session->username : NULL;
  sr_session_ctx_t *sess = NULL;
  int rc = gnmi_nacm_session_start_as(sr_conn, SR_DS_OPERATIONAL,
    user, &sess);
  if (rc != SR_ERR_OK)
    return -1;

  /* Build one notification with updates for all subscription paths */
  Gnmi__Notification notif = GNMI__NOTIFICATION__INIT;
  notif.timestamp = get_time_nanosec();

  /* Collect all updates across subscription paths */
  size_t total_updates = 0;
  Gnmi__Update **all_updates = NULL;

  for (size_t i = 0; i < sublist->n_subscription; i++) {
    Gnmi__Subscription *sub = sublist->subscription[i];
    if (!sub->path)
      continue;

    /* Merge prefix + subscription path */
    char *fullpath = gnmi_merge_xpath(sublist->prefix, sub->path, NULL);
    if (!fullpath)
      continue;

    /* Fetch data */
    sr_data_t *sr_data = NULL;
    rc = sr_get_data(sess, fullpath, 0, 0, 0, &sr_data);
    if (rc != SR_ERR_OK || !sr_data || !sr_data->tree) {
      free(fullpath);
      if (sr_data)
        sr_release_data(sr_data);
      continue;
    }

    /* Find matching nodes */
    struct ly_set *set = NULL;
    lyd_find_xpath(sr_data->tree, fullpath, &set);
    if (set && set->count > 0) {
      for (uint32_t j = 0; j < set->count; j++) {
        Gnmi__Update *upd = calloc(1, sizeof(*upd));
        gnmi__update__init(upd);

        /* Encode path */
        upd->path = calloc(1, sizeof(*upd->path));
        node_to_gnmi_path(set->dnodes[j], upd->path);

        /* Encode value */
        upd->val = calloc(1, sizeof(*upd->val));
        encode_node(GNMI__ENCODING__JSON_IETF, set->dnodes[j], upd->val, NULL);

        {
        Gnmi__Update **tmp = realloc(all_updates, (total_updates + 1) *
          sizeof(Gnmi__Update *));
        if (!tmp) { free(upd); break; }
        all_updates = tmp;
        all_updates[total_updates++] = upd;
        }
      }
    }
    if (set)
      ly_set_free(set, NULL);
    sr_release_data(sr_data);
    free(fullpath);
  }

  notif.n_update = total_updates;
  notif.update = all_updates;

  /* Set prefix target if present */
  if (sublist->prefix && sublist->prefix->target &&
      sublist->prefix->target[0]) {
    notif.prefix = calloc(1, sizeof(*notif.prefix));
    gnmi__path__init(notif.prefix);
    notif.prefix->target = strdup(sublist->prefix->target);
  }

  /* Pack and queue the notification as a SubscribeResponse */
  int qrc = 0;
  if (total_updates > 0 || sublist->n_subscription > 0) {
    Gnmi__SubscribeResponse resp = GNMI__SUBSCRIBE_RESPONSE__INIT;
    resp.response_case =
      GNMI__SUBSCRIBE_RESPONSE__RESPONSE_UPDATE;
    resp.update = &notif;
    qrc = send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&resp));
  }

  /* Build sync_response */
  if (qrc == 0) {
    Gnmi__SubscribeResponse sync = GNMI__SUBSCRIBE_RESPONSE__INIT;
    sync.response_case =
      GNMI__SUBSCRIBE_RESPONSE__RESPONSE_SYNC_RESPONSE;
    sync.sync_response = 1;
    qrc = send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&sync));
  }

  /* Cleanup */
  for (size_t i = 0; i < total_updates; i++) {
    gnmi_update_free(all_updates[i]);
  }
  free(all_updates);

  if (notif.prefix) {
    free(notif.prefix->target);
    free(notif.prefix);
  }

  sr_session_stop(sess);
  return qrc;
}

/* - Validate subscribe request ------------------------------------ */

static grpc_status_code
validate_subscribe(Gnmi__SubscribeRequest *req, char **err_msg)
{
  if (req->request_case != GNMI__SUBSCRIBE_REQUEST__REQUEST_SUBSCRIBE) {
    *err_msg = strdup("First message must be SubscriptionList");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  Gnmi__SubscriptionList *sl = req->subscribe;
  if (!sl) {
    *err_msg = strdup("Empty subscription list");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Check subscription count */
  if (sl->n_subscription > MAX_SUBS_PER_STREAM) {
    *err_msg = strdup("Too many subscriptions");
    return GRPC_STATUS_RESOURCE_EXHAUSTED;
  }

  /* Check encoding */
  if (sl->encoding != GNMI__ENCODING__JSON_IETF &&
      sl->encoding != GNMI__ENCODING__JSON &&
      sl->encoding != GNMI__ENCODING__ASCII &&
      sl->encoding != GNMI__ENCODING__PROTO) {
    *err_msg = strdup("Unsupported encoding");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* TODO: use_aliases (gNMI 0.7.0 s3.5.1.6)
   * Aliases let the client define short names for frequently-used
   * paths, reducing per-message overhead on long-lived streams.
   * Not implemented because no mainstream gNMI client (gnmic, Arista
   * gNMIc, OpenConfig collector) uses this feature, and it requires
   * per-stream alias state management (map of alias->path). */
  if (sl->use_aliases) {
    *err_msg = strdup("use_aliases not supported");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Mode-specific checks */
  if (sl->mode == GNMI__SUBSCRIPTION_LIST__MODE__POLL) {
    if (sl->updates_only) {
      *err_msg = strdup(
        "updates_only not supported in POLL mode");
      return GRPC_STATUS_INVALID_ARGUMENT;
    }
  }

  return GRPC_STATUS_OK;
}

/* - State machine steps ------------------------------------------- */

/*
 * Bidi streaming flow:
 *
 * 1. accept call -> STREAM_INIT
 * 2. send initial metadata -> STREAM_RECV_FIRST
 * 3. recv first SubscribeRequest -> dispatch by mode
 *
 * ONCE:
 *   4. build data + sync -> queue
 *   5. send queue items one by one -> STREAM_SENDING
 *   6. after last -> close with OK
 *
 * POLL:
 *   4. build data + sync -> queue, send
 *   5. after last -> STREAM_RECV_POLL
 *   6. recv Poll message -> build data + sync, send
 *   7. loop to step 5
 *   8. recv fails (client done) -> close with OK
 */

static void step_send_initial_metadata(struct stream_ctx *sctx, bool success);
static void step_recv_first(struct stream_ctx *sctx, bool success);
static void step_send_msg(struct stream_ctx *sctx, bool success);
static void step_recv_poll(struct stream_ctx *sctx, bool success);
static void step_stream_recv(struct stream_ctx *sctx, bool success);
static void step_close_done(struct stream_ctx *sctx, bool success);
static void stream_arm_subscriptions(struct stream_ctx *sctx);
static void stream_try_send(struct stream_ctx *sctx);

void stream_step(struct call_ctx *base, bool success)
{
  struct stream_ctx *sctx = (struct stream_ctx *)base;

  switch (sctx->state) {
  case STREAM_INIT: step_send_initial_metadata(sctx, success);
    break;
  case STREAM_RECV_FIRST: step_recv_first(sctx, success);
    break;
  case STREAM_SENDING: step_send_msg(sctx, success);
    break;
  case STREAM_RECV_POLL: step_recv_poll(sctx, success);
    break;
  case STREAM_ACTIVE:
    /* CQ event while in STREAM mode = client recv completed
     * (disconnect or cancel) */
    step_stream_recv(sctx, success);
    break;
  case STREAM_CLOSING: step_close_done(sctx, success);
    break;
  case STREAM_DONE: stream_free(sctx);
    break;
  }
}

/* Step 1: call accepted -> send initial metadata */
static void step_send_initial_metadata(struct stream_ctx *sctx, bool success)
{
  if (!success) {
    stream_free(sctx);
    return;
  }

  /* Extract per-stream username from client metadata (gnmic --username). */
  const char *md_user = NULL;
  for (size_t i = 0; i < sctx->base.md_recv.count; i++) {
    grpc_slice key = sctx->base.md_recv.metadata[i].key;
    if (grpc_slice_eq(key, grpc_slice_from_static_string("username"))) {
      grpc_slice val = sctx->base.md_recv.metadata[i].value;
      sctx->stream_user = strndup(
        (const char *)GRPC_SLICE_START_PTR(val), GRPC_SLICE_LENGTH(val));
      md_user = sctx->stream_user;
      if (md_user)
        gnmi_log(GNMI_LOG_DEBUG, "Subscribe metadata username: %s", md_user);
      break;
    }
  }

  /* Track session for this stream */
  char *peer = grpc_call_get_peer(sctx->base.call);
  if (peer) {
    sctx->session = gnmi_session_get(
        gnmi_server_get_sessions(sctx->base.srv), peer, md_user);
    if (sctx->session) {
      gnmi_session_touch(sctx->session);
      gnmi_session_stream_add(sctx->session);
    }
    gpr_free(peer);
  }

  /* Re-arm to accept next Subscribe call */
  subscribe_arm(sctx->base.srv, sctx->method_idx);

  /* Send initial metadata */
  grpc_op op = {0};
  op.op = GRPC_OP_SEND_INITIAL_METADATA;
  op.data.send_initial_metadata.count = 0;

  sctx->state = STREAM_RECV_FIRST;
  if (batch_op(sctx, &op, 1) != GRPC_CALL_OK) {
    gnmi_log(GNMI_LOG_ERROR, "Subscribe: send metadata failed");
    stream_free(sctx);
  }
}

/* Step 2: metadata sent -> recv first SubscribeRequest */
static void step_recv_first(struct stream_ctx *sctx, bool success)
{
  if (!success) {
    stream_close(sctx, GRPC_STATUS_INTERNAL, "metadata send failed");
    return;
  }

  /* Issue recv for the first message */
  grpc_op op = {0};
  op.op = GRPC_OP_RECV_MESSAGE;
  op.data.recv_message.recv_message = &sctx->recv_msg;

  /* State stays STREAM_RECV_FIRST - the next CQ event re-enters
   * stream_step and we detect recv_msg != NULL in the same state.
   * Actually, we need a sub-state. Let's use a trick: set state to
   * a different value after recv completes. We handle it below. */

  /* We change state to a sentinel: when step_recv_first is called
   * again with recv_msg filled, we process the subscription. */
  sctx->state = STREAM_RECV_FIRST;

  /* But wait - after sending metadata, the CQ event comes back here.
   * The RECV hasn't been issued yet. Let me restructure: we combine
   * metadata send and message recv in one batch? No - gRPC requires
   * separate batches for send and recv.
   *
   * The flow is:
   *   step_send_initial_metadata: batch(SEND_INITIAL_METADATA) -> CQ
   *   -> step_recv_first called (success from metadata send)
   *   -> issue batch(RECV_MESSAGE) -> CQ
   *   -> stream_step called again, state still STREAM_RECV_FIRST
   *   -> but now recv_msg is filled -> process
   *
   * We need to distinguish "metadata sent" from "message received".
   * Solution: use a flag.
   */
  if (sctx->recv_msg == NULL) {
    /* First call: metadata was sent, now issue recv */
    if (batch_op(sctx, &op, 1) != GRPC_CALL_OK) {
      stream_close(sctx, GRPC_STATUS_INTERNAL, "recv failed");
    }
    return;
  }

  /* Second call: recv_msg is now filled - process the subscription */
  Gnmi__SubscribeRequest *req = (Gnmi__SubscribeRequest *)gnmi_unpack(
    &gnmi__subscribe_request__descriptor, sctx->recv_msg);
  grpc_byte_buffer_destroy(sctx->recv_msg);
  sctx->recv_msg = NULL;

  if (!req) {
    stream_close(sctx, GRPC_STATUS_INVALID_ARGUMENT, "Failed to parse SubscribeRequest");
    return;
  }

  /* Validate */
  char *err_msg = NULL;
  grpc_status_code code = validate_subscribe(req, &err_msg);
  if (code != GRPC_STATUS_OK) {
    stream_close(sctx, code, err_msg);
    free(err_msg);
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);
    return;
  }

  /* Store original request for POLL re-fetch */
  sctx->orig_req = req;

  /* Determine mode */
  switch (req->subscribe->mode) {
  case GNMI__SUBSCRIPTION_LIST__MODE__ONCE: sctx->mode = SUB_MODE_ONCE;
    break;
  case GNMI__SUBSCRIPTION_LIST__MODE__POLL: sctx->mode = SUB_MODE_POLL;
    break;
  case GNMI__SUBSCRIPTION_LIST__MODE__STREAM: sctx->mode = SUB_MODE_STREAM;
    break;
  default: stream_close(sctx, GRPC_STATUS_INVALID_ARGUMENT, "Invalid subscription mode");
    return;
  }

  gnmi_log(GNMI_LOG_DEBUG, "Subscribe: mode=%d n_subs=%zu", sctx->mode, req->subscribe->n_subscription);

  /* Build initial data + sync for all modes.
   * When updates_only is set (ONCE/STREAM), skip the initial data
   * snapshot and send only the sync_response (gNMI 0.7.0 s3.5.1.5). */
  if (req->subscribe->updates_only) {
    Gnmi__SubscribeResponse sync = GNMI__SUBSCRIBE_RESPONSE__INIT;
    sync.response_case = GNMI__SUBSCRIBE_RESPONSE__RESPONSE_SYNC_RESPONSE;
    sync.sync_response = 1;
    if (send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&sync)) < 0) {
      stream_close(sctx, GRPC_STATUS_INTERNAL, "Failed to queue sync_response");
      return;
    }
  } else if (build_subscribe_data(sctx, req->subscribe) < 0) {
    stream_close(sctx, GRPC_STATUS_INTERNAL, "Failed to build subscription data");
    return;
  }

  /* Start sending the queued messages */
  sctx->send_queue_idx = 0;
  stream_send_next(sctx);
}

/* Send the next message from the queue */
static void stream_send_next(struct stream_ctx *sctx)
{
  if (sctx->send_queue_idx >= sctx->send_queue_len) {
    /* All messages sent */
    switch (sctx->mode) {
    case SUB_MODE_ONCE:
      /* ONCE: close the stream */
      gnmi_log(GNMI_LOG_DEBUG, "Subscribe ONCE: complete (%zu messages sent)", sctx->send_queue_len);
      stream_close(sctx, GRPC_STATUS_OK, NULL);
      return;
    case SUB_MODE_POLL:
      /* POLL: wait for next Poll message */
      gnmi_log(GNMI_LOG_DEBUG, "Subscribe POLL: waiting for next poll request");
      sctx->state = STREAM_RECV_POLL;
      {
        grpc_op op = {0};
        op.op = GRPC_OP_RECV_MESSAGE;
        op.data.recv_message.recv_message =
          &sctx->recv_msg;
        if (batch_op(sctx, &op, 1) != GRPC_CALL_OK)
          stream_close(sctx, GRPC_STATUS_INTERNAL, "recv failed");
      }
      return;
    case SUB_MODE_STREAM: if (sctx->state == STREAM_ACTIVE) {
        /* Already armed - just return to idle */
        return;
      }
      /* First time: arm timers and/or sysrepo subscriptions */
      stream_arm_subscriptions(sctx);
      return;
    default: stream_close(sctx, GRPC_STATUS_INTERNAL, "bad mode");
      return;
    }
  }

  /* Send the next message */
  sctx->state = STREAM_SENDING;
  grpc_op op = {0};
  op.op = GRPC_OP_SEND_MESSAGE;
  op.data.send_message.send_message =
    sctx->send_queue[sctx->send_queue_idx];

  if (batch_op(sctx, &op, 1) != GRPC_CALL_OK) {
    stream_close(sctx, GRPC_STATUS_INTERNAL, "send failed");
  }
}

/* CQ event: message sent -> advance to next */
static void step_send_msg(struct stream_ctx *sctx, bool success)
{
  if (!success) {
    /* Client disconnected or error */
    stream_free(sctx);
    return;
  }

  /* Free the sent buffer */
  if (sctx->send_queue_idx < sctx->send_queue_len) {
    grpc_byte_buffer_destroy( sctx->send_queue[sctx->send_queue_idx]);
    sctx->send_queue[sctx->send_queue_idx] = NULL;
  }
  sctx->send_queue_idx++;

  /* In STREAM mode, when all queued messages are sent:
   * - First time: arm timers/subscriptions
   * - Subsequent: return to idle STREAM_ACTIVE */
  if (sctx->mode == SUB_MODE_STREAM &&
      sctx->send_queue_idx >= sctx->send_queue_len) {
    bool first_time = (sctx->state != STREAM_ACTIVE && sctx->n_entries == 0);
    /* Clear the queue */
    for (size_t i = 0; i < sctx->send_queue_len; i++)
      if (sctx->send_queue[i])
        grpc_byte_buffer_destroy(sctx->send_queue[i]);
    free(sctx->send_queue);
    sctx->send_queue = NULL;
    sctx->send_queue_len = 0;
    sctx->send_queue_idx = 0;

    if (first_time) {
      /* First time: arm timers and sysrepo subscriptions */
      stream_arm_subscriptions(sctx);
    } else {
      /* Subsequent: return to idle */
      sctx->state = STREAM_ACTIVE;
    }
    return;
  }

  /* Send next or transition */
  stream_send_next(sctx);
}

/* CQ event: received Poll message */
static void step_recv_poll(struct stream_ctx *sctx, bool success)
{
  if (!success || !sctx->recv_msg) {
    /* Client closed (WritesDone) or error -> close stream */
    stream_close(sctx, GRPC_STATUS_OK, NULL);
    return;
  }

  /* Parse the Poll message */
  Gnmi__SubscribeRequest *req = (Gnmi__SubscribeRequest *)gnmi_unpack(
    &gnmi__subscribe_request__descriptor, sctx->recv_msg);
  grpc_byte_buffer_destroy(sctx->recv_msg);
  sctx->recv_msg = NULL;

  if (!req) {
    stream_close(sctx, GRPC_STATUS_INVALID_ARGUMENT, "Failed to parse Poll request");
    return;
  }

  /* Verify it's a Poll message */
  if (req->request_case != GNMI__SUBSCRIBE_REQUEST__REQUEST_POLL) {
    /* Could be aliases or another subscribe - reject */
    if (req->request_case ==
        GNMI__SUBSCRIBE_REQUEST__REQUEST_SUBSCRIBE) {
      stream_close(sctx, GRPC_STATUS_INVALID_ARGUMENT, "Duplicate subscription in POLL mode");
    } else {
      stream_close(sctx, GRPC_STATUS_INVALID_ARGUMENT, "Expected Poll message");
    }
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);
    return;
  }
  protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);

  /* Re-fetch data using the original subscription paths */
  /* Clear send queue from previous round */
  for (size_t i = sctx->send_queue_idx; i < sctx->send_queue_len; i++)
    if (sctx->send_queue[i])
      grpc_byte_buffer_destroy(sctx->send_queue[i]);
  free(sctx->send_queue);
  sctx->send_queue = NULL;
  sctx->send_queue_len = 0;
  sctx->send_queue_idx = 0;

  if (build_subscribe_data(sctx, sctx->orig_req->subscribe) < 0) {
    stream_close(sctx, GRPC_STATUS_INTERNAL, "Failed to re-fetch subscription data");
    return;
  }

  sctx->send_queue_idx = 0;
  stream_send_next(sctx);
}

/* - STREAM mode: arm timers and sysrepo subscriptions ------------- */

static void on_sample_timer(evutil_socket_t fd, short what, void *arg);
static void on_change_event(evutil_socket_t fd, short what, void *arg);
static int on_sr_change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
          sr_event_t event, uint32_t request_id,
          void *private_data);

static void ns_to_timeval(uint64_t ns, struct timeval *tv);
static void on_heartbeat_timer(evutil_socket_t fd, short what, void *arg);

static void stream_arm_subscriptions(struct stream_ctx *sctx)
{
  Gnmi__SubscriptionList *sl = sctx->orig_req->subscribe;
  struct event_base *evbase = gnmi_server_get_evbase(sctx->base.srv);
  sr_conn_ctx_t *sr_conn = gnmi_server_get_sr_conn(sctx->base.srv);

  sctx->allow_aggregation = sl->allow_aggregation;

  /* Create a session for STREAM mode data fetching (with per-stream NACM user) */
  const char *stream_user = sctx->session ? sctx->session->username : NULL;
  int rc = gnmi_nacm_session_start_as(sr_conn, SR_DS_OPERATIONAL,
    stream_user, &sctx->sr_sess);
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_ERROR, "Subscribe STREAM: sr_session_start failed: %s", sr_strerror(rc));
    stream_close(sctx, GRPC_STATUS_INTERNAL, "Failed to start sysrepo session");
    return;
  }

  sctx->n_entries = sl->n_subscription;
  sctx->entries = calloc(sctx->n_entries, sizeof(struct sub_entry));

  for (size_t i = 0; i < sl->n_subscription; i++) {
    Gnmi__Subscription *sub = sl->subscription[i];
    struct sub_entry *e = &sctx->entries[i];
    e->sctx = sctx;

    e->xpath = gnmi_merge_xpath(sl->prefix, sub->path, NULL);
    e->mode = sub->mode;
    e->sample_interval_ns = sub->sample_interval;
    e->suppress_redundant = sub->suppress_redundant;
    e->heartbeat_ns = sub->heartbeat_interval;

    if (sub->mode == GNMI__SUBSCRIPTION_MODE__SAMPLE &&
        e->sample_interval_ns > 0) {
      /* SAMPLE: create evtimer */
      e->ev_timer = evtimer_new(evbase, on_sample_timer, e);

      struct timeval tv;
      ns_to_timeval(e->sample_interval_ns, &tv);
      evtimer_add(e->ev_timer, &tv);

      gnmi_log(GNMI_LOG_DEBUG, "Subscribe STREAM: SAMPLE timer for %s "
         "(interval %lu ns, suppress_redundant=%d)",
         e->xpath, (unsigned long)e->sample_interval_ns,
         e->suppress_redundant);
    } else {
      /* ON_CHANGE (or TARGET_DEFINED -> treat as ON_CHANGE):
       * subscribe via sysrepo, wake main loop on changes */
      e->ev_change = event_new(evbase, -1, EV_PERSIST, on_change_event, e);

      /* Extract module name from xpath */
      const char *mod_start = e->xpath;
      if (mod_start[0] == '/')
        mod_start++;
      const char *colon = strchr(mod_start, ':');
      char *module = NULL;
      if (colon)
        module = strndup(mod_start, colon - mod_start);
      else
        module = strdup(mod_start);

      int rc = sr_module_change_subscribe( sctx->sr_sess, module, e->xpath, on_sr_change_cb, e,
        0, SR_SUBSCR_DEFAULT, &e->sr_sub);
      if (rc != SR_ERR_OK) {
        gnmi_log(GNMI_LOG_WARNING, "Subscribe ON_CHANGE failed for %s: %s",
           e->xpath, sr_strerror(rc));
      } else {
        gnmi_log(GNMI_LOG_DEBUG, "Subscribe STREAM: ON_CHANGE for %s "
           "(module=%s)", e->xpath, module);
      }
      free(module);

      /* Heartbeat timer for ON_CHANGE (gNMI 0.7.0 s3.5.1.5.3):
       * periodically send current value as a liveness signal. */
      if (e->heartbeat_ns > 0) {
        e->ev_heartbeat = evtimer_new(evbase, on_heartbeat_timer, e);
        struct timeval hb_tv;
        ns_to_timeval(e->heartbeat_ns, &hb_tv);
        evtimer_add(e->ev_heartbeat, &hb_tv);
        gnmi_log(GNMI_LOG_DEBUG, "Subscribe STREAM: heartbeat for %s "
           "(interval %lu ns)", e->xpath, (unsigned long)e->heartbeat_ns);
      }
    }
  }

  /* Enter STREAM_ACTIVE state.
   *
   * Client disconnect is detected when a send fails (success=false in
   * step_send_msg).  We cannot issue a concurrent RECV_MESSAGE because
   * gRPC CQ events carry the same tag for both SEND and RECV, and the
   * state machine cannot disambiguate which completed.
   *
   * Dead/slow clients are handled by gRPC server keepalive (configured
   * in server.c) which closes the transport after two missed pings. */
  sctx->state = STREAM_ACTIVE;

  gnmi_log(GNMI_LOG_INFO, "Subscribe STREAM: active with %zu entries", sctx->n_entries);
}

/* Client recv completed while in STREAM mode -> client disconnected */
static void step_stream_recv(struct stream_ctx *sctx, bool success)
{
  (void)success;
  sctx->recv_pending = false;
  /* Client sent WritesDone or cancelled - clean up */
  gnmi_log(GNMI_LOG_DEBUG, "Subscribe STREAM: client disconnected");
  stream_close(sctx, GRPC_STATUS_OK, NULL);
}

/* Build and queue a notification for a single subscription entry */
static void stream_queue_sample(struct stream_ctx *sctx, struct sub_entry *e)
{
  if (!sctx->sr_sess || sctx->state != STREAM_ACTIVE)
    return;

  sr_data_t *sr_data = NULL;
  int rc = sr_get_data(sctx->sr_sess, e->xpath, 0, 0, 0, &sr_data);
  if (rc != SR_ERR_OK || !sr_data || !sr_data->tree) {
    if (sr_data)
      sr_release_data(sr_data);
    return;
  }

  struct ly_set *set = NULL;
  lyd_find_xpath(sr_data->tree, e->xpath, &set);
  if (!set || set->count == 0) {
    if (set)
      ly_set_free(set, NULL);
    sr_release_data(sr_data);
    return;
  }

  /* suppress_redundant: serialize to JSON and compare with previous.
   * If identical, skip the notification (gNMI 0.7.0 s3.5.1.5.2). */
  if (e->suppress_redundant) {
    char *json = NULL;
    lyd_print_mem(&json, set->dnodes[0], LYD_JSON, LYD_PRINT_SHRINK);
    if (json && e->last_json && strcmp(json, e->last_json) == 0) {
      free(json);
      ly_set_free(set, NULL);
      sr_release_data(sr_data);
      return;
    }
    free(e->last_json);
    e->last_json = json;
  }

  /* Build notification with updates */
  Gnmi__Notification notif = GNMI__NOTIFICATION__INIT;
  notif.timestamp = get_time_nanosec();
  notif.n_update = set->count;
  notif.update = calloc(set->count, sizeof(Gnmi__Update *));

  for (uint32_t j = 0; j < set->count; j++) {
    Gnmi__Update *upd = calloc(1, sizeof(*upd));
    gnmi__update__init(upd);
    upd->path = calloc(1, sizeof(*upd->path));
    node_to_gnmi_path(set->dnodes[j], upd->path);
    upd->val = calloc(1, sizeof(*upd->val));
    encode_node(GNMI__ENCODING__JSON_IETF, set->dnodes[j], upd->val, NULL);
    notif.update[j] = upd;
  }

  /* allow_aggregation: accumulate updates without sending yet.
   * They'll be flushed as one Notification in stream_flush_aggregated(). */
  if (sctx->allow_aggregation) {
    Gnmi__Update **tmp = realloc(sctx->agg_updates,
      (sctx->agg_n_updates + set->count) * sizeof(Gnmi__Update *));
    if (tmp) {
      sctx->agg_updates = tmp;
      for (uint32_t j = 0; j < set->count; j++)
        sctx->agg_updates[sctx->agg_n_updates++] = notif.update[j];
      /* Ownership transferred -- don't free the updates */
      free(notif.update);
    } else {
      /* OOM -- fall through to non-aggregated send */
      goto send_now;
    }
    ly_set_free(set, NULL);
    sr_release_data(sr_data);
    return;
  }

send_now:;
  Gnmi__SubscribeResponse resp = GNMI__SUBSCRIBE_RESPONSE__INIT;
  resp.response_case = GNMI__SUBSCRIBE_RESPONSE__RESPONSE_UPDATE;
  resp.update = &notif;
  int qrc = send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&resp));

  /* Cleanup */
  for (uint32_t j = 0; j < set->count; j++)
    gnmi_update_free(notif.update[j]);
  free(notif.update);
  ly_set_free(set, NULL);
  sr_release_data(sr_data);

  if (qrc < 0)
    stream_close(sctx, GRPC_STATUS_RESOURCE_EXHAUSTED, "Send queue overflow");
}

/* Flush aggregated updates into one Notification and queue it. */
static void stream_flush_aggregated(struct stream_ctx *sctx)
{
  if (sctx->agg_n_updates == 0)
    return;

  Gnmi__Notification notif = GNMI__NOTIFICATION__INIT;
  notif.timestamp = get_time_nanosec();
  notif.n_update = sctx->agg_n_updates;
  notif.update = sctx->agg_updates;

  Gnmi__SubscribeResponse resp = GNMI__SUBSCRIBE_RESPONSE__INIT;
  resp.response_case = GNMI__SUBSCRIBE_RESPONSE__RESPONSE_UPDATE;
  resp.update = &notif;
  int qrc = send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&resp));

  /* Free updates (ownership was with aggregation buffer) */
  for (size_t i = 0; i < sctx->agg_n_updates; i++) {
    gnmi_update_free(sctx->agg_updates[i]);
  }
  free(sctx->agg_updates);
  sctx->agg_updates = NULL;
  sctx->agg_n_updates = 0;

  if (qrc < 0)
    stream_close(sctx, GRPC_STATUS_RESOURCE_EXHAUSTED, "Send queue overflow");
}

/* Try to send queued messages (called after timer/change enqueues) */
static void stream_try_send(struct stream_ctx *sctx)
{
  /* Flush any aggregated updates into the send queue first */
  if (sctx->allow_aggregation)
    stream_flush_aggregated(sctx);

  gnmi_log(GNMI_LOG_DEBUG, "stream_try_send: state=%d queue_len=%zu idx=%zu",
     sctx->state, sctx->send_queue_len, sctx->send_queue_idx);

  if (sctx->state != STREAM_ACTIVE)
    return; /* currently sending or closing */
  if (sctx->send_queue_len == 0)
    return; /* nothing to send */

  /* Start sending from current position */
  stream_send_next(sctx);
}

static void ns_to_timeval(uint64_t ns, struct timeval *tv)
{
  tv->tv_sec = ns / NSEC_PER_SEC;
  tv->tv_usec = (ns % NSEC_PER_SEC) / NSEC_PER_USEC;
}

/* Heartbeat timer callback for ON_CHANGE subscriptions (gNMI 0.7.0 s3.5.1.5.3).
 * Sends the current value periodically as a liveness signal even when nothing
 * changed.  Reset whenever a real ON_CHANGE notification is sent. */
static void on_heartbeat_timer(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  struct sub_entry *e = arg;
  struct stream_ctx *sctx = e->sctx;

  if (gnmi_server_is_shutting_down(sctx->base.srv))
    return;
  if (sctx->state != STREAM_ACTIVE)
    return;

  gnmi_log(GNMI_LOG_DEBUG, "Heartbeat timer fired for %s", e->xpath);

  /* Send current state as a full sample */
  stream_queue_sample(sctx, e);
  stream_try_send(sctx);

  /* Re-arm */
  struct timeval tv;
  ns_to_timeval(e->heartbeat_ns, &tv);
  evtimer_add(e->ev_heartbeat, &tv);
}

/* SAMPLE timer callback - fires on the main libevent loop */
static void on_sample_timer(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  struct sub_entry *e = arg;
  struct stream_ctx *sctx = e->sctx;

  /* Stop re-arming if the server is shutting down */
  if (gnmi_server_is_shutting_down(sctx->base.srv))
    return;

  gnmi_log(GNMI_LOG_DEBUG, "SAMPLE timer fired for %s (state=%d queue_len=%zu)",
     e->xpath, sctx->state, sctx->send_queue_len);

  if (sctx->state != STREAM_ACTIVE)
    return;

  /* Fetch data and queue notification */
  stream_queue_sample(sctx, e);

  gnmi_log(GNMI_LOG_DEBUG, "SAMPLE timer: queued, queue_len=%zu idx=%zu",
     sctx->send_queue_len, sctx->send_queue_idx);

  /* Re-arm the timer */
  struct timeval tv;
  ns_to_timeval(e->sample_interval_ns, &tv);
  evtimer_add(e->ev_timer, &tv);

  /* Try to send */
  stream_try_send(sctx);
}

/* sysrepo ON_CHANGE callback - fires on sysrepo's internal thread.
 * Must NOT access gRPC stream directly. Uses event_active to wake
 * the main libevent loop (thread-safe after evthread_use_pthreads). */
static int on_sr_change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
          sr_event_t event, uint32_t request_id,
          void *private_data)
{
  (void)sub_id; (void)module_name; (void)xpath; (void)request_id;
  struct sub_entry *e = private_data;

  if (event != SR_EV_DONE)
    return SR_ERR_OK;

  /* Stash the session for the main-loop callback to use.
   * Thread safety: this write is followed by event_active() which
   * internally acquires a libevent mutex, providing a full memory
   * barrier.  The main-loop reader (on_change_event) runs after the
   * event is activated, so the write is guaranteed visible. */
  e->change_sess = session;

  /* Wake the main event loop - thread-safe */
  if (e->ev_change)
    event_active(e->ev_change, EV_READ, 0);

  return SR_ERR_OK;
}

/* Build a delta notification from sr_get_change_tree_next.
 * Produces updates for CREATED/MODIFIED and deletes for DELETED. */
static void stream_queue_delta(struct stream_ctx *sctx, struct sub_entry *e)
{
  sr_session_ctx_t *sess = e->change_sess;
  if (!sess)
    return;

  sr_change_iter_t *iter = NULL;
  int rc = sr_get_changes_iter(sess, e->xpath, &iter);
  if (rc != SR_ERR_OK || !iter) {
    /* Fallback: re-fetch full data */
    stream_queue_sample(sctx, e);
    return;
  }

  Gnmi__Notification notif = GNMI__NOTIFICATION__INIT;
  notif.timestamp = get_time_nanosec();

  /* Collect updates and deletes */
  size_t n_updates = 0, n_deletes = 0;
  Gnmi__Update **updates = NULL;
  Gnmi__Path **deletes = NULL;

  sr_change_oper_t op;
  const struct lyd_node *node;
  const char *prev_val, *prev_list;
  int prev_dflt;

  while (sr_get_change_tree_next(sess, iter, &op, &node, &prev_val, &prev_list, &prev_dflt) == SR_ERR_OK) {
    switch (op) {
    case SR_OP_CREATED:
    case SR_OP_MODIFIED: {
      Gnmi__Update *upd = calloc(1, sizeof(*upd));
      gnmi__update__init(upd);
      upd->path = calloc(1, sizeof(*upd->path));
      node_to_gnmi_path(node, upd->path);
      upd->val = calloc(1, sizeof(*upd->val));
      encode_node(GNMI__ENCODING__JSON_IETF, node, upd->val, NULL);
      {
      Gnmi__Update **tmp = realloc(updates, (n_updates + 1) * sizeof(*updates));
      if (!tmp) { free(upd); break; }
      updates = tmp;
      updates[n_updates++] = upd;
      }
      break;
    }
    case SR_OP_DELETED: {
      Gnmi__Path *dp = calloc(1, sizeof(*dp));
      node_to_gnmi_path(node, dp);
      {
      Gnmi__Path **tmp = realloc(deletes, (n_deletes + 1) * sizeof(*deletes));
      if (!tmp) { free(dp); break; }
      deletes = tmp;
      deletes[n_deletes++] = dp;
      }
      break;
    }
    case SR_OP_MOVED:
      /* Treat as update */
      break;
    }
  }
  sr_free_change_iter(iter);

  /* Only send if there are actual changes */
  if (n_updates == 0 && n_deletes == 0)
    return;

  notif.n_update = n_updates;
  notif.update = updates;
  notif.n_delete_ = n_deletes;
  notif.delete_ = deletes;

  Gnmi__SubscribeResponse resp = GNMI__SUBSCRIBE_RESPONSE__INIT;
  resp.response_case = GNMI__SUBSCRIBE_RESPONSE__RESPONSE_UPDATE;
  resp.update = &notif;
  int qrc = send_queue_push(sctx, gnmi_pack((ProtobufCMessage *)&resp));

  /* Cleanup */
  for (size_t i = 0; i < n_updates; i++)
    gnmi_update_free(updates[i]);
  free(updates);

  for (size_t i = 0; i < n_deletes; i++) {
    if (deletes[i]) {
      gnmi_path_free_elems(deletes[i]);
      free(deletes[i]);
    }
  }
  free(deletes);

  if (qrc < 0)
    stream_close(sctx, GRPC_STATUS_RESOURCE_EXHAUSTED, "Send queue overflow");
}

/* ON_CHANGE event callback - fires on the main libevent loop */
static void on_change_event(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  struct sub_entry *e = arg;
  struct stream_ctx *sctx = e->sctx;

  if (sctx->state != STREAM_ACTIVE || !e->change_sess)
    return;

  /* Use delta-based notification via sr_get_changes_iter */
  stream_queue_delta(sctx, e);
  e->change_sess = NULL;

  /* Reset heartbeat timer on real change (gNMI 0.7.0 s3.5.1.5.3) */
  if (e->ev_heartbeat && e->heartbeat_ns > 0) {
    struct timeval hb_tv;
    ns_to_timeval(e->heartbeat_ns, &hb_tv);
    evtimer_add(e->ev_heartbeat, &hb_tv);
  }

  stream_try_send(sctx);
}

/* - Close stream -------------------------------------------------- */

static void stream_close(struct stream_ctx *sctx, grpc_status_code code, const char *msg)
{
  sctx->state = STREAM_CLOSING;
  sctx->close_code = code;

  grpc_op ops[2];
  memset(ops, 0, sizeof(ops));
  int n = 0;

  /* Send status */
  grpc_slice detail = msg ? grpc_slice_from_copied_string(msg) : grpc_empty_slice();
  ops[n].op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  ops[n].data.send_status_from_server.status = code;
  ops[n].data.send_status_from_server.status_details = &detail;
  ops[n].data.send_status_from_server.trailing_metadata_count = 0;
  n++;

  /* Recv close from client */
  ops[n].op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  ops[n].data.recv_close_on_server.cancelled = &sctx->close_cancelled;
  n++;

  if (batch_op(sctx, ops, n) != GRPC_CALL_OK) {
    grpc_slice_unref(detail);
    stream_free(sctx);
    return;
  }
  grpc_slice_unref(detail);
}

static void step_close_done(struct stream_ctx *sctx, bool success)
{
  (void)success;
  stream_free(sctx);
}

/* - Cleanup ------------------------------------------------------- */

static void stream_free(struct stream_ctx *sctx)
{
  /* Decrement session stream count */
  gnmi_session_stream_del(sctx->session);

  if (sctx->recv_msg)
    grpc_byte_buffer_destroy(sctx->recv_msg);
  if (sctx->send_msg)
    grpc_byte_buffer_destroy(sctx->send_msg);

  /* Free aggregated updates if any */
  for (size_t i = 0; i < sctx->agg_n_updates; i++) {
    gnmi_update_free(sctx->agg_updates[i]);
  }
  free(sctx->agg_updates);

  /* Free send queue */
  for (size_t i = 0; i < sctx->send_queue_len; i++)
    if (sctx->send_queue[i])
      grpc_byte_buffer_destroy(sctx->send_queue[i]);
  free(sctx->send_queue);

  /* Free STREAM mode entries */
  for (size_t i = 0; i < sctx->n_entries; i++) {
    struct sub_entry *e = &sctx->entries[i];
    if (e->ev_timer) {
      evtimer_del(e->ev_timer);
      event_free(e->ev_timer);
    }
    if (e->sr_sub)
      sr_unsubscribe(e->sr_sub);
    if (e->ev_change) {
      event_del(e->ev_change);
      event_free(e->ev_change);
    }
    if (e->ev_heartbeat) {
      evtimer_del(e->ev_heartbeat);
      event_free(e->ev_heartbeat);
    }
    free(e->last_json);
    free(e->xpath);
  }
  free(sctx->entries);

  if (sctx->sr_sess)
    sr_session_stop(sctx->sr_sess);

  if (sctx->orig_req)
    protobuf_c_message_free_unpacked( (ProtobufCMessage *)sctx->orig_req, NULL);
  free(sctx->close_msg);
  free(sctx->stream_user);

  grpc_metadata_array_destroy(&sctx->base.md_recv);
  if (sctx->base.call)
    grpc_call_unref(sctx->base.call);
  free(sctx);
}

/* - Arm: accept next Subscribe call ------------------------------- */

void subscribe_arm(gnmi_server_t *srv, int method_idx)
{
  if (gnmi_server_is_shutting_down(srv))
    return;

  struct stream_ctx *sctx = calloc(1, sizeof(*sctx));
  if (!sctx)
    return;

  sctx->base.srv = srv;
  sctx->base.step = stream_step;
  sctx->method_idx = method_idx;
  sctx->state = STREAM_INIT;
  grpc_metadata_array_init(&sctx->base.md_recv);

  void *handle = gnmi_service_get_method_handle(method_idx);
  if (!handle) {
    gnmi_log(GNMI_LOG_ERROR, "Subscribe: no method handle");
    free(sctx);
    return;
  }

  grpc_call_error err = grpc_server_request_registered_call( gnmi_server_get_grpc(srv), handle,
    &sctx->base.call, &sctx->base.deadline,
    &sctx->base.md_recv, NULL, /* no initial payload for streaming */
    gnmi_server_get_cq(srv), gnmi_server_get_cq(srv),
    &sctx->base);

  if (err != GRPC_CALL_OK) {
    gnmi_log(GNMI_LOG_ERROR, "Subscribe request_registered_call failed: %d", err);
    free(sctx);
  }
}

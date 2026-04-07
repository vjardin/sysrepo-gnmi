/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#include "gnmi_service.h"
#include "server.h"
#include "capabilities.h"
#include "get.h"
#include "set.h"
#include "subscribe.h"
#include "rpc.h"
#include "confirm.h"
#include "session.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <grpc/grpc.h>
#include <grpc/byte_buffer_reader.h>
#include <grpc/support/alloc.h>

/* - protobuf-c <-> grpc_byte_buffer bridge ------------------------ */

grpc_byte_buffer *gnmi_pack(const ProtobufCMessage *msg)
{
  size_t len = protobuf_c_message_get_packed_size(msg);
  grpc_slice slice = grpc_slice_malloc(len);
  protobuf_c_message_pack(msg, GRPC_SLICE_START_PTR(slice));
  grpc_byte_buffer *bb = grpc_raw_byte_buffer_create(&slice, 1);
  grpc_slice_unref(slice);
  return bb;
}

ProtobufCMessage *gnmi_unpack(const ProtobufCMessageDescriptor *desc, grpc_byte_buffer *bb)
{
  if (!bb)
    return NULL;

  grpc_byte_buffer_reader reader;
  if (!grpc_byte_buffer_reader_init(&reader, bb))
    return NULL;

  grpc_slice slice = grpc_byte_buffer_reader_readall(&reader);
  ProtobufCMessage *msg = protobuf_c_message_unpack( desc, NULL,
    GRPC_SLICE_LENGTH(slice), GRPC_SLICE_START_PTR(slice));
  grpc_slice_unref(slice);
  grpc_byte_buffer_reader_destroy(&reader);
  return msg;
}

/* - RPC method table ---------------------------------------------- */

typedef grpc_status_code (*unary_handler_fn)(sr_conn_ctx_t *sr_conn,
               const struct gnmi_session *session,
               grpc_byte_buffer *req, grpc_byte_buffer **resp, char **status_msg);

struct rpc_method {
  const char       *path;
  void             *handle;    /* from grpc_server_register_method */
  bool              streaming; /* bidi streaming (Subscribe) */
  unary_handler_fn  handler;   /* unary RPCs only */
};

static struct rpc_method methods[] = {
  { "/gnmi.gNMI/Capabilities", NULL, false, handle_capabilities },
  { "/gnmi.gNMI/Get",          NULL, false, handle_get },
  { "/gnmi.gNMI/Set",          NULL, false, handle_set },
  { "/gnmi.gNMI/Subscribe",    NULL, true,  NULL },
  { "/gnmi.gNMI/Confirm",      NULL, false, handle_confirm },
  { "/gnmi.gNMI/Rpc",          NULL, false, handle_rpc },
};

#define N_METHODS (sizeof(methods) / sizeof(methods[0]))

static const char *extract_metadata_username(const grpc_metadata_array *md);

/* - Unary RPC state machine --------------------------------------- */

static void step_got_call(struct call_ctx *base, bool success);
static void step_done(struct call_ctx *base, bool success);

/*
 * Unary RPC flow through the CQ:
 *
 * 1. grpc_server_request_registered_call() -> CQ event -> step_got_call()
 * 2. Dispatch handler (pack/unpack)
 * 3. grpc_call_start_batch(SEND_INITIAL_METADATA + SEND_MESSAGE +
 *    SEND_STATUS + RECV_CLOSE) -> CQ event -> step_done()
 * 4. Free call_ctx, re-arm for next call
 */

struct unary_ctx {
  struct call_ctx      base;
  int                  method_idx;
  grpc_byte_buffer    *response_bb; /* freed in step_done */
};

static void step_got_call(struct call_ctx *base, bool success)
{
  struct unary_ctx *ctx = (struct unary_ctx *)base;

  if (!success || gnmi_server_is_shutting_down(base->srv)) {
    /* Server shutting down or cancelled */
    grpc_metadata_array_destroy(&base->md_recv);
    if (base->request_payload)
      grpc_byte_buffer_destroy(base->request_payload);
    if (base->call)
      grpc_call_unref(base->call);
    free(ctx);
    return;
  }

  /* Re-arm to accept the next call on this method immediately */
  gnmi_service_rearm(base->srv, ctx->method_idx);

  /* Dispatch the handler */
  grpc_byte_buffer *response_bb = NULL;
  char *status_msg = NULL;
  grpc_status_code code;

  struct rpc_method *m = &methods[ctx->method_idx];
  gnmi_log(GNMI_LOG_DEBUG, "RPC %s received", m->path);

  /* Extract per-RPC username from client metadata (gnmic --username) */
  const char *rpc_user = extract_metadata_username(&base->md_recv);
  if (rpc_user)
    gnmi_log(GNMI_LOG_DEBUG, "RPC metadata username: %s", rpc_user);

  /* Track session */
  char *peer = grpc_call_get_peer(base->call);
  struct gnmi_session *session = NULL;
  if (peer) {
    session = gnmi_session_get(
        gnmi_server_get_sessions(base->srv), peer, rpc_user);
    if (session)
      gnmi_session_touch(session);
    gpr_free(peer);
  }

  if (m->handler) {
    code = m->handler(gnmi_server_get_sr_conn(base->srv), session, base->request_payload, &response_bb, &status_msg);
  } else {
    code = GRPC_STATUS_UNIMPLEMENTED;
    status_msg = strdup("Not implemented");
  }

  if (session && code != GRPC_STATUS_OK)
    gnmi_session_record_error(session);

  gnmi_log(GNMI_LOG_DEBUG, "RPC %s completed (status=%d%s%s)",
           m->path, code,
           status_msg ? " msg=" : "",
           status_msg ? status_msg : "");

  /* Build batch: send metadata + message + status, recv close */
  grpc_op ops[4];
  memset(ops, 0, sizeof(ops));
  int n = 0;

  /* Send initial metadata */
  ops[n].op = GRPC_OP_SEND_INITIAL_METADATA;
  ops[n].data.send_initial_metadata.count = 0;
  n++;

  /* Send response message (if any) */
  if (response_bb) {
    ops[n].op = GRPC_OP_SEND_MESSAGE;
    ops[n].data.send_message.send_message = response_bb;
    n++;
  }

  /* Send status */
  grpc_slice detail = status_msg ? grpc_slice_from_copied_string(status_msg) : grpc_empty_slice();
  ops[n].op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  ops[n].data.send_status_from_server.status = code;
  ops[n].data.send_status_from_server.status_details = &detail;
  ops[n].data.send_status_from_server.trailing_metadata_count = 0;
  n++;

  /* Recv close from client */
  ops[n].op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  ops[n].data.recv_close_on_server.cancelled = &base->cancelled;
  n++;

  base->step = step_done;

  grpc_call_error err = grpc_call_start_batch( base->call, ops, n, base, NULL);

  grpc_slice_unref(detail);
  free(status_msg);

  if (err != GRPC_CALL_OK) {
    gnmi_log(GNMI_LOG_ERROR, "start_batch failed: %d", err);
    if (response_bb)
      grpc_byte_buffer_destroy(response_bb);
    step_done(base, false);
    return;
  }

  /* Store for cleanup after send completes */
  ctx->response_bb = response_bb;
}

static void step_done(struct call_ctx *base, bool success)
{
  (void)success;
  struct unary_ctx *ctx = (struct unary_ctx *)base;
  if (ctx->response_bb)
    grpc_byte_buffer_destroy(ctx->response_bb);
  grpc_metadata_array_destroy(&base->md_recv);
  if (base->request_payload)
    grpc_byte_buffer_destroy(base->request_payload);
  if (base->call)
    grpc_call_unref(base->call);
  free(base);
}

/* - Re-arm: request next call for a given method ------------------ */

void gnmi_service_rearm(gnmi_server_t *srv, int method_idx)
{
  if (gnmi_server_is_shutting_down(srv))
    return;

  struct rpc_method *m = &methods[method_idx];

  if (m->streaming) {
    subscribe_arm(srv, method_idx);
    return;
  }

  struct unary_ctx *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return;

  ctx->base.srv = srv;
  ctx->base.step = step_got_call;
  ctx->method_idx = method_idx;
  grpc_metadata_array_init(&ctx->base.md_recv);

  grpc_call_error err = grpc_server_request_registered_call( gnmi_server_get_grpc(srv), m->handle,
    &ctx->base.call, &ctx->base.deadline,
    &ctx->base.md_recv, &ctx->base.request_payload,
    gnmi_server_get_cq(srv), gnmi_server_get_cq(srv),
    &ctx->base);

  if (err != GRPC_CALL_OK) {
    gnmi_log(GNMI_LOG_ERROR, "request_registered_call(%s) failed: %d", m->path, err);
    free(ctx);
  }
}

/* Global NACM user, set once at init from server config */
static const char *nacm_user;

/* Extract "username" from gRPC client metadata (sent by gnmic --username). */
static const char *extract_metadata_username(const grpc_metadata_array *md)
{
  for (size_t i = 0; i < md->count; i++) {
    grpc_slice key = md->metadata[i].key;
    if (grpc_slice_eq(key, grpc_slice_from_static_string("username")))
      return (const char *)GRPC_SLICE_START_PTR(md->metadata[i].value);
  }
  return NULL;
}

/* - Init: register methods and arm -------------------------------- */

int gnmi_service_init(gnmi_server_t *srv)
{
  grpc_server *gs = gnmi_server_get_grpc(srv);

  for (int i = 0; i < (int)N_METHODS; i++) {
    grpc_server_register_method_payload_handling payload =
      methods[i].streaming ?
      GRPC_SRM_PAYLOAD_NONE :
      GRPC_SRM_PAYLOAD_READ_INITIAL_BYTE_BUFFER;
    methods[i].handle = grpc_server_register_method( gs, methods[i].path, NULL, payload, 0);
    if (!methods[i].handle) {
      gnmi_log(GNMI_LOG_FATAL, "Failed to register method %s", methods[i].path);
      return -1;
    }
    gnmi_log(GNMI_LOG_DEBUG, "Registered method: %s", methods[i].path);
  }

  /* Store NACM user for session creation in RPC handlers */
  nacm_user = gnmi_server_get_nacm_user(srv);

  /* Must arm AFTER grpc_server_start, so we return and let
   * server.c call start, then arm. We'll arm in a post-start hook. */
  return 0;
}

/* Start a sysrepo session with NACM user if configured */

int gnmi_session_start(gnmi_server_t *srv, sr_datastore_t ds, sr_session_ctx_t **sess)
{
  return gnmi_nacm_session_start_as(gnmi_server_get_sr_conn(srv), ds, NULL, sess);
}

int gnmi_nacm_session_start(sr_conn_ctx_t *conn, sr_datastore_t ds, sr_session_ctx_t **sess)
{
  return gnmi_nacm_session_start_as(conn, ds, NULL, sess);
}

int gnmi_nacm_session_start_as(sr_conn_ctx_t *conn, sr_datastore_t ds,
    const char *user, sr_session_ctx_t **sess)
{
  int rc = sr_session_start(conn, ds, sess);
  if (rc != SR_ERR_OK)
    return rc;

  /* Explicit user takes precedence, then -u flag */
  const char *effective = (user && user[0]) ? user : nacm_user;
  if (effective && effective[0])
    sr_session_set_user(*sess, effective);

  return SR_ERR_OK;
}

/* Get the registered method handle by index */
void *gnmi_service_get_method_handle(int idx)
{
  if (idx < 0 || idx >= (int)N_METHODS)
    return NULL;
  return methods[idx].handle;
}

/* Called by server.c after grpc_server_start() */
void gnmi_service_arm_all(gnmi_server_t *srv)
{
  for (int i = 0; i < (int)N_METHODS; i++)
    gnmi_service_rearm(srv, i);
}

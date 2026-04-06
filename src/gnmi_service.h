/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdbool.h>

#include <grpc/grpc.h>
#include <grpc/byte_buffer.h>
#include <protobuf-c/protobuf-c.h>
#include <sysrepo.h>

/* Forward declaration */
typedef struct gnmi_server gnmi_server_t;

/*
 * Per-call context - allocated for each incoming RPC.
 * The `step` function pointer is the CQ state machine callback:
 * each CQ event invokes it, and it transitions to the next state.
 */
struct call_ctx {
  gnmi_server_t        *srv;
  grpc_call            *call;
  void                 *registered_method;
  grpc_metadata_array   md_recv;
  grpc_byte_buffer     *request_payload;
  gpr_timespec          deadline;
  int                   cancelled;

  /* State machine: called on each CQ event */
  void (*step)(struct call_ctx *ctx, bool success);

  /* Linked list for active streams (subscribe) */
  struct call_ctx      *next;
};

/* Pack a protobuf-c message into a grpc_byte_buffer */
grpc_byte_buffer *gnmi_pack(const ProtobufCMessage *msg);

/* Unpack a grpc_byte_buffer into a protobuf-c message */
ProtobufCMessage *gnmi_unpack(const ProtobufCMessageDescriptor *desc, grpc_byte_buffer *bb);

/* Register all gNMI RPC methods and start accepting calls */
int gnmi_service_init(gnmi_server_t *srv);

/* Start a sysrepo session with NACM user set (if configured).
 * Caller must sr_session_stop() the session. */
int gnmi_session_start(gnmi_server_t *srv, sr_datastore_t ds, sr_session_ctx_t **sess);

/* Start a sysrepo session with NACM user set, from a connection.
 * For use in RPC handlers that receive sr_conn_ctx_t (not gnmi_server_t). */
int gnmi_nacm_session_start(sr_conn_ctx_t *conn, sr_datastore_t ds, sr_session_ctx_t **sess);

/* Drain: re-arm request_registered_call for a given method index */
void gnmi_service_rearm(gnmi_server_t *srv, int method_idx);

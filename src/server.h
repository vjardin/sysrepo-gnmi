/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdbool.h>
#include <sysrepo.h>
#include <grpc/grpc.h>

struct event_base;  /* forward */
typedef struct gnmi_session_registry gnmi_session_registry_t;

typedef struct gnmi_server gnmi_server_t;

struct gnmi_config {
  const char *bind_addr;
  const char *tls_key;
  const char *tls_cert;
  const char *tls_ca;
  const char *username;
  const char *password;
  int         log_level;
  bool        insecure;
  int         max_sessions;           /* 0 = unlimited */
  int         max_streams_per_session; /* 0 = unlimited */
  const char *yang_dir;               /* directory for server YANG modules */
};

struct stream_ctx;  /* forward */

gnmi_server_t *gnmi_server_create(const struct gnmi_config *cfg, sr_conn_ctx_t *sr_conn);
int  gnmi_server_run(gnmi_server_t *srv);
void gnmi_server_shutdown(gnmi_server_t *srv);
void gnmi_server_destroy(gnmi_server_t *srv);

/* Accessors for internal server state (used by gnmi_service/subscribe) */
grpc_server            *gnmi_server_get_grpc(gnmi_server_t *srv);
grpc_completion_queue  *gnmi_server_get_cq(gnmi_server_t *srv);
sr_conn_ctx_t          *gnmi_server_get_sr_conn(gnmi_server_t *srv);
bool                    gnmi_server_is_shutting_down(gnmi_server_t *srv);
struct event_base      *gnmi_server_get_evbase(gnmi_server_t *srv);
const char             *gnmi_server_get_nacm_user(gnmi_server_t *srv);
gnmi_session_registry_t *gnmi_server_get_sessions(gnmi_server_t *srv);

/* Server uptime in seconds */
uint64_t gnmi_server_uptime(gnmi_server_t *srv);

/* Active stream tracking for graceful shutdown */
void gnmi_server_stream_register(gnmi_server_t *srv, struct stream_ctx *sctx);
void gnmi_server_stream_unregister(gnmi_server_t *srv, struct stream_ctx *sctx);

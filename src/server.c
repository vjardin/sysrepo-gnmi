/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#include "server.h"
#include "gnmi_service.h"
#include "confirm.h"
#include "log.h"

#include <stdlib.h>
#include <signal.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <event2/event.h>
#include <event2/thread.h>

struct gnmi_server {
  struct event_base      *evbase;
  struct event           *ev_sigterm;
  struct event           *ev_sigint;
  struct event           *ev_cq_poll;
  grpc_server            *grpc_srv;
  grpc_completion_queue  *cq;
  sr_conn_ctx_t          *sr_conn;
  const char             *nacm_user;  /* NACM user for sessions, or NULL */
  bool                    shutting_down;
};

/* - CQ polling callback (evtimer, fires every ~2ms) --------------- */

static void on_cq_poll_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  gnmi_server_t *srv = arg;
  gpr_timespec deadline = gpr_time_0(GPR_CLOCK_MONOTONIC);
  grpc_event ev;

  for (;;) {
    ev = grpc_completion_queue_next(srv->cq, deadline, NULL);
    if (ev.type == GRPC_QUEUE_TIMEOUT)
      break;
    if (ev.type == GRPC_QUEUE_SHUTDOWN) {
      event_base_loopbreak(srv->evbase);
      return;
    }
    if (ev.type == GRPC_OP_COMPLETE && ev.tag) {
      struct call_ctx *ctx = ev.tag;
      ctx->step(ctx, ev.success);
    }
  }
}

/* - Signal handling ----------------------------------------------- */

static void on_signal_cb(evutil_socket_t sig, short what, void *arg)
{
  (void)what;
  gnmi_server_t *srv = arg;

  if (!srv->shutting_down) {
    gnmi_log(GNMI_LOG_INFO, "Received signal %d, shutting down", (int)sig);
    gnmi_server_shutdown(srv);
  } else {
    gnmi_log(GNMI_LOG_WARNING, "Forced exit on second signal");
    exit(EXIT_FAILURE);
  }
}

/* - Public API ---------------------------------------------------- */

gnmi_server_t *gnmi_server_create(const struct gnmi_config *cfg, sr_conn_ctx_t *sr_conn)
{
  gnmi_server_t *srv = calloc(1, sizeof(*srv));
  if (!srv)
    return NULL;

  srv->sr_conn = sr_conn;
  srv->nacm_user = cfg->username;  /* NULL if not set */

  /* Initialize libevent with pthread support (for event_active) */
  evthread_use_pthreads();
  srv->evbase = event_base_new();
  if (!srv->evbase) {
    gnmi_log(GNMI_LOG_FATAL, "event_base_new() failed");
    goto err;
  }

  /* Signal handlers */
  srv->ev_sigterm = evsignal_new(srv->evbase, SIGTERM, on_signal_cb, srv);
  srv->ev_sigint = evsignal_new(srv->evbase, SIGINT, on_signal_cb, srv);
  evsignal_add(srv->ev_sigterm, NULL);
  evsignal_add(srv->ev_sigint, NULL);

  /* Initialize gRPC */
  grpc_init();

  srv->cq = grpc_completion_queue_create_for_next(NULL);

#define GNMI_DEFAULT_MAX_MSG_BYTES  (64 * 1024 * 1024)  /* 64 MB */

  /* Max message size from env (default 64 MB) */
  const char *max_msg_env = getenv("GNMI_MAX_MSG_SIZE_KB");
  int max_msg_bytes = max_msg_env ? atoi(max_msg_env) * 1024
          : GNMI_DEFAULT_MAX_MSG_BYTES;
  grpc_arg args_arr[] = {
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH,
      .value.integer = max_msg_bytes },
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_MAX_SEND_MESSAGE_LENGTH,
      .value.integer = max_msg_bytes },
  };
  grpc_channel_args ch_args = {
    .num_args = 2,
    .args = args_arr,
  };
  srv->grpc_srv = grpc_server_create(&ch_args, NULL);
  grpc_server_register_completion_queue(srv->grpc_srv, srv->cq, NULL);

  /* Bind address */
  grpc_server_credentials *creds;
  if (cfg->insecure) {
    creds = grpc_insecure_server_credentials_create();
  } else {
    /* TODO Phase 7: TLS credentials */
    gnmi_log(GNMI_LOG_FATAL, "TLS not yet implemented, use -f");
    goto err;
  }

  int port = grpc_server_add_http2_port(srv->grpc_srv, cfg->bind_addr, creds);
  grpc_server_credentials_release(creds);
  if (port == 0) {
    gnmi_log(GNMI_LOG_FATAL, "Failed to bind to %s", cfg->bind_addr);
    goto err;
  }
  gnmi_log(GNMI_LOG_INFO, "Listening on %s (port %d)", cfg->bind_addr, port);

  /* Register RPC methods and start accepting calls */
  if (gnmi_service_init(srv) != 0)
    goto err;

  grpc_server_start(srv->grpc_srv);

  /* Arm all RPC methods to accept calls (must be after start) */
  extern void gnmi_service_arm_all(gnmi_server_t *srv);
  gnmi_service_arm_all(srv);

  /* Confirmed-commit state machine (timer on the shared event_base) */
  confirm_state_t *cs = confirm_state_create(srv->evbase, sr_conn);
  if (cs)
    confirm_state_set_global(cs);

#define CQ_POLL_INTERVAL_US  2000  /* microseconds */

  /* CQ poll timer */
  struct timeval tv = { .tv_sec = 0, .tv_usec = CQ_POLL_INTERVAL_US };
  srv->ev_cq_poll = event_new(srv->evbase, -1, EV_PERSIST, on_cq_poll_cb, srv);
  evtimer_add(srv->ev_cq_poll, &tv);

  return srv;

err:
  gnmi_server_destroy(srv);
  return NULL;
}

int gnmi_server_run(gnmi_server_t *srv)
{
  gnmi_log(GNMI_LOG_INFO, "Server running, waiting for connections");
  return event_base_dispatch(srv->evbase);
}

void gnmi_server_shutdown(gnmi_server_t *srv)
{
  if (srv->shutting_down)
    return;
  srv->shutting_down = true;

  gnmi_log(GNMI_LOG_INFO, "Initiating graceful shutdown");

  /* Tell gRPC to stop accepting new calls */
  grpc_server_shutdown_and_notify(srv->grpc_srv, srv->cq, NULL);

  /* The CQ will eventually produce GRPC_QUEUE_SHUTDOWN,
   * which triggers event_base_loopbreak in on_cq_poll_cb */
}

void gnmi_server_destroy(gnmi_server_t *srv)
{
  if (!srv)
    return;

  if (srv->ev_cq_poll) {
    event_del(srv->ev_cq_poll);
    event_free(srv->ev_cq_poll);
  }
  if (srv->ev_sigterm) {
    event_del(srv->ev_sigterm);
    event_free(srv->ev_sigterm);
  }
  if (srv->ev_sigint) {
    event_del(srv->ev_sigint);
    event_free(srv->ev_sigint);
  }

  /* Destroy confirmed-commit state */
  confirm_state_t *cs = confirm_state_get_global();
  if (cs) {
    confirm_state_destroy(cs);
    confirm_state_set_global(NULL);
  }

  if (srv->grpc_srv) {
    grpc_server_destroy(srv->grpc_srv);
  }
  if (srv->cq) {
    grpc_completion_queue_destroy(srv->cq);
  }

  grpc_shutdown();

  if (srv->evbase)
    event_base_free(srv->evbase);

  free(srv);
}

/* - Accessors for gnmi_service.c ---------------------------------- */

grpc_server *gnmi_server_get_grpc(gnmi_server_t *srv)
{
  return srv->grpc_srv;
}

grpc_completion_queue *gnmi_server_get_cq(gnmi_server_t *srv)
{
  return srv->cq;
}

sr_conn_ctx_t *gnmi_server_get_sr_conn(gnmi_server_t *srv)
{
  return srv->sr_conn;
}

bool gnmi_server_is_shutting_down(gnmi_server_t *srv)
{
  return srv->shutting_down;
}

struct event_base *gnmi_server_get_evbase(gnmi_server_t *srv)
{
  return srv->evbase;
}

const char *gnmi_server_get_nacm_user(gnmi_server_t *srv)
{
  return srv->nacm_user;
}

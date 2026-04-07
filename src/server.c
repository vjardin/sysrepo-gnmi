/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#include "server.h"
#include "gnmi_service.h"
#include "subscribe.h"
#include "confirm.h"
#include "session.h"
#include "monitoring.h"
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
  int                     idle_polls;  /* consecutive empty CQ polls */
  gnmi_session_registry_t *sessions;
  struct event           *ev_session_reap;  /* periodic idle session reaper */
  struct stream_ctx      *active_streams;   /* linked list of active subscribe streams */
  struct event           *ev_drain;         /* drain timer for graceful shutdown */
  struct timespec         start_time;       /* for uptime calculation */
};

/* Adaptive CQ polling */

/*
 * TODO: replace timer-based polling with gRPC callback CQ
 * (GRPC_CQ_CALLBACK + grpc_completion_queue_create_for_callback).
 *
 * Callback CQ would let gRPC invoke a functor directly on each CQ
 * event, which we could relay to the main libevent loop via
 * event_active() -- eliminating the poll timer entirely.
 *
 * This was attempted but does not work with the pure-C gRPC API:
 * the GRPC_CQ_CALLBACK completion type requires gRPC's internal
 * thread pool to drive I/O on the CQ's pollset, but that thread
 * pool is only managed by the C++ grpc::Server runtime. In pure C,
 * with both GRPC_CQ_DEFAULT_POLLING and GRPC_CQ_NON_POLLING, the
 * functor_run callback fires only during shutdown -- never for
 * active RPCs -- because no internal thread is polling.
 *
 * Revisit if gRPC C core adds a stable mechanism to drive I/O
 * independently of grpc_completion_queue_next(), or if this project
 * ever links against the C++ runtime.
 */

#define CQ_POLL_FAST_US    2000   /* 2ms when active */
#define CQ_POLL_SLOW_US   50000   /* 50ms when idle */
#define CQ_IDLE_THRESHOLD      5  /* consecutive empty polls before slowing */

static void on_cq_poll_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  gnmi_server_t *srv = arg;
  gpr_timespec deadline = gpr_time_0(GPR_CLOCK_MONOTONIC);
  grpc_event ev;
  bool got_event = false;

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
      got_event = true;
    }
  }

  /* Adaptive backoff: fast poll when busy, slow when idle */
  if (got_event) {
    srv->idle_polls = 0;
  } else if (srv->idle_polls < CQ_IDLE_THRESHOLD) {
    srv->idle_polls++;
  }

  struct timeval tv;
  if (srv->idle_polls < CQ_IDLE_THRESHOLD) {
    tv = (struct timeval){ .tv_sec = 0, .tv_usec = CQ_POLL_FAST_US };
  } else {
    tv = (struct timeval){ .tv_sec = 0, .tv_usec = CQ_POLL_SLOW_US };
  }
  evtimer_add(srv->ev_cq_poll, &tv);
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

/* - Active stream tracking ---------------------------------------- */

void gnmi_server_stream_register(gnmi_server_t *srv, struct stream_ctx *sctx)
{
  sctx->next_stream = srv->active_streams;
  srv->active_streams = sctx;
}

void gnmi_server_stream_unregister(gnmi_server_t *srv, struct stream_ctx *sctx)
{
  struct stream_ctx **pp = &srv->active_streams;
  while (*pp) {
    if (*pp == sctx) {
      *pp = sctx->next_stream;
      sctx->next_stream = NULL;
      return;
    }
    pp = &(*pp)->next_stream;
  }
}

/* - Drain timer for graceful shutdown ----------------------------- */

#define DRAIN_TIMEOUT_SEC  3

static void on_drain_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  gnmi_server_t *srv = arg;
  gnmi_log(GNMI_LOG_INFO, "Drain timeout, breaking event loop");
  event_base_loopbreak(srv->evbase);
}

/* - Session idle reaper ------------------------------------------- */

#define SESSION_REAP_INTERVAL_SEC  30   /* check every 30s */
#define SESSION_IDLE_TIMEOUT_SEC   60   /* reap after 60s idle (no active streams) */

static void on_session_reap_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what;
  gnmi_server_t *srv = arg;
  gnmi_session_reap_idle(srv->sessions, SESSION_IDLE_TIMEOUT_SEC);
  /* Re-arm */
  struct timeval tv = { .tv_sec = SESSION_REAP_INTERVAL_SEC };
  evtimer_add(srv->ev_session_reap, &tv);
}

/* - Public API ---------------------------------------------------- */

gnmi_server_t *gnmi_server_create(const struct gnmi_config *cfg, sr_conn_ctx_t *sr_conn)
{
  gnmi_server_t *srv = calloc(1, sizeof(*srv));
  if (!srv)
    return NULL;

  srv->sr_conn = sr_conn;
  srv->nacm_user = cfg->username;  /* NULL if not set */
  clock_gettime(CLOCK_MONOTONIC, &srv->start_time);

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

#define GNMI_KEEPALIVE_TIME_MS      30000  /* send ping every 30s */
#define GNMI_KEEPALIVE_TIMEOUT_MS   10000  /* close after 10s with no pong */

  grpc_arg args_arr[] = {
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH,
      .value.integer = max_msg_bytes },
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_MAX_SEND_MESSAGE_LENGTH,
      .value.integer = max_msg_bytes },
    /* Keepalive: detect dead/unresponsive clients and reclaim resources */
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_KEEPALIVE_TIME_MS,
      .value.integer = GNMI_KEEPALIVE_TIME_MS },
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
      .value.integer = GNMI_KEEPALIVE_TIMEOUT_MS },
    { .type = GRPC_ARG_INTEGER,
      .key = GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
      .value.integer = 1 },
  };
  grpc_channel_args ch_args = {
    .num_args = sizeof(args_arr) / sizeof(args_arr[0]),
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
  gnmi_service_arm_all(srv);

  /* Confirmed-commit state machine (timer on the shared event_base) */
  confirm_state_t *cs = confirm_state_create(srv->evbase, sr_conn);
  if (cs)
    confirm_state_set_global(cs);

  /* CQ poll timer (one-shot, re-armed adaptively in callback) */
  struct timeval tv = { .tv_sec = 0, .tv_usec = CQ_POLL_FAST_US };
  srv->ev_cq_poll = evtimer_new(srv->evbase, on_cq_poll_cb, srv);
  evtimer_add(srv->ev_cq_poll, &tv);

  /* Session registry + idle reaper */
  srv->sessions = gnmi_session_registry_create(srv->evbase,
      cfg->max_sessions, cfg->max_streams_per_session);
  srv->ev_session_reap = evtimer_new(srv->evbase, on_session_reap_cb, srv);
  struct timeval reap_tv = { .tv_sec = SESSION_REAP_INTERVAL_SEC };
  evtimer_add(srv->ev_session_reap, &reap_tv);

  /* Operational state monitoring (non-fatal if it fails) */
  monitoring_init(srv, sr_conn, cfg->yang_dir);

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

  /* Close all active subscribe streams with UNAVAILABLE.
   * Walk a snapshot: stream_close may trigger stream_free which
   * modifies the list via gnmi_server_stream_unregister. */
  int n_streams = 0;
  struct stream_ctx *sctx = srv->active_streams;
  while (sctx) {
    struct stream_ctx *next = sctx->next_stream;
    stream_shutdown(sctx);
    n_streams++;
    sctx = next;
  }
  if (n_streams > 0)
    gnmi_log(GNMI_LOG_INFO, "Shutdown: closed %d active stream(s)", n_streams);

  /* Tell gRPC to stop accepting new calls */
  grpc_server_shutdown_and_notify(srv->grpc_srv, srv->cq, NULL);

  /* Start drain timer: keep polling the CQ so pending sends
   * (the UNAVAILABLE status to streams) can be flushed. After
   * DRAIN_TIMEOUT_SEC, break the event loop regardless. */
  srv->ev_drain = evtimer_new(srv->evbase, on_drain_cb, srv);
  struct timeval tv = { .tv_sec = DRAIN_TIMEOUT_SEC };
  evtimer_add(srv->ev_drain, &tv);
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

  /* Drain timer cleanup */
  if (srv->ev_drain) {
    event_del(srv->ev_drain);
    event_free(srv->ev_drain);
  }

  /* Session registry cleanup (candidate release must be before destroy) */
  if (srv->ev_session_reap) {
    event_del(srv->ev_session_reap);
    event_free(srv->ev_session_reap);
  }
  if (srv->sessions) {
    gnmi_session_candidate_cleanup_all(srv->sessions);
    uint32_t n = gnmi_session_count(srv->sessions);
    if (n > 0)
      gnmi_log(GNMI_LOG_INFO, "Shutdown: %u active session(s)", n);
    gnmi_session_registry_destroy(srv->sessions);
  }

  /* Monitoring cleanup */
  monitoring_cleanup();

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

gnmi_session_registry_t *gnmi_server_get_sessions(gnmi_server_t *srv)
{
  return srv->sessions;
}

uint64_t gnmi_server_uptime(gnmi_server_t *srv)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)(now.tv_sec - srv->start_time.tv_sec);
}

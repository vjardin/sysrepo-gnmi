/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "monitoring.h"
#include "server.h"
#include "session.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sysrepo.h>
#include <libyang/libyang.h>

#define MON_MODULE  "sysrepo-gnmi-monitoring"
#define MON_PATH    "/sysrepo-gnmi-monitoring:server-state"

static sr_session_ctx_t      *mon_sess;
static sr_subscription_ctx_t *mon_sub;
static gnmi_server_t         *mon_srv;

static int
oper_get_cb(sr_session_ctx_t *session, uint32_t sub_id,
    const char *module_name, const char *path,
    const char *request_xpath, uint32_t request_id,
    struct lyd_node **parent, void *private_data)
{
  (void)sub_id; (void)module_name; (void)path;
  (void)request_xpath; (void)request_id; (void)private_data;

  const struct ly_ctx *ctx = sr_session_acquire_context(session);
  gnmi_session_registry_t *reg = gnmi_server_get_sessions(mon_srv);
  char buf[64];

  /* uptime */
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)gnmi_server_uptime(mon_srv));
  lyd_new_path(NULL, ctx, MON_PATH "/uptime", buf, 0, parent);

  /* active-sessions */
  snprintf(buf, sizeof(buf), "%u", gnmi_session_count(reg));
  lyd_new_path(*parent, ctx, MON_PATH "/active-sessions", buf, 0, NULL);

  /* active-streams (total across all sessions) */
  snprintf(buf, sizeof(buf), "%u", gnmi_session_stream_total(reg));
  lyd_new_path(*parent, ctx, MON_PATH "/active-streams", buf, 0, NULL);

  /* counters: aggregate across all sessions */
  uint64_t total_rpcs = 0, failed_rpcs = 0;
  const struct gnmi_session *s;
  for (s = gnmi_session_first(reg); s; s = s->next) {
    total_rpcs += s->rpc_count;
    failed_rpcs += s->rpc_errors;
  }
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)total_rpcs);
  lyd_new_path(*parent, ctx, MON_PATH "/counters/total-rpcs", buf, 0, NULL);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)failed_rpcs);
  lyd_new_path(*parent, ctx, MON_PATH "/counters/failed-rpcs", buf, 0, NULL);

  /* per-session entries */
  for (s = gnmi_session_first(reg); s; s = s->next) {
    char sess_path[256];

    snprintf(sess_path, sizeof(sess_path),
             MON_PATH "/session[id='%lu']/peer-address",
             (unsigned long)s->id);
    lyd_new_path(*parent, ctx, sess_path, s->peer_addr, 0, NULL);

    if (s->username) {
      snprintf(sess_path, sizeof(sess_path),
               MON_PATH "/session[id='%lu']/username",
               (unsigned long)s->id);
      lyd_new_path(*parent, ctx, sess_path, s->username, 0, NULL);
    }

    snprintf(sess_path, sizeof(sess_path),
             MON_PATH "/session[id='%lu']/active-streams",
             (unsigned long)s->id);
    snprintf(buf, sizeof(buf), "%d", s->active_streams);
    lyd_new_path(*parent, ctx, sess_path, buf, 0, NULL);

    snprintf(sess_path, sizeof(sess_path),
             MON_PATH "/session[id='%lu']/rpc-count",
             (unsigned long)s->id);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s->rpc_count);
    lyd_new_path(*parent, ctx, sess_path, buf, 0, NULL);

    snprintf(sess_path, sizeof(sess_path),
             MON_PATH "/session[id='%lu']/rpc-errors",
             (unsigned long)s->id);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s->rpc_errors);
    lyd_new_path(*parent, ctx, sess_path, buf, 0, NULL);
  }

  sr_session_release_context(session);
  return SR_ERR_OK;
}

int monitoring_init(gnmi_server_t *srv, sr_conn_ctx_t *conn, const char *yang_dir)
{
  mon_srv = srv;

  /* Install the monitoring YANG module if not already present */
  char yang_path[512];
  snprintf(yang_path, sizeof(yang_path), "%s/sysrepo-gnmi-monitoring.yang",
           yang_dir ? yang_dir : ".");

  int rc = sr_install_module(conn, yang_path, yang_dir, NULL);
  if (rc != SR_ERR_OK && rc != SR_ERR_EXISTS) {
    gnmi_log(GNMI_LOG_WARNING, "monitoring: sr_install_module failed: %s",
             sr_strerror(rc));
    return -1;
  }

  /* Start a session and subscribe for operational data */
  rc = sr_session_start(conn, SR_DS_OPERATIONAL, &mon_sess);
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_WARNING, "monitoring: sr_session_start failed: %s",
             sr_strerror(rc));
    return -1;
  }

  rc = sr_oper_get_subscribe(mon_sess, MON_MODULE, MON_PATH,
      oper_get_cb, NULL, 0, &mon_sub);
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_WARNING, "monitoring: sr_oper_get_subscribe failed: %s",
             sr_strerror(rc));
    sr_session_stop(mon_sess);
    mon_sess = NULL;
    return -1;
  }

  gnmi_log(GNMI_LOG_INFO, "Monitoring: operational state provider registered");
  return 0;
}

void monitoring_cleanup(void)
{
  if (mon_sub)
    sr_unsubscribe(mon_sub);
  mon_sub = NULL;
  if (mon_sess)
    sr_session_stop(mon_sess);
  mon_sess = NULL;
  mon_srv = NULL;
}

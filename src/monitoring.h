/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <sysrepo.h>

typedef struct gnmi_server gnmi_server_t;

/* Initialize monitoring: install YANG module if needed, subscribe to
 * provide operational data.  Call after sr_connect + server create. */
int monitoring_init(gnmi_server_t *srv, sr_conn_ctx_t *conn, const char *yang_dir);

/* Cleanup: unsubscribe and release resources. */
void monitoring_cleanup(void);

/* Session lifecycle notifications */
void monitoring_notify_session_start(uint64_t id, const char *peer, const char *username);

/* Notify session end. killed_by is the session ID of the operator that
 * issued kill-session (0 if not applicable). */
void monitoring_notify_session_end(uint64_t id, const char *peer,
    const char *reason, uint64_t killed_by);

/* Confirmed-commit lifecycle notifications.
 * event: "start", "confirm", "cancel", "timeout-rollback" */
void monitoring_notify_confirmed_commit(const char *commit_id, const char *event);

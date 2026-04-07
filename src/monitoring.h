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

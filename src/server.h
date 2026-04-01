/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdbool.h>
#include <sysrepo.h>

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
};

gnmi_server_t *gnmi_server_create(const struct gnmi_config *cfg, sr_conn_ctx_t *sr_conn);
int  gnmi_server_run(gnmi_server_t *srv);
void gnmi_server_shutdown(gnmi_server_t *srv);
void gnmi_server_destroy(gnmi_server_t *srv);

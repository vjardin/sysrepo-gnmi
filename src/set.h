/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include "gnmi_service.h"

struct event_base;  /* forward */

grpc_status_code handle_set(sr_conn_ctx_t *sr_conn, const char *user, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg);

/* Initialize candidate datastore support (stores event_base for idle timer). */
void candidate_init(struct event_base *evbase);

/* Release candidate datastore session if held (called on shutdown). */
void candidate_cleanup(void);

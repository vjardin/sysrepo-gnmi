/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include "gnmi_service.h"

struct gnmi_session;

grpc_status_code handle_get(sr_conn_ctx_t *sr_conn,
          const struct gnmi_session *session,
          grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg);

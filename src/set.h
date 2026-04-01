/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include "gnmi_service.h"

grpc_status_code handle_set(sr_conn_ctx_t *sr_conn, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg);

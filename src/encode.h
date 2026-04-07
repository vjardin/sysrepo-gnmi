/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <libyang/libyang.h>
#include <sysrepo.h>
#include <grpc/status.h>
#include "gnmi.pb-c.h"

/* Encode a libyang data node to JSON_IETF string.
 * Caller must free() the returned string. Returns NULL on error. */
char *encode_json_ietf(const struct lyd_node *node);

/* Encode a data node into a Gnmi__TypedValue.
 * Sets the json_ietf_val field. Returns gRPC status code. */
grpc_status_code encode_node(Gnmi__Encoding encoding, const struct lyd_node *node, Gnmi__TypedValue *val,
           char **err_msg);

/* Decode a JSON_IETF TypedValue into a libyang data tree.
 * For Set: parses data at the given xpath edit point.
 * Caller must free the returned tree with lyd_free_all().
 * Returns gRPC status code. */
grpc_status_code decode_json_ietf(sr_session_ctx_t *sess, const char *xpath, const uint8_t *json, size_t json_len,
          struct lyd_node **out,
          char **err_msg);

/* Free a dynamically allocated Gnmi__TypedValue's inner data.
 * Handles all encoding types (json_ietf_val, ascii_val, string_val, etc).
 * Does NOT free the val struct itself. */
void gnmi_typed_value_free(Gnmi__TypedValue *val);

/* Free a dynamically allocated Gnmi__Update (path + val + struct). */
void gnmi_update_free(Gnmi__Update *u);

/* Decode + attach NETCONF operation metadata for Set operations.
 * operation is "merge", "replace", or "remove".
 * Returns gRPC status code. *out is the root of the edit tree. */
grpc_status_code decode_update(sr_session_ctx_t *sess, const char *xpath, const Gnmi__TypedValue *val,
             const char *operation,
             struct lyd_node **out,
             char **err_msg);

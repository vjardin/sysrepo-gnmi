/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "rpc.h"
#include "xpath.h"
#include "encode.h"
#include "log.h"
#include "compat.h"

#include <stdlib.h>
#include <string.h>

#define RPC_DEFAULT_TIMEOUT_MS  2000   /* 2s default RPC timeout */
#define RPC_MAX_TIMEOUT_MS     10000   /* 10s maximum RPC timeout */

#include <sysrepo.h>
#include <libyang/libyang.h>
#include <libyang/in.h>
#include <cjson/cJSON.h>

#include "gnmi.pb-c.h"

grpc_status_code handle_rpc(sr_conn_ctx_t *sr_conn, const char *user, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg)
{
  Gnmi__RpcRequest *req = NULL;
  Gnmi__RpcResponse resp = GNMI__RPC_RESPONSE__INIT;
  sr_session_ctx_t *sess = NULL;
  struct lyd_node *input = NULL;
  sr_data_t *output = NULL;
  char *xpath = NULL;
  grpc_status_code ret = GRPC_STATUS_INTERNAL;

  /* Unpack request */
  req = (Gnmi__RpcRequest *)gnmi_unpack( &gnmi__rpc_request__descriptor, request_bb);
  if (!req) {
    *status_msg = strdup("Failed to parse RpcRequest");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Convert path to xpath */
  xpath = gnmi_to_xpath(req->path, status_msg);
  if (!xpath) {
    ret = GRPC_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }

  /* Validate value is set and is JSON_IETF */
  if (!req->val ||
      req->val->value_case == GNMI__TYPED_VALUE__VALUE__NOT_SET) {
    *status_msg = strdup("Value not set");
    ret = GRPC_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  if (req->val->value_case != GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL) {
    *status_msg = strdup("Only JSON_IETF encoding supported");
    ret = GRPC_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }

  uint32_t timeout_ms = req->timeout ? req->timeout * 1000 : RPC_DEFAULT_TIMEOUT_MS;
  if (timeout_ms > RPC_MAX_TIMEOUT_MS)
    timeout_ms = RPC_MAX_TIMEOUT_MS;

  gnmi_log(GNMI_LOG_DEBUG, "Rpc RPC (%s) timeout %ums", xpath, timeout_ms);

  /* Create session (with NACM user if configured) */
  int rc = gnmi_nacm_session_start_as(sr_conn, SR_DS_RUNNING, user, &sess);
  if (rc != SR_ERR_OK) {
    *status_msg = strdup("Failed to start sysrepo session");
    goto cleanup;
  }

  /* Parse the RPC input using lyd_parse_op with LYD_TYPE_RPC_YANG */
  {
    const struct ly_ctx *ctx = sr_session_acquire_context(sess);

    /* Build JSON wrapping: {"module:rpc-name": {"input-leaf": value}} */
    char *json_data = strndup( (const char *)req->val->json_ietf_val.data, req->val->json_ietf_val.len);

    /* The JSON from the client contains only the input children.
     * We need to wrap it in the RPC path structure for lyd_parse_op.
     * Format: {"module:rpc-name": {<input children>}}
     * Use cJSON to build it properly. */
    const char *full_rpc_name = xpath + 1; /* skip leading / */

    cJSON *input_obj = cJSON_Parse(json_data);
    cJSON *wrapper = cJSON_CreateObject();
    if (!wrapper || !input_obj) {
      cJSON_Delete(input_obj);
      cJSON_Delete(wrapper);
      sr_session_release_context(sess);
      free(json_data);
      *status_msg = strdup("Failed to parse RPC input JSON");
      ret = GRPC_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
    cJSON_AddItemToObject(wrapper, full_rpc_name, input_obj);
    char *wrapped = cJSON_PrintUnformatted(wrapper);
    cJSON_Delete(wrapper); /* also frees input_obj */

    struct ly_in *in = NULL;
    LY_ERR ly_err = ly_in_new_memory(wrapped, &in);
    if (ly_err != LY_SUCCESS) {
      sr_session_release_context(sess);
      free(json_data);
      cJSON_free(wrapped);
      *status_msg = strdup("Failed to create input");
      ret = GRPC_STATUS_INTERNAL;
      goto cleanup;
    }

    struct lyd_node *op_node = NULL;
    ly_err = gnmi_lyd_parse_op(ctx, NULL, in, LYD_JSON, LYD_TYPE_RPC_YANG, 0, &input, &op_node);
    ly_in_free(in, 0);
    sr_session_release_context(sess);
    free(json_data);
    cJSON_free(wrapped);

    if (ly_err != LY_SUCCESS || !input) {
      *status_msg = strdup("Failed to parse RPC input");
      ret = GRPC_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }

  /* Send the RPC to sysrepo */
  rc = sr_rpc_send_tree(sess, input, timeout_ms, &output);
  if (rc != SR_ERR_OK) {
    const sr_error_info_t *err_info = NULL;
    sr_session_get_error(sess, &err_info);
    if (err_info && err_info->err_count > 0)
      *status_msg = strdup(err_info->err[0].message);
    else
      *status_msg = strdup(sr_strerror(rc));
    ret = GRPC_STATUS_ABORTED;
    goto cleanup;
  }

  /* Build response */
  resp.timestamp = get_time_nanosec();

  /* Encode output if present */
  if (output && output->tree) {
    resp.val = calloc(1, sizeof(*resp.val));
    gnmi__typed_value__init(resp.val);

    /* Find the RPC output node in the tree */
    struct lyd_node *out_node = lyd_child(output->tree);
    if (out_node) {
      char *json = NULL;
      lyd_print_mem(&json, out_node, LYD_JSON, GNMI_LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK);
      if (json) {
        resp.val->value_case =
          GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL;
        resp.val->json_ietf_val.data = (uint8_t *)json;
        resp.val->json_ietf_val.len = strlen(json);
      }
    }
  }

  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
  ret = GRPC_STATUS_OK;

cleanup:
  if (resp.val) {
    gnmi_typed_value_free(resp.val);
    free(resp.val);
  }
  if (output)
    sr_release_data(output);
  if (input)
    lyd_free_all(input);
  free(xpath);
  if (sess)
    sr_session_stop(sess);
  if (req)
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);

  return ret;
}

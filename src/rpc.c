/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "rpc.h"
#include "xpath.h"
#include "encode.h"
#include "session.h"
#include "log.h"
#include "compat.h"

#include <stdlib.h>
#include <string.h>

#define RPC_DEFAULT_TIMEOUT_MS  2000   /* 2s default RPC timeout */
#define RPC_MAX_TIMEOUT_MS     10000   /* 10s maximum RPC timeout */

#include <errno.h>

#include <sysrepo.h>
#include <libyang/libyang.h>
#include <libyang/in.h>
#include <cjson/cJSON.h>

#include "gnmi.pb-c.h"

/* get-schema: read YANG source from sysrepo repository */

static grpc_status_code
handle_get_schema(sr_conn_ctx_t *sr_conn, const Gnmi__RpcRequest *req,
    grpc_byte_buffer **response_bb, char **status_msg)
{
  /* Parse input JSON: {"identifier": "...", "version": "..."} */
  char *json_in = strndup((const char *)req->val->json_ietf_val.data,
                          req->val->json_ietf_val.len);
  cJSON *obj = cJSON_Parse(json_in);
  free(json_in);
  if (!obj) {
    *status_msg = strdup("Invalid JSON input");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "identifier"));
  const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "version"));
  if (!name || !name[0]) {
    cJSON_Delete(obj);
    *status_msg = strdup("Missing 'identifier' in get-schema input");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Look up the module to get its revision */
  sr_session_ctx_t *sess = NULL;
  int rc = sr_session_start(sr_conn, SR_DS_RUNNING, &sess);
  if (rc != SR_ERR_OK) {
    cJSON_Delete(obj);
    *status_msg = strdup("Failed to start session");
    return GRPC_STATUS_INTERNAL;
  }

  const struct ly_ctx *ctx = sr_session_acquire_context(sess);
  const struct lys_module *mod;
  if (version && version[0])
    mod = ly_ctx_get_module(ctx, name, version);
  else
    mod = ly_ctx_get_module_implemented(ctx, name);

  if (!mod) {
    sr_session_release_context(sess);
    sr_session_stop(sess);
    if (asprintf(status_msg, "Module '%s' not found", name) < 0)
      *status_msg = strdup("Module not found");
    cJSON_Delete(obj);
    return GRPC_STATUS_NOT_FOUND;
  }

  /* Build file path: <repo>/yang/<name>@<rev>.yang */
  char yang_path[512];
  if (mod->revision)
    snprintf(yang_path, sizeof(yang_path), "%s/yang/%s@%s.yang",
             sr_get_repo_path(), name, mod->revision);
  else
    snprintf(yang_path, sizeof(yang_path), "%s/yang/%s.yang",
             sr_get_repo_path(), name);

  sr_session_release_context(sess);
  sr_session_stop(sess);

  /* Read the YANG file */
  FILE *f = fopen(yang_path, "r");
  if (!f) {
    cJSON_Delete(obj);
    if (asprintf(status_msg, "Cannot read schema file: %s", strerror(errno)) < 0)
      *status_msg = strdup("Cannot read schema file");
    return GRPC_STATUS_NOT_FOUND;
  }

  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *schema = malloc(fsize + 1);
  if (!schema) {
    fclose(f);
    cJSON_Delete(obj);
    *status_msg = strdup("Out of memory");
    return GRPC_STATUS_RESOURCE_EXHAUSTED;
  }
  size_t nread = fread(schema, 1, fsize, f);
  schema[nread] = '\0';
  fclose(f);

  gnmi_log(GNMI_LOG_DEBUG, "get-schema: returned '%s' (%zu bytes)", name, nread);
  cJSON_Delete(obj);

  /* Build JSON response: {"sysrepo-gnmi-monitoring:schema": "..."} */
  cJSON *out_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(out_obj, "sysrepo-gnmi-monitoring:schema", schema);
  char *out_json = cJSON_PrintUnformatted(out_obj);
  cJSON_Delete(out_obj);
  free(schema);

  Gnmi__RpcResponse resp = GNMI__RPC_RESPONSE__INIT;
  resp.timestamp = get_time_nanosec();
  resp.val = calloc(1, sizeof(*resp.val));
  gnmi__typed_value__init(resp.val);
  resp.val->value_case = GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL;
  resp.val->json_ietf_val.data = (uint8_t *)out_json;
  resp.val->json_ietf_val.len = strlen(out_json);

  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);

  cJSON_free(out_json);
  free(resp.val);
  return GRPC_STATUS_OK;
}

grpc_status_code handle_rpc(sr_conn_ctx_t *sr_conn,
          struct gnmi_session *session,
          grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg)
{
  const char *user = session ? session->username : NULL;
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

  /* Short-circuit: handle get-schema directly (sysrepo RPC callback
   * output doesn't propagate through sr_rpc_send_tree in-process) */
  if (strcmp(xpath, "/sysrepo-gnmi-monitoring:get-schema") == 0) {
    if (req->val && req->val->value_case == GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL) {
      ret = handle_get_schema(sr_conn, req, response_bb, status_msg);
    } else {
      *status_msg = strdup("get-schema requires JSON_IETF input");
      ret = GRPC_STATUS_INVALID_ARGUMENT;
    }
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
    *status_msg = gnmi_collect_sr_errors(sess, rc);
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

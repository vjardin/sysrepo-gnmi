/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "encode.h"
#include "log.h"
#include "compat.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libyang/in.h>
#include <libyang/tree_schema.h>
#include <cjson/cJSON.h>

/* - JSON unwrap helpers ---------------------------------------------- */

/* Unwrap a container-wrapped JSON: {"module:name": {...}} -> "{...}"
 * Returns a newly allocated string (caller frees), or strdup("{}"). */
static char *json_unwrap_container(const char *json)
{
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return strdup("{}");
  cJSON *wrapper = root->child;
  if (!wrapper) {
    cJSON_Delete(root);
    return strdup("{}");
  }
  char *cj = cJSON_PrintUnformatted(wrapper);
  cJSON_Delete(root);
  if (!cj)
    return strdup("{}");
  char *result = strdup(cj);
  cJSON_free(cj);
  return result ? result : strdup("{}");
}

/* Extract a named child value from parent JSON.
 * Parent JSON: {"module:parent": {"module:child": {...}, ...}}
 * Tries both "module:name" and "name" as keys.
 * Returns a newly allocated string (caller frees), or strdup("{}"). */
static char *json_extract_child(const char *json, const char *module, const char *name)
{
  cJSON *root = cJSON_Parse(json);
  if (!root)
    return strdup("{}");
  /* Navigate into wrapper: {"mod:parent": {inner}} */
  cJSON *parent_val = root->child;
  if (!parent_val || !cJSON_IsObject(parent_val)) {
    cJSON_Delete(root);
    return strdup("{}");
  }
  /* Try qualified name first, then bare name */
  char qualified[256];
  snprintf(qualified, sizeof(qualified), "%s:%s", module, name);
  cJSON *child = cJSON_GetObjectItem(parent_val, qualified);
  if (!child)
    child = cJSON_GetObjectItem(parent_val, name);
  if (!child) {
    cJSON_Delete(root);
    return strdup("{}");
  }
  char *cj = cJSON_PrintUnformatted(child);
  cJSON_Delete(root);
  if (!cj)
    return strdup("{}");
  char *result = strdup(cj);
  cJSON_free(cj);
  return result ? result : strdup("{}");
}

/* - encode_json_ietf: lyd_node -> JSON string ---------------------- */

char *encode_json_ietf(const struct lyd_node *node)
{
  char *json = NULL;
  LY_ERR err;

  if (!node || !node->schema)
    return strdup("{}");

  switch (node->schema->nodetype) {
  case LYS_LEAF:
  case LYS_LEAFLIST:
    /* Print single value with shrink */
    err = lyd_print_mem(&json, node, LYD_JSON, LYD_PRINT_SHRINK);
    if (err != LY_SUCCESS || !json)
      return strdup("");

    /* lyd_print_mem wraps leaf in {"module:name": value}
     * Use cJSON to extract the bare value robustly. */
    {
      cJSON *root = cJSON_Parse(json);
      if (root) {
        cJSON *item = root->child;
        if (item) {
          char *cj = cJSON_PrintUnformatted(item);
          cJSON_Delete(root);
          free(json);
          if (!cj)
            return strdup("");
          char *bare = strdup(cj);
          cJSON_free(cj);
          return bare ? bare : strdup("");
        }
        cJSON_Delete(root);
      }
    }
    return json;

  case LYS_CONTAINER:
  case LYS_LIST:
  case LYS_RPC:
  case LYS_ACTION:
  case LYS_NOTIF: {
    const char *mod_name = node->schema->module->name;
    const char *node_name = node->schema->name;

    /* If no children, return empty object */
    const struct lyd_node *child = lyd_child(node);
    if (!child)
      return strdup("{}");

    /* Step 1: print children with siblings */
    err = lyd_print_mem(&json, child, LYD_JSON, GNMI_LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK);
    if (err == LY_SUCCESS && json && json[0] != '\0')
      return json;

    gnmi_log(GNMI_LOG_DEBUG, "encode: lyd_print_mem empty for children of %s:%s", mod_name, node_name);
    free(json);
    json = NULL;

    /* Step 2: print the node itself and unwrap {"module:name": {...}} */
    err = lyd_print_mem(&json, node, LYD_JSON, LYD_PRINT_SHRINK);
    if (err == LY_SUCCESS && json && json[0] != '\0') {
      char *unwrapped = json_unwrap_container(json);
      free(json);
      gnmi_log(GNMI_LOG_DEBUG, "encode: node-level print succeeded for %s:%s", mod_name, node_name);
      return unwrapped;
    }

    gnmi_log(GNMI_LOG_DEBUG, "encode: node-level print also empty for %s:%s", mod_name, node_name);
    free(json);
    json = NULL;

    /* Step 3: print node with siblings flag */
    err = lyd_print_mem(&json, node, LYD_JSON, GNMI_LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK);
    if (err == LY_SUCCESS && json && json[0] != '\0') {
      char *unwrapped = json_unwrap_container(json);
      free(json);
      gnmi_log(GNMI_LOG_DEBUG, "encode: node+siblings print succeeded for %s:%s", mod_name, node_name);
      return unwrapped;
    }

    free(json);
    json = NULL;

    /* Step 4: walk up to parent and extract our node's content */
    if (node->parent) {
      gnmi_log(GNMI_LOG_DEBUG, "encode: trying parent for %s:%s", mod_name, node_name);
      err = lyd_print_mem(&json, lyd_parent(node), LYD_JSON, LYD_PRINT_SHRINK);
      if (err == LY_SUCCESS && json && json[0] != '\0') {
        char *extracted = json_extract_child(json, mod_name, node_name);
        free(json);
        gnmi_log(GNMI_LOG_DEBUG, "encode: parent print succeeded for %s:%s", mod_name, node_name);
        return extracted;
      }
      free(json);
    }

    gnmi_log(GNMI_LOG_WARNING, "encode: all print attempts failed for %s:%s", mod_name, node_name);
    return strdup("{}");
  }

  default: return strdup("{}");
  }
}

/* - encode_node: dispatch to JSON_IETF ---------------------------- */

grpc_status_code encode_node(Gnmi__Encoding encoding, const struct lyd_node *node, Gnmi__TypedValue *val,
           char **err_msg)
{
  (void)err_msg;

  switch (encoding) {
  case GNMI__ENCODING__JSON:
  case GNMI__ENCODING__JSON_IETF: {
    char *json = encode_json_ietf(node);
    if (!json)
      return GRPC_STATUS_INTERNAL;

    gnmi__typed_value__init(val);
    val->value_case = GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL;
    val->json_ietf_val.data = (uint8_t *)json;
    val->json_ietf_val.len = strlen(json);
    return GRPC_STATUS_OK;
  }

  case GNMI__ENCODING__PROTO: {
    /* PROTO encoding: use native TypedValue fields for leaves based on
     * YANG type; fall back to json_ietf_val for containers/lists. */
    gnmi__typed_value__init(val);
    if (node->schema &&
        (node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
      const char *str_val = lyd_get_value(node);
      LY_DATA_TYPE bt = ((struct lysc_node_leaf *)node->schema)->type->basetype;
      switch (bt) {
      case LY_TYPE_BOOL:
        val->value_case = GNMI__TYPED_VALUE__VALUE_BOOL_VAL;
        val->bool_val = (str_val && strcmp(str_val, "true") == 0);
        break;
      case LY_TYPE_INT8:
      case LY_TYPE_INT16:
      case LY_TYPE_INT32:
      case LY_TYPE_INT64:
        val->value_case = GNMI__TYPED_VALUE__VALUE_INT_VAL;
        val->int_val = str_val ? strtoll(str_val, NULL, 10) : 0;
        break;
      case LY_TYPE_UINT8:
      case LY_TYPE_UINT16:
      case LY_TYPE_UINT32:
      case LY_TYPE_UINT64:
        val->value_case = GNMI__TYPED_VALUE__VALUE_UINT_VAL;
        val->uint_val = str_val ? strtoull(str_val, NULL, 10) : 0;
        break;
      case LY_TYPE_DEC64:
        val->value_case = GNMI__TYPED_VALUE__VALUE_FLOAT_VAL;
        val->float_val = str_val ? strtof(str_val, NULL) : 0.0f;
        break;
      case LY_TYPE_EMPTY:
        val->value_case = GNMI__TYPED_VALUE__VALUE_BOOL_VAL;
        val->bool_val = true;
        break;
      default:
        /* STRING, ENUM, IDENT, BINARY, BITS, LEAFREF, UNION, INST */
        val->value_case = GNMI__TYPED_VALUE__VALUE_STRING_VAL;
        val->string_val = strdup(str_val ? str_val : "");
        break;
      }
    } else {
      /* Container/list: use json_ietf_val as compact representation */
      char *json = encode_json_ietf(node);
      if (!json)
        return GRPC_STATUS_INTERNAL;
      val->value_case = GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL;
      val->json_ietf_val.data = (uint8_t *)json;
      val->json_ietf_val.len = strlen(json);
    }
    return GRPC_STATUS_OK;
  }

  case GNMI__ENCODING__ASCII: {
    /* ASCII: plain string for leaves, JSON for containers */
    char *ascii = NULL;
    if (node->schema &&
        (node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
      const char *v = lyd_get_value(node);
      ascii = v ? strdup(v) : strdup("");
    } else {
      lyd_print_mem(&ascii, node, LYD_JSON,
        GNMI_LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK);
      if (!ascii)
        ascii = strdup("{}");
    }
    gnmi__typed_value__init(val);
    val->value_case = GNMI__TYPED_VALUE__VALUE_ASCII_VAL;
    val->ascii_val = ascii;
    return GRPC_STATUS_OK;
  }

  default: if (err_msg)
      *err_msg = strdup("Unsupported encoding");
    return GRPC_STATUS_UNIMPLEMENTED;
  }
}

/* - decode_json_ietf: JSON string -> lyd_node tree ----------------- */

grpc_status_code decode_json_ietf(sr_session_ctx_t *sess, const char *xpath, const uint8_t *json, size_t json_len,
          struct lyd_node **out,
          char **err_msg)
{
  /* NOTE: ctx is held from here until 'done:' label.
   * All error paths must goto done, never return directly. */
  const struct ly_ctx *ctx = sr_session_acquire_context(sess);
  struct lyd_node *tree = NULL;
  LY_ERR ly_err;
  grpc_status_code ret = GRPC_STATUS_OK;

  *out = NULL;

  /* Make a null-terminated copy of the JSON data */
  char *data = strndup((const char *)json, json_len);
  if (!data) {
    ret = GRPC_STATUS_INTERNAL;
    goto done;
  }

  /* Root case: parse entire config */
  if (strcmp(xpath, "/*") == 0) {
    ly_err = lyd_parse_data_mem(ctx, data, LYD_JSON, LYD_PARSE_ONLY | LYD_PARSE_STRICT, 0, &tree);
    if (ly_err != LY_SUCCESS) {
      if (err_msg)
        *err_msg = strdup("Failed to parse JSON data");
      ret = GRPC_STATUS_INVALID_ARGUMENT;
      goto done;
    }
    *out = tree;
    goto done;
  }

  /* Find the schema node to determine type */
  const struct lysc_node *schema = lys_find_path(ctx, NULL, xpath, 0);

  if (schema && (schema->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
    /* Leaf: strip surrounding quotes if present */
    size_t dlen = strlen(data);
    if (dlen >= 2 && data[0] == '"' && data[dlen - 1] == '"') {
      memmove(data, data + 1, dlen - 2);
      data[dlen - 2] = '\0';
    }
    /* Handle empty leaf: [null] -> "" */
    if (strcmp(data, "[null]") == 0)
      data[0] = '\0';

    /* Create path with the actual leaf value */
    const char *val = data[0] ? data : NULL;
    ly_err = gnmi_lyd_new_path2(NULL, ctx, xpath, val, LYD_NEW_PATH_UPDATE, &tree, NULL);
    if (ly_err != LY_SUCCESS || !tree) {
      if (err_msg)
        *err_msg = strdup("Failed to create leaf path");
      ret = GRPC_STATUS_INVALID_ARGUMENT;
      goto done;
    }
    *out = tree;
    goto done;
  }

  /* Non-leaf: create tree structure at xpath */
  ly_err = gnmi_lyd_new_path2(NULL, ctx, xpath, NULL, LYD_NEW_PATH_UPDATE, &tree, NULL);
  if (ly_err != LY_SUCCESS || !tree) {
    if (err_msg)
      *err_msg = strdup("Failed to create path");
    ret = GRPC_STATUS_INVALID_ARGUMENT;
    goto done;
  }

  /* Container/List: parse JSON fragment as children of the edit point.
   * Use lyd_parse_data() with the parent parameter so libyang
   * interprets JSON keys relative to the parent node - no need for
   * module-qualified top-level keys in the fragment. */
  if (data[0] != '\0' && strcmp(data, "{}") != 0) {
    struct ly_set *set = NULL;
    lyd_find_xpath(tree, xpath, &set);
    if (set && set->count > 0) {
      struct lyd_node *edit_node = set->dnodes[0];
      struct ly_in *in = NULL;

      ly_err = ly_in_new_memory(data, &in);
      if (ly_err != LY_SUCCESS) {
        if (err_msg)
          *err_msg = strdup(
            "Failed to create input");
        ly_set_free(set, NULL);
        lyd_free_all(tree);
        *out = NULL;
        ret = GRPC_STATUS_INTERNAL;
        goto done;
      }

      /* Parse with parent context - children are added
       * directly under edit_node */
      ly_err = lyd_parse_data(ctx, edit_node, in, LYD_JSON, LYD_PARSE_ONLY |
            LYD_PARSE_STRICT,
            0, NULL);
      ly_in_free(in, 0);

      if (ly_err != LY_SUCCESS) {
        if (err_msg)
          *err_msg = strdup(
            "Failed to parse JSON fragment");
        ly_set_free(set, NULL);
        lyd_free_all(tree);
        *out = NULL;
        ret = GRPC_STATUS_INVALID_ARGUMENT;
        goto done;
      }
    }
    if (set)
      ly_set_free(set, NULL);
  }

  *out = tree;

done:
  free(data);
  sr_session_release_context(sess);
  return ret;
}

/* - decode_update: decode + attach operation metadata ------------- */

grpc_status_code decode_update(sr_session_ctx_t *sess, const char *xpath, const Gnmi__TypedValue *val,
             const char *operation,
             struct lyd_node **out,
             char **err_msg)
{
  *out = NULL;

  /* Validate value is JSON_IETF */
  if (!val || val->value_case != GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL) {
    if (!val || val->value_case == GNMI__TYPED_VALUE__VALUE__NOT_SET) {
      if (err_msg)
        *err_msg = strdup("Value not set");
    } else {
      if (err_msg)
        *err_msg = strdup(
          "Only JSON_IETF encoding supported");
    }
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Decode the JSON value into a libyang tree */
  struct lyd_node *tree = NULL;
  grpc_status_code rc = decode_json_ietf( sess, xpath, val->json_ietf_val.data, val->json_ietf_val.len,
    &tree, err_msg);
  if (rc != GRPC_STATUS_OK)
    return rc;
  /* NULL tree is valid for replace with {} (empty config) */
  if (!tree) {
    *out = NULL;
    return GRPC_STATUS_OK;
  }

  /* For replace on list/leaflist: attach sysrepo:operation=purge
   * to clear existing entries before inserting new ones */
  if (strcmp(operation, "replace") == 0 && strcmp(xpath, "/*") != 0) {
    const struct ly_ctx *pctx = sr_session_acquire_context(sess);
    const struct lysc_node *schema = lys_find_path(pctx, NULL, xpath, 0);
    if (schema && (schema->nodetype & (LYS_LIST | LYS_LEAFLIST))) {
      const struct lys_module *sr_mod =
        ly_ctx_get_module_implemented(pctx, "sysrepo");
      if (sr_mod) {
        struct ly_set *pset = NULL;
        lyd_find_xpath(tree, xpath, &pset);
        if (pset) {
          for (uint32_t i = 0; i < pset->count; i++)
            lyd_new_meta(pctx, pset->dnodes[i], sr_mod, "operation",
                   "purge", 0, NULL);
          ly_set_free(pset, NULL);
        }
      }
    }
    sr_session_release_context(sess);
  }

  /* Attach NETCONF operation metadata to the edit point */
  const struct ly_ctx *ctx = sr_session_acquire_context(sess);
  const struct lys_module *nc_mod =
    ly_ctx_get_module_implemented(ctx, "ietf-netconf");

  if (nc_mod) {
    struct ly_set *set = NULL;
    if (strcmp(xpath, "/*") == 0) {
      /* Root: tag each top-level sibling */
      struct lyd_node *n;
      LY_LIST_FOR(tree, n) {
        lyd_new_meta(ctx, n, nc_mod, "operation", operation, 0, NULL);
      }
    } else {
      /* Specific path: find and tag edit point */
      lyd_find_xpath(tree, xpath, &set);
      if (set) {
        for (uint32_t i = 0; i < set->count; i++) {
          lyd_new_meta(ctx, set->dnodes[i], nc_mod, "operation", operation, 0, NULL);
        }
        ly_set_free(set, NULL);
      }
    }
  }

  sr_session_release_context(sess);
  *out = tree;
  return GRPC_STATUS_OK;
}

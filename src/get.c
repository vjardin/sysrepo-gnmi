/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#include "get.h"
#include "xpath.h"
#include "encode.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#include <sysrepo.h>
#include <libyang/libyang.h>

#include "gnmi.pb-c.h"

/* - Build updates for a single xpath ------------------------------ */

static grpc_status_code
build_get_updates(sr_session_ctx_t *sess, const char *fullpath, Gnmi__Notification *notif, char **err_msg)
{
  sr_data_t *sr_data = NULL;
  struct ly_set *set = NULL;
  grpc_status_code ret = GRPC_STATUS_OK;

  /* Fetch data from sysrepo */
  int rc = sr_get_data(sess, fullpath, 0, 0, 0, &sr_data);
  if (rc != SR_ERR_OK) {
    if (rc == SR_ERR_NOT_FOUND)
      return GRPC_STATUS_OK; /* empty is fine */
    if (err_msg)
      *err_msg = strdup(sr_strerror(rc));
    return GRPC_STATUS_INTERNAL;
  }

  if (!sr_data || !sr_data->tree)
    goto cleanup;

  /* Find matching nodes in the returned tree */
  LY_ERR ly_err = lyd_find_xpath(sr_data->tree, fullpath, &set);
  if (ly_err != LY_SUCCESS || !set || set->count == 0)
    goto cleanup;

  /* Allocate update array */
  notif->n_update = set->count;
  notif->update = calloc(set->count, sizeof(Gnmi__Update *));
  if (!notif->update) {
    ret = GRPC_STATUS_INTERNAL;
    goto cleanup;
  }

  for (uint32_t i = 0; i < set->count; i++) {
    struct lyd_node *node = set->dnodes[i];

    Gnmi__Update *upd = calloc(1, sizeof(*upd));
    gnmi__update__init(upd);

    /* Encode path */
    upd->path = calloc(1, sizeof(*upd->path));
    if (node_to_gnmi_path(node, upd->path) < 0) {
      free(upd->path);
      free(upd);
      ret = GRPC_STATUS_INTERNAL;
      goto cleanup;
    }

    /* Encode value */
    upd->val = calloc(1, sizeof(*upd->val));
    ret = encode_node(GNMI__ENCODING__JSON_IETF, node, upd->val, err_msg);
    if (ret != GRPC_STATUS_OK) {
      gnmi_path_free_elems(upd->path);
      free(upd->path);
      free(upd->val);
      free(upd);
      goto cleanup;
    }

    notif->update[i] = upd;
  }

cleanup:
  if (set)
    ly_set_free(set, NULL);
  if (sr_data)
    sr_release_data(sr_data);
  return ret;
}

/* - Build one notification per requested path --------------------- */

static grpc_status_code
build_get_notification(sr_session_ctx_t *sess, const Gnmi__Path *prefix, const Gnmi__Path *path,
           Gnmi__Encoding encoding,
           Gnmi__GetRequest__DataType data_type,
           Gnmi__Notification *notif,
           char **err_msg)
{
  (void)encoding; /* only JSON_IETF supported */
  grpc_status_code ret;
  sr_datastore_t ds = SR_DS_OPERATIONAL;
  sr_datastore_t orig_ds;

  gnmi__notification__init(notif);
  notif->timestamp = get_time_nanosec();

  /* Process prefix: set target in response */
  if (prefix && prefix->target && prefix->target[0]) {
    notif->prefix = calloc(1, sizeof(*notif->prefix));
    gnmi__path__init(notif->prefix);
    notif->prefix->target = strdup(prefix->target);
  }

  /* Merge prefix + path into full xpath */
  char *fullpath = gnmi_merge_xpath(prefix, path, err_msg);
  if (!fullpath)
    return GRPC_STATUS_INVALID_ARGUMENT;

  /* Validate origin */
  if (gnmi_check_origin(prefix, path, err_msg) < 0) {
    free(fullpath);
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  gnmi_log(GNMI_LOG_DEBUG, "Get path: %s", fullpath);

  /* Datastore selection */
  if (data_type == GNMI__GET_REQUEST__DATA_TYPE__CONFIG)
    ds = SR_DS_RUNNING;

  /* Switch datastore (save + restore) */
  orig_ds = sr_session_get_ds(sess);
  sr_session_switch_ds(sess, ds);

  ret = build_get_updates(sess, fullpath, notif, err_msg);

  /* Restore original datastore */
  sr_session_switch_ds(sess, orig_ds);
  free(fullpath);
  return ret;
}

/* - Get RPC handler ----------------------------------------------- */

grpc_status_code handle_get(sr_conn_ctx_t *sr_conn, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg)
{
  Gnmi__GetRequest *req = NULL;
  Gnmi__GetResponse resp = GNMI__GET_RESPONSE__INIT;
  sr_session_ctx_t *sess = NULL;
  grpc_status_code ret = GRPC_STATUS_INTERNAL;

  /* Unpack request */
  req = (Gnmi__GetRequest *)gnmi_unpack( &gnmi__get_request__descriptor, request_bb);
  if (!req) {
    *status_msg = strdup("Failed to parse GetRequest");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  /* Validate encoding */
  if (req->encoding != GNMI__ENCODING__JSON_IETF &&
      req->encoding != GNMI__ENCODING__JSON) {
    *status_msg = strdup("Unsupported encoding");
    ret = GRPC_STATUS_UNIMPLEMENTED;
    goto cleanup;
  }

  /* Validate no use_models */
  if (req->n_use_models > 0) {
    *status_msg = strdup("use_model feature unsupported");
    ret = GRPC_STATUS_UNIMPLEMENTED;
    goto cleanup;
  }

  /* Validate data_type */
  if (req->type > GNMI__GET_REQUEST__DATA_TYPE__OPERATIONAL) {
    *status_msg = strdup("Unsupported DataType");
    ret = GRPC_STATUS_UNIMPLEMENTED;
    goto cleanup;
  }

  /* Create session */
  int rc = sr_session_start(sr_conn, SR_DS_RUNNING, &sess);
  if (rc != SR_ERR_OK) {
    *status_msg = strdup("Failed to start sysrepo session");
    goto cleanup;
  }

  /* Build one notification per path */
  size_t n_paths = req->n_path;
  if (n_paths == 0)
    n_paths = 1; /* root path */

  resp.n_notification = n_paths;
  resp.notification = calloc(n_paths, sizeof(Gnmi__Notification *));
  if (!resp.notification)
    goto cleanup;

  for (size_t i = 0; i < n_paths; i++) {
    Gnmi__Notification *notif = calloc(1, sizeof(*notif));
    resp.notification[i] = notif;

    Gnmi__Path *path = (i < req->n_path) ? req->path[i] : NULL;

    /* If no path provided, use root */
    Gnmi__Path root_path = GNMI__PATH__INIT;
    root_path.origin = "rfc7951";
    if (!path)
      path = &root_path;

    ret = build_get_notification(sess, req->prefix, path, req->encoding, req->type, notif, status_msg);
    if (ret != GRPC_STATUS_OK)
      goto cleanup;
  }

  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
  ret = GRPC_STATUS_OK;

cleanup:
  /* Free notifications */
  if (resp.notification) {
    for (size_t i = 0; i < resp.n_notification; i++) {
      Gnmi__Notification *n = resp.notification[i];
      if (!n)
        continue;
      /* Free updates */
      for (size_t j = 0; j < n->n_update; j++) {
        Gnmi__Update *u = n->update[j];
        if (!u)
          continue;
        if (u->path) {
          gnmi_path_free_elems(u->path);
          free(u->path);
        }
        if (u->val) {
          if (u->val->value_case ==
              GNMI__TYPED_VALUE__VALUE_JSON_IETF_VAL)
            free(u->val->json_ietf_val.data);
          free(u->val);
        }
        free(u);
      }
      free(n->update);
      /* Free prefix if allocated */
      if (n->prefix) {
        free(n->prefix->target);
        free(n->prefix);
      }
      free(n);
    }
    free(resp.notification);
  }

  if (sess)
    sr_session_stop(sess);
  if (req)
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);

  return ret;
}

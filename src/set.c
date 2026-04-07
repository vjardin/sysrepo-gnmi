/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "set.h"
#include "confirm.h"
#include "xpath.h"
#include "encode.h"
#include "log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <sysrepo.h>
#include <libyang/libyang.h>

#include "gnmi.pb-c.h"
#include "gnmi_ext.pb-c.h"
#include "google/protobuf/duration.pb-c.h"

/* - Candidate datastore session (persistent, locked) -------------- */
/*
 * Mirrors how netopeer2 handles candidate: a persistent sysrepo session
 * is created and locked on first Set(target=candidate).  The lock keeps
 * candidate independent from running.  Edits accumulate in candidate
 * across multiple Set RPCs.  commit-candidate copies to running and
 * releases the session.  discard-candidate discards and releases.
 * sr_unlock() resets candidate to mirror running automatically.
 */

/* Auto-discard candidate after 5 minutes of inactivity */
#define CANDIDATE_IDLE_TIMEOUT_SEC  300

static sr_session_ctx_t *candidate_sess;
static struct event *candidate_idle_timer;
static struct event_base *candidate_evbase;
static void candidate_release(void);

static void candidate_idle_cb(evutil_socket_t fd, short what, void *arg)
{
  (void)fd; (void)what; (void)arg;
  if (!candidate_sess)
    return;
  gnmi_log(GNMI_LOG_WARNING, "Candidate: idle timeout, discarding uncommitted changes");
  sr_discard_changes(candidate_sess);
  candidate_release();
}

static sr_session_ctx_t *candidate_acquire(sr_conn_ctx_t *conn,
    struct event_base *evbase)
{
  if (candidate_sess) {
    /* Reset idle timer on each access */
    if (candidate_idle_timer) {
      struct timeval tv = { .tv_sec = CANDIDATE_IDLE_TIMEOUT_SEC };
      evtimer_add(candidate_idle_timer, &tv);
    }
    return candidate_sess;
  }

  int rc = sr_session_start(conn, SR_DS_CANDIDATE, &candidate_sess);
  if (rc != SR_ERR_OK)
    return NULL;

  rc = sr_lock(candidate_sess, NULL, 0);
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_ERROR, "Candidate: sr_lock failed: %s", sr_strerror(rc));
    sr_session_stop(candidate_sess);
    candidate_sess = NULL;
    return NULL;
  }

  /* Start idle timeout */
  if (evbase) {
    candidate_idle_timer = evtimer_new(evbase, candidate_idle_cb, NULL);
    struct timeval tv = { .tv_sec = CANDIDATE_IDLE_TIMEOUT_SEC };
    evtimer_add(candidate_idle_timer, &tv);
  }

  gnmi_log(GNMI_LOG_DEBUG, "Candidate: session created and locked");
  return candidate_sess;
}

static void candidate_release(void)
{
  if (!candidate_sess)
    return;
  /* Cancel idle timer */
  if (candidate_idle_timer) {
    evtimer_del(candidate_idle_timer);
    event_free(candidate_idle_timer);
    candidate_idle_timer = NULL;
  }
  /* sr_unlock resets candidate to mirror running */
  sr_unlock(candidate_sess, NULL);
  sr_session_stop(candidate_sess);
  candidate_sess = NULL;
  gnmi_log(GNMI_LOG_DEBUG, "Candidate: session unlocked and released");
}

void candidate_cleanup(void)
{
  if (candidate_sess) {
    gnmi_log(GNMI_LOG_INFO, "Candidate: releasing on shutdown");
    sr_discard_changes(candidate_sess);
  }
  candidate_release();
}

void candidate_init(struct event_base *evbase)
{
  candidate_evbase = evbase;
}

/* - Result tracking ----------------------------------------------- */

struct set_result {
  Gnmi__UpdateResult   res;
  Gnmi__Path          *path;   /* allocated, caller frees */
};

static void set_result_init(struct set_result *r, Gnmi__UpdateResult__Operation op)
{
  gnmi__update_result__init(&r->res);
  r->res.op = op;
  r->path = NULL;
}

/* - Edit batch: accumulate nodes ---------------------------------- */

struct edit_batch {
  struct lyd_node *root;
};

static void edit_batch_init(struct edit_batch *eb)
{
  eb->root = NULL;
}

static int edit_batch_push(struct edit_batch *eb, struct lyd_node *node)
{
  if (!node)
    return 0;
  if (!eb->root) {
    /* First node: duplicate as our root */
    lyd_dup_siblings(node, NULL, LYD_DUP_RECURSIVE, &eb->root);
  } else {
    /* Merge into existing tree */
    struct lyd_node *dup = NULL;
    lyd_dup_siblings(node, NULL, LYD_DUP_RECURSIVE, &dup);
    if (dup)
      lyd_insert_sibling(eb->root, dup, NULL);
  }
  return 0;
}

static void edit_batch_free(struct edit_batch *eb)
{
  if (eb->root)
    lyd_free_all(eb->root);
  eb->root = NULL;
}

/* - Process delete paths ------------------------------------------ */

static grpc_status_code
process_deletes(sr_session_ctx_t *sess, const Gnmi__SetRequest *req, const char *prefix_xpath, struct edit_batch *eb,
    struct set_result *results, size_t *n_results,
    char **err_msg)
{
  const struct ly_ctx *ctx = sr_session_acquire_context(sess);
  const struct lys_module *nc_mod =
    ly_ctx_get_module_implemented(ctx, "ietf-netconf");
  sr_session_release_context(sess);

  for (size_t i = 0; i < req->n_delete_; i++) {
    struct set_result *r = &results[(*n_results)++];
    set_result_init(r, GNMI__UPDATE_RESULT__OPERATION__DELETE);

    /* Build full xpath */
    char *fullpath = gnmi_merge_xpath(NULL, req->delete_[i], err_msg);
    if (!fullpath)
      return GRPC_STATUS_INVALID_ARGUMENT;

    /* Prepend prefix if any */
    if (prefix_xpath && prefix_xpath[0]) {
      char *combined = NULL;
      if (asprintf(&combined, "%s%s", prefix_xpath, fullpath) < 0) {
        free(fullpath);
        return GRPC_STATUS_RESOURCE_EXHAUSTED;
      }
      free(fullpath);
      fullpath = combined;
    }

    gnmi_log(GNMI_LOG_DEBUG, "Set delete: %s", fullpath);

    /* Copy path to result */
    r->path = calloc(1, sizeof(*r->path));
    gnmi__path__init(r->path);
    /* Just copy the delete path elements */
    r->res.path = r->path;

    /* Fetch the node to delete */
    sr_data_t *sr_data = NULL;
    int rc = sr_get_data(sess, fullpath, 1, 0, 0, &sr_data);
    if (rc != SR_ERR_OK || !sr_data || !sr_data->tree) {
      /* Non-existent path: silent success (gNMI spec 3.4.6) */
      free(fullpath);
      if (sr_data)
        sr_release_data(sr_data);
      continue;
    }

    /* Mark nodes for deletion and parents with ether */
    struct ly_set *set = NULL;
    lyd_find_xpath(sr_data->tree, fullpath, &set);
    if (set && set->count > 0) {
      const struct ly_ctx *lctx =
        sr_session_acquire_context(sess);
      const struct lys_module *sr_mod =
        ly_ctx_get_module_implemented(lctx, "sysrepo");

      for (uint32_t j = 0; j < set->count; j++) {
        /* Mark target with ietf-netconf:operation=remove */
        if (nc_mod)
          lyd_new_meta(lctx, set->dnodes[j], nc_mod, "operation", "remove", 0, NULL);

        /* Mark parent containers with
         * sysrepo:operation=ether so they are
         * not created if they don't exist */
        struct lyd_node *p = lyd_parent(set->dnodes[j]);
        while (p && sr_mod) {
          lyd_new_meta(lctx, p, sr_mod, "operation", "ether", 0, NULL);
          p = lyd_parent(p);
        }
      }
      sr_session_release_context(sess);
      edit_batch_push(eb, sr_data->tree);
    }
    if (set)
      ly_set_free(set, NULL);
    sr_release_data(sr_data);
    free(fullpath);
  }
  return GRPC_STATUS_OK;
}

/* - Process update/replace paths ---------------------------------- */

static grpc_status_code
process_updates(sr_session_ctx_t *sess, size_t n_updates, Gnmi__Update **updates, const char *prefix_xpath,
    const Gnmi__Path *req_prefix,
    const char *operation,
    Gnmi__UpdateResult__Operation op_type,
    struct edit_batch *eb,
    struct set_result *results, size_t *n_results,
    char **err_msg)
{
  for (size_t i = 0; i < n_updates; i++) {
    Gnmi__Update *upd = updates[i];
    struct set_result *r = &results[(*n_results)++];
    set_result_init(r, op_type);

    /* Validate */
    if (!upd->path) {
      if (err_msg)
        *err_msg = strdup("Missing path in update");
      return GRPC_STATUS_INVALID_ARGUMENT;
    }
    if (!upd->val) {
      if (err_msg)
        *err_msg = strdup("Value not set");
      return GRPC_STATUS_INVALID_ARGUMENT;
    }

    /* Check origin */
    if (gnmi_check_origin(req_prefix, upd->path, err_msg) < 0)
      return GRPC_STATUS_INVALID_ARGUMENT;

    /* Build full xpath */
    char *path_str = gnmi_to_xpath(upd->path, err_msg);
    if (!path_str)
      return GRPC_STATUS_INVALID_ARGUMENT;

    char *fullpath;
    if (prefix_xpath && prefix_xpath[0]) {
      if (asprintf(&fullpath, "%s%s", prefix_xpath, path_str) < 0) {
        free(path_str);
        if (err_msg)
          *err_msg = strdup("Out of memory");
        return GRPC_STATUS_RESOURCE_EXHAUSTED;
      }
      free(path_str);
    } else {
      fullpath = path_str;
    }

    gnmi_log(GNMI_LOG_DEBUG, "Set %s: %s", operation, fullpath);

    /* Check for wildcards in update (not allowed).
     * Exception: "/\*" means root (empty path). */
    if (strchr(fullpath, '*') && strcmp(fullpath, "/*") != 0) {
      if (err_msg)
        *err_msg = strdup(
          "Wildcards not allowed in update path");
      free(fullpath);
      return GRPC_STATUS_INVALID_ARGUMENT;
    }

    /* Copy path to result */
    r->path = calloc(1, sizeof(*r->path));
    gnmi__path__init(r->path);
    r->res.path = r->path;

    /* Decode value and attach operation metadata */
    struct lyd_node *tree = NULL;
    grpc_status_code rc = decode_update( sess, fullpath, upd->val, operation, &tree, err_msg);
    if (rc != GRPC_STATUS_OK) {
      free(fullpath);
      return rc;
    }

    if (tree) {
      edit_batch_push(eb, tree);
      lyd_free_all(tree);
    } else if (strcmp(operation, "replace") == 0 && strcmp(fullpath, "/*") == 0) {
      /* Replace root with {} = delete all config.
       * Use sr_replace_config with NULL to clear. */
      int src = sr_replace_config(sess, NULL, NULL, 0);
      if (src != SR_ERR_OK)
        gnmi_log(GNMI_LOG_WARNING, "replace_config NULL: %s", sr_strerror(src));
    }
    free(fullpath);
  }
  return GRPC_STATUS_OK;
}

/* - Set RPC handler ----------------------------------------------- */

grpc_status_code handle_set(sr_conn_ctx_t *sr_conn, const char *user, grpc_byte_buffer *request_bb, grpc_byte_buffer **response_bb,
          char **status_msg)
{
  Gnmi__SetRequest *req = NULL;
  Gnmi__SetResponse resp = GNMI__SET_RESPONSE__INIT;
  sr_session_ctx_t *sess = NULL;
  struct edit_batch eb;
  grpc_status_code ret = GRPC_STATUS_INTERNAL;
  bool use_candidate = false;
  char *prefix_xpath = NULL;

  edit_batch_init(&eb);

  /* Total operations */
  size_t max_results = 0;
  struct set_result *results = NULL;
  size_t n_results = 0;

  /* Unpack request */
  req = (Gnmi__SetRequest *)gnmi_unpack( &gnmi__set_request__descriptor, request_bb);
  if (!req) {
    *status_msg = strdup("Failed to parse SetRequest");
    return GRPC_STATUS_INVALID_ARGUMENT;
  }

  max_results = req->n_delete_ + req->n_replace + req->n_update;
  gnmi_log(GNMI_LOG_DEBUG, "Set: %zu delete, %zu replace, %zu update",
           req->n_delete_, req->n_replace, req->n_update);
  results = calloc(max_results ? max_results : 1, sizeof(*results));

  /* Candidate datastore dispatch via prefix target */
  const char *target = (req->prefix && req->prefix->target) ?
    req->prefix->target : "";
  use_candidate = (strcmp(target, "candidate") == 0);

  /* commit-candidate: copy candidate to running, then release.
   * sr_copy_config(sess, mod, src_ds, timeout) copies FROM src_ds
   * INTO the session's current DS.  So we need a running session
   * and copy FROM candidate. */
  if (strcmp(target, "commit-candidate") == 0) {
    if (!candidate_sess) {
      *status_msg = strdup("No candidate session to commit");
      ret = GRPC_STATUS_FAILED_PRECONDITION;
      goto cleanup;
    }
    /* Switch the locked candidate session to running DS, then copy
     * FROM candidate.  The lock holder can read candidate. */
    sr_session_switch_ds(candidate_sess, SR_DS_RUNNING);
    int crc = sr_copy_config(candidate_sess, NULL, SR_DS_CANDIDATE, 0);
    sr_session_switch_ds(candidate_sess, SR_DS_CANDIDATE);
    candidate_release();
    if (crc != SR_ERR_OK) {
      *status_msg = strdup(sr_strerror(crc));
      ret = GRPC_STATUS_ABORTED;
      goto cleanup;
    }
    /* Persist to startup */
    sr_session_ctx_t *su = NULL;
    if (sr_session_start(sr_conn, SR_DS_STARTUP, &su) == SR_ERR_OK) {
      sr_copy_config(su, NULL, SR_DS_RUNNING, 0);
      sr_session_stop(su);
    }
    gnmi_log(GNMI_LOG_INFO, "Candidate: committed to running");
    resp.timestamp = get_time_nanosec();
    *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
    ret = GRPC_STATUS_OK;
    goto cleanup;
  }

  /* discard-candidate: discard changes and release */
  if (strcmp(target, "discard-candidate") == 0) {
    if (candidate_sess)
      sr_discard_changes(candidate_sess);
    candidate_release();
    gnmi_log(GNMI_LOG_INFO, "Candidate: discarded");
    resp.timestamp = get_time_nanosec();
    *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
    ret = GRPC_STATUS_OK;
    goto cleanup;
  }

  /* For candidate edits, use the persistent locked session */
  int rc;
  if (use_candidate) {
    sess = candidate_acquire(sr_conn, candidate_evbase);
    if (!sess) {
      *status_msg = strdup("Failed to acquire candidate session");
      ret = GRPC_STATUS_ABORTED;
      goto cleanup;
    }
  } else {
    /* Create session (with NACM user if configured) */
    rc = gnmi_nacm_session_start_as(sr_conn, SR_DS_RUNNING, user, &sess);
    if (rc != SR_ERR_OK) {
      *status_msg = strdup("Failed to start sysrepo session");
      goto cleanup;
    }
  }

  /* Check for gnmi_ext.Commit extension (gnmic --commit-request/confirm/cancel).
   * This is the OpenConfig standard; the custom req->confirm is the legacy. */
  confirm_state_t *cs = confirm_state_get_global();
  GnmiExt__Commit *ext_commit = NULL;
  for (size_t i = 0; i < req->n_extension; i++) {
    if (req->extension[i]->ext_case == GNMI_EXT__EXTENSION__EXT_COMMIT) {
      ext_commit = req->extension[i]->commit;
      break;
    }
  }

  /* Handle confirm/cancel actions (no edits needed, just state change) */
  if (ext_commit && ext_commit->action_case == GNMI_EXT__COMMIT__ACTION_CONFIRM) {
    if (!cs || !confirm_state_waiting(cs)) {
      *status_msg = strdup("No commit pending to confirm");
      ret = GRPC_STATUS_FAILED_PRECONDITION;
      goto cleanup;
    }
    if (confirm_state_confirm(cs, sr_conn, status_msg) < 0) {
      ret = GRPC_STATUS_INTERNAL;
      goto cleanup;
    }
    gnmi_log(GNMI_LOG_INFO, "Commit-confirmed: confirmed (id=%s)", ext_commit->id);
    resp.timestamp = get_time_nanosec();
    *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
    ret = GRPC_STATUS_OK;
    goto cleanup;
  }

  if (ext_commit && ext_commit->action_case == GNMI_EXT__COMMIT__ACTION_CANCEL) {
    if (cs && confirm_state_waiting(cs)) {
      confirm_state_restore(cs);
      gnmi_log(GNMI_LOG_INFO, "Commit-confirmed: cancelled (id=%s)", ext_commit->id);
    }
    resp.timestamp = get_time_nanosec();
    *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
    ret = GRPC_STATUS_OK;
    goto cleanup;
  }

  if (ext_commit && ext_commit->action_case == GNMI_EXT__COMMIT__ACTION_SET_ROLLBACK_DURATION) {
    /* Update the rollback timer on an existing commit */
    if (!cs || !confirm_state_waiting(cs)) {
      *status_msg = strdup("No commit pending to update");
      ret = GRPC_STATUS_FAILED_PRECONDITION;
      goto cleanup;
    }
    Google__Protobuf__Duration *dur = ext_commit->set_rollback_duration->rollback_duration;
    uint32_t new_timeout = dur ? (uint32_t)dur->seconds : cs->timeout_secs;
    struct timeval tv = { .tv_sec = new_timeout };
    evtimer_add(cs->ev_timeout, &tv);
    cs->timeout_secs = new_timeout;
    gnmi_log(GNMI_LOG_INFO, "Commit-confirmed: rollback duration updated to %us", new_timeout);
    resp.timestamp = get_time_nanosec();
    *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
    ret = GRPC_STATUS_OK;
    goto cleanup;
  }

  /* Determine if this Set includes a commit-request (extension or legacy) */
  bool has_confirm = (req->confirm != NULL) ||
    (ext_commit && ext_commit->action_case == GNMI_EXT__COMMIT__ACTION_COMMIT);

  if (cs && confirm_state_waiting(cs) && !has_confirm) {
    *status_msg = strdup(
      "Previous Set awaiting Confirm - "
      "send Confirm or wait for timeout");
    ret = GRPC_STATUS_FAILED_PRECONDITION;
    goto cleanup;
  }

  /* Build prefix xpath */
  if (req->prefix && req->prefix->n_elem > 0) {
    prefix_xpath = gnmi_to_xpath(req->prefix, status_msg);
    if (!prefix_xpath) {
      ret = GRPC_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }

  /* Process deletes */
  ret = process_deletes(sess, req, prefix_xpath, &eb, results, &n_results, status_msg);
  if (ret != GRPC_STATUS_OK)
    goto cleanup;

  /* Process replaces */
  ret = process_updates(sess, req->n_replace, req->replace, prefix_xpath, req->prefix, "replace",
            GNMI__UPDATE_RESULT__OPERATION__REPLACE,
            &eb, results, &n_results, status_msg);
  if (ret != GRPC_STATUS_OK)
    goto cleanup;

  /* Process updates (merge) */
  ret = process_updates(sess, req->n_update, req->update, prefix_xpath, req->prefix, "merge",
            GNMI__UPDATE_RESULT__OPERATION__UPDATE,
            &eb, results, &n_results, status_msg);
  if (ret != GRPC_STATUS_OK)
    goto cleanup;

  /* Snapshot config BEFORE applying edits (for confirmed-commit rollback) */
  if (has_confirm && cs) {
    /* Extract timeout from extension or legacy confirm */
    uint32_t req_timeout = 0;
    if (ext_commit && ext_commit->action_case == GNMI_EXT__COMMIT__ACTION_COMMIT &&
        ext_commit->commit && ext_commit->commit->rollback_duration) {
      req_timeout = (uint32_t)ext_commit->commit->rollback_duration->seconds;
    } else if (req->confirm) {
      req_timeout = req->confirm->timeout_secs;
    }

    uint32_t min_wait = 0, timeout = 0;
    if (confirm_state_snapshot(cs, req_timeout, &min_wait, &timeout) < 0) {
      *status_msg = strdup("Failed to snapshot config");
      ret = GRPC_STATUS_INTERNAL;
      goto cleanup;
    }
    cs->set_txn_id = req->transaction_id;

    if (ext_commit)
      gnmi_log(GNMI_LOG_INFO, "Commit-confirmed: started (id=%s, timeout=%us)",
               ext_commit->id, timeout);

    /* Legacy confirm response */
    if (req->confirm) {
      resp.confirm = calloc(1, sizeof(*resp.confirm));
      gnmi__confirm_parms_response__init(resp.confirm);
      resp.confirm->min_wait_secs = min_wait;
      resp.confirm->timeout_secs = timeout;
    }
  }

  /* Apply the edit batch */
  if (eb.root) {
    rc = sr_edit_batch(sess, eb.root, "merge");
    if (rc != SR_ERR_OK) {
      const sr_error_info_t *err_info = NULL;
      sr_session_get_error(sess, &err_info);
      if (err_info && err_info->err_count > 0)
        *status_msg = strdup(err_info->err[0].message);
      else
        *status_msg = strdup(sr_strerror(rc));
      sr_discard_changes(sess);
      ret = GRPC_STATUS_ABORTED;
      goto cleanup;
    }
  }

  rc = sr_apply_changes(sess, 0);
  if (rc != SR_ERR_OK) {
    const sr_error_info_t *err_info = NULL;
    sr_session_get_error(sess, &err_info);
    if (err_info && err_info->err_count > 0)
      *status_msg = strdup(err_info->err[0].message);
    else
      *status_msg = strdup(sr_strerror(rc));
    sr_discard_changes(sess);
    ret = GRPC_STATUS_ABORTED;
    goto cleanup;
  }

  /* Persist to startup (skip for candidate and confirmed-commit) */
  if (use_candidate) {
    /* Candidate: changes stay in candidate until commit-candidate */
  } else if (has_confirm && cs) {
    /* Timer already armed by confirm_state_snapshot above */
  } else {
    /* No confirm - persist to startup immediately */
    sr_session_ctx_t *startup_sess = NULL;
    rc = sr_session_start(sr_conn, SR_DS_STARTUP, &startup_sess);
    if (rc == SR_ERR_OK) {
      sr_copy_config(startup_sess, NULL, SR_DS_RUNNING, 0);
      sr_session_stop(startup_sess);
    }
  }

  /* Build response */
  resp.timestamp = get_time_nanosec();
  if (req->prefix && req->prefix->target && req->prefix->target[0]) {
    resp.prefix = calloc(1, sizeof(*resp.prefix));
    gnmi__path__init(resp.prefix);
    resp.prefix->target = strdup(req->prefix->target);
  }

  resp.n_response = n_results;
  resp.response = calloc(n_results, sizeof(Gnmi__UpdateResult *));
  for (size_t i = 0; i < n_results; i++) {
    Gnmi__UpdateResult *ur = calloc(1, sizeof(*ur));
    gnmi__update_result__init(ur);
    ur->op = results[i].res.op;
    /* Path: copy from request for now */
    resp.response[i] = ur;
  }

  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
  ret = GRPC_STATUS_OK;

cleanup:
  /* Free response structures */
  if (resp.response) {
    for (size_t i = 0; i < resp.n_response; i++) {
      if (resp.response[i])
        free(resp.response[i]);
    }
    free(resp.response);
  }
  if (resp.prefix) {
    free(resp.prefix->target);
    free(resp.prefix);
  }
  free(resp.confirm);

  for (size_t i = 0; i < n_results; i++) {
    if (results[i].path) {
      gnmi_path_free_elems(results[i].path);
      free(results[i].path);
    }
  }
  free(results);
  free(prefix_xpath);

  edit_batch_free(&eb);

  if (sess && !use_candidate)
    sr_session_stop(sess);
  if (req)
    protobuf_c_message_free_unpacked((ProtobufCMessage *)req, NULL);

  return ret;
}

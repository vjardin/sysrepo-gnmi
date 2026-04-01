/*
 * Seed operational state for tests. Run once before test session.
 * Sets test-state data and stays alive to maintain the subscription.
 * Kill with SIGTERM when tests are done.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <sysrepo.h>

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig)
{
  (void)sig;
  running = 0;
}

static int
oper_get_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *path,
      const char *request_xpath, uint32_t request_id,
      struct lyd_node **parent, void *private_data)
{
  (void)sub_id; (void)path; (void)request_xpath;
  (void)request_id; (void)private_data;

  const struct ly_ctx *ctx = sr_session_acquire_context(session);

  lyd_new_path(NULL, ctx, "/gnmi-server-test:test-state/things[name='A']/counter", "1", 0, parent);
  lyd_new_path(*parent, ctx, "/gnmi-server-test:test-state/things[name='B']/counter", "2", 0, NULL);
  /* Empty presence container */
  lyd_new_path(*parent, ctx, "/gnmi-server-test:test-state/cargo", NULL, 0, NULL);

  sr_session_release_context(session);
  (void)module_name;
  return SR_ERR_OK;
}

static int
module_change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
     sr_event_t event, uint32_t request_id,
     void *private_data)
{
  (void)sub_id; (void)module_name; (void)request_id; (void)private_data;

  if (event == SR_EV_CHANGE) {
    /* Reject changes to custom-error with an error message */
    sr_session_set_error_message(session, "Fiddlesticks: %s", xpath);
    return SR_ERR_CALLBACK_FAILED;
  }
  return SR_ERR_OK;
}

static int
clear_stats_rpc_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
       sr_event_t event, uint32_t request_id,
       struct lyd_node *output, void *private_data)
{
  (void)sub_id; (void)event; (void)request_id; (void)private_data;

  /* Check input: first child leaf is "interface" */
  const struct lyd_node *child = lyd_child(input);
  if (child && child->schema && child->schema->nodetype == LYS_LEAF) {
    const char *val = lyd_get_value(child);
    if (val && strcmp(val, "error") == 0) {
      sr_session_set_error_message(session, "Fiddlesticks: %s", op_path);
      return SR_ERR_CALLBACK_FAILED;
    }
    if (val && strcmp(val, "timeout") == 0) {
      sleep(3); /* exceed SR_RPC_CB_TIMEOUT */
    }
  }

  /* Set output */
  lyd_new_path(output, NULL, "old-stats", "613", LYD_NEW_VAL_OUTPUT, NULL);

  return SR_ERR_OK;
}

int main(void)
{
  sr_conn_ctx_t *conn = NULL;
  sr_session_ctx_t *sess = NULL;
  sr_subscription_ctx_t *sub = NULL;

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  fprintf(stderr, "seed_oper[%d]: starting\n", getpid());
  fprintf(stderr, "seed_oper: SYSREPO_SHM_PREFIX=%s\n", getenv("SYSREPO_SHM_PREFIX") ?: "(null)");
  fprintf(stderr, "seed_oper: SYSREPO_REPOSITORY_PATH=%s\n", getenv("SYSREPO_REPOSITORY_PATH") ?: "(null)");
  fprintf(stderr, "seed_oper: GNMI_YANG_DIR=%s\n", getenv("GNMI_YANG_DIR") ?: "(null)");
  fprintf(stderr, "seed_oper: LD_LIBRARY_PATH=%s\n", getenv("LD_LIBRARY_PATH") ?: "(null)");
  fflush(stderr);

  fprintf(stderr, "seed_oper: calling sr_connect...\n");
  fflush(stderr);
  int rc = sr_connect(0, &conn);
  if (rc != SR_ERR_OK) {
    fprintf(stderr, "sr_connect: %s\n", sr_strerror(rc));
    return 1;
  }
  fprintf(stderr, "seed_oper: sr_connect OK\n");
  fflush(stderr);

  /* Install test YANG modules (after sr_connect bootstraps the repo).
   * Use batch install API to avoid segfault with individual installs. */
  {
    const char *yang_dir = getenv("GNMI_YANG_DIR");
    if (yang_dir) {
      char p1[512], p2[512];
      snprintf(p1, sizeof(p1), "%s/gnmi-server-test.yang", yang_dir);
      snprintf(p2, sizeof(p2), "%s/gnmi-server-test-wine.yang", yang_dir);
      const char *paths[] = { p1, p2, NULL };

      fprintf(stderr, "seed_oper: calling sr_install_modules...\n");
      fflush(stderr);
      rc = sr_install_modules(conn, paths, yang_dir, NULL);
      fprintf(stderr, "seed_oper: sr_install_modules returned %d (%s)\n", rc, sr_strerror(rc));
      fflush(stderr);
      if (rc != SR_ERR_OK && rc != SR_ERR_EXISTS)
        fprintf(stderr, "sr_install_modules: %s\n", sr_strerror(rc));
    }
  }

  fprintf(stderr, "seed_oper: calling sr_session_start...\n");
  fflush(stderr);
  rc = sr_session_start(conn, SR_DS_OPERATIONAL, &sess);
  if (rc != SR_ERR_OK) {
    fprintf(stderr, "sr_session_start: %s\n", sr_strerror(rc));
    goto cleanup;
  }
  fprintf(stderr, "seed_oper: sr_session_start OK\n");
  fflush(stderr);

  /* Subscribe to provide operational data */
  fprintf(stderr, "seed_oper: calling sr_oper_get_subscribe...\n");
  fflush(stderr);
  rc = sr_oper_get_subscribe(sess, "gnmi-server-test", "/gnmi-server-test:test-state",
           oper_get_cb, NULL, 0, &sub);
  if (rc != SR_ERR_OK) {
    fprintf(stderr, "sr_oper_get_subscribe: %s\n", sr_strerror(rc));
    goto cleanup;
  }

  /* Subscribe to clear-stats RPC */
  rc = sr_rpc_subscribe_tree(sess, "/gnmi-server-test:clear-stats", clear_stats_rpc_cb, NULL, 0, 0, &sub);
  if (rc != SR_ERR_OK)
    fprintf(stderr, "sr_rpc_subscribe_tree: %s\n", sr_strerror(rc));
  else
    fprintf(stderr, "seed_oper: clear-stats RPC callback registered\n");

  /* Subscribe to module changes on test2/custom-error.
   * Module-change subs must be on a RUNNING session. */
  {
    sr_session_ctx_t *run_sess = NULL;
    rc = sr_session_start(conn, SR_DS_RUNNING, &run_sess);
    if (rc == SR_ERR_OK) {
      rc = sr_module_change_subscribe(run_sess, "gnmi-server-test",
        "/gnmi-server-test:test2/custom-error",
        module_change_cb, NULL, 0,
        SR_SUBSCR_DEFAULT, &sub);
      /* Don't stop run_sess - it must stay alive for the sub */
    }
  }
  if (rc != SR_ERR_OK)
    fprintf(stderr, "sr_module_change_subscribe: %s\n", sr_strerror(rc));
  else
    fprintf(stderr, "seed_oper: module-change callback registered\n");

  fprintf(stderr, "seed_oper: operational data provider ready\n");
  fflush(stderr);

  /* Stay alive until killed */
  while (running)
    sleep(1);

cleanup:
  if (sub)
    sr_unsubscribe(sub);
  if (sess)
    sr_session_stop(sess);
  if (conn)
    sr_disconnect(conn);
  return 0;
}

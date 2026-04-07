/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * sysrepo-gnmi: gNMI server for sysrepo (pure C)
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sysrepo.h>

#include "log.h"
#include "server.h"

#define GNMI_SERVER_VERSION "0.1.0"
#define GNMI_DEFAULT_BIND   "localhost:50051"
#define SR_CONNECT_RETRIES  5
#define SR_CONNECT_DELAY_S  2

static void usage(const char *prog)
{
  fprintf(stderr, "Usage: %s [OPTIONS]\n"
    "\n"
    "gNMI server for sysrepo\n"
    "\n"
    "Options:\n"
    "  -b, --bind ADDR:PORT    Bind address (default: %s)\n"
    "  -k, --key FILE          TLS server private key PEM\n"
    "  -c, --cert FILE         TLS server certificate PEM\n"
    "  -a, --ca FILE           TLS root CA PEM (mutual TLS)\n"
    "  -u, --username USER     Username for authentication / NACM\n"
    "  -p, --password PASS     Password for authentication\n"
    "  -l, --log-level LEVEL   Log level 0-4 (default: 2=warning)\n"
    "  -f, --insecure          No TLS, no authentication\n"
    "  -S, --syslog            Log to syslog (in addition to stderr)\n"
    "  -L, --log-dir DIR       Transaction data log directory\n"
    "  -M, --max-sessions N    Max concurrent sessions (0=unlimited, default: 0)\n"
    "  -m, --max-streams N     Max subscribe streams per session (0=unlimited, default: 0)\n"
    "  -v, --version           Print version and exit\n"
    "  -h, --help              Print this help\n",
    prog, GNMI_DEFAULT_BIND);
}

static const struct option longopts[] = {
  { "bind",      required_argument, NULL, 'b' },
  { "key",       required_argument, NULL, 'k' },
  { "cert",      required_argument, NULL, 'c' },
  { "ca",        required_argument, NULL, 'a' },
  { "username",  required_argument, NULL, 'u' },
  { "password",  required_argument, NULL, 'p' },
  { "log-level", required_argument, NULL, 'l' },
  { "insecure",  no_argument,       NULL, 'f' },
  { "syslog",    no_argument,       NULL, 'S' },
  { "log-dir",       required_argument, NULL, 'L' },
  { "max-sessions",  required_argument, NULL, 'M' },
  { "max-streams",   required_argument, NULL, 'm' },
  { "version",       no_argument,       NULL, 'v' },
  { "help",          no_argument,       NULL, 'h' },
  { NULL, 0, NULL, 0 },
};

int main(int argc, char **argv)
{
  struct gnmi_config cfg = {
    .bind_addr = GNMI_DEFAULT_BIND,
    .log_level = GNMI_LOG_WARNING,
  };
  int opt;
  int enable_syslog = 0;
  const char *log_dir = NULL;

  while ((opt = getopt_long(argc, argv, "b:k:c:a:u:p:l:fSL:M:m:vh", longopts, NULL)) != -1) {
    switch (opt) {
    case 'b': cfg.bind_addr = optarg;
      break;
    case 'k': cfg.tls_key = optarg;
      break;
    case 'c': cfg.tls_cert = optarg;
      break;
    case 'a': cfg.tls_ca = optarg;
      break;
    case 'u': cfg.username = optarg;
      break;
    case 'p': cfg.password = optarg;
      break;
    case 'l': cfg.log_level = atoi(optarg);
      break;
    case 'f': cfg.insecure = 1;
      break;
    case 'S': enable_syslog = 1;
      break;
    case 'L': log_dir = optarg;
      break;
    case 'M': cfg.max_sessions = atoi(optarg);
      break;
    case 'm': cfg.max_streams_per_session = atoi(optarg);
      break;
    case 'v': printf("sysrepo-gnmi %s\n", GNMI_SERVER_VERSION);
      return EXIT_SUCCESS;
    case 'h': usage(argv[0]);
      return EXIT_SUCCESS;
    default: usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  gnmi_log_init(cfg.log_level);
  if (enable_syslog)
    gnmi_log_enable_syslog("gnmi-server");
  if (log_dir)
    gnmi_log_data_init(log_dir);

  gnmi_log(GNMI_LOG_INFO, "sysrepo-gnmi %s starting on %s", GNMI_SERVER_VERSION, cfg.bind_addr);

  if (cfg.insecure && !cfg.username)
    gnmi_log(GNMI_LOG_WARNING,
      "WARNING: running insecure without NACM user (-u) -- all access unrestricted");

  /* Connect to sysrepo with retry */
  sr_conn_ctx_t *sr_conn = NULL;
  int rc;
  for (int attempt = 1; attempt <= SR_CONNECT_RETRIES; attempt++) {
    rc = sr_connect(0, &sr_conn);
    if (rc == SR_ERR_OK)
      break;
    gnmi_log(GNMI_LOG_WARNING, "sr_connect failed: %s (attempt %d/%d, retry in %ds)",
       sr_strerror(rc), attempt, SR_CONNECT_RETRIES,
       SR_CONNECT_DELAY_S);
    if (attempt < SR_CONNECT_RETRIES)
      sleep(SR_CONNECT_DELAY_S);
  }
  if (rc != SR_ERR_OK) {
    gnmi_log(GNMI_LOG_FATAL, "sr_connect failed after %d attempts: %s",
       SR_CONNECT_RETRIES, sr_strerror(rc));
    return EXIT_FAILURE;
  }

  gnmi_log(GNMI_LOG_INFO, "Connected to sysrepo");

  /* Create and run gRPC server */
  gnmi_server_t *srv = gnmi_server_create(&cfg, sr_conn);
  if (!srv) {
    gnmi_log(GNMI_LOG_FATAL, "Failed to create server");
    sr_disconnect(sr_conn);
    return EXIT_FAILURE;
  }

  gnmi_server_run(srv);

  gnmi_log(GNMI_LOG_INFO, "Server stopped");
  gnmi_server_destroy(srv);
  sr_disconnect(sr_conn);
  return EXIT_SUCCESS;
}

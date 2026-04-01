/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#define _GNU_SOURCE

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cjson/cJSON.h>

static enum gnmi_log_level current_level = GNMI_LOG_WARNING;
static int use_syslog;
static char *txn_log_dir;

static const char *level_str[] = {
  [GNMI_LOG_FATAL]   = "FATAL",
  [GNMI_LOG_ERROR]   = "ERROR",
  [GNMI_LOG_WARNING] = "WARN ",
  [GNMI_LOG_INFO]    = "INFO ",
  [GNMI_LOG_DEBUG]   = "DEBUG",
};

static const int syslog_prio[] = {
  [GNMI_LOG_FATAL]   = LOG_CRIT,
  [GNMI_LOG_ERROR]   = LOG_ERR,
  [GNMI_LOG_WARNING] = LOG_WARNING,
  [GNMI_LOG_INFO]    = LOG_INFO,
  [GNMI_LOG_DEBUG]   = LOG_DEBUG,
};

void gnmi_log_init(int level)
{
  if (level < GNMI_LOG_FATAL)
    level = GNMI_LOG_FATAL;
  if (level > GNMI_LOG_DEBUG)
    level = GNMI_LOG_DEBUG;
  current_level = level;

  sr_log_stderr(level >= GNMI_LOG_DEBUG ? SR_LL_DBG : level >= GNMI_LOG_INFO ? SR_LL_INF :
          level >= GNMI_LOG_WARNING ? SR_LL_WRN :
          SR_LL_ERR);

  ly_log_level(level >= GNMI_LOG_DEBUG ? LY_LLDBG : level >= GNMI_LOG_INFO ? LY_LLVRB :
         level >= GNMI_LOG_WARNING ? LY_LLWRN :
         LY_LLERR);
}

void gnmi_log_enable_syslog(const char *ident)
{
  openlog(ident ? ident : "gnmi-server", LOG_PID | LOG_NDELAY, LOG_DAEMON);
  use_syslog = 1;
}

void gnmi_log(enum gnmi_log_level lvl, const char *fmt, ...)
{
  if (lvl > current_level)
    return;

  va_list ap;

  if (use_syslog) {
    va_start(ap, fmt);
    vsyslog(syslog_prio[lvl], fmt, ap);
    va_end(ap);
  }

  /* Always also write to stderr */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);

  fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
    tm.tm_hour, tm.tm_min, tm.tm_sec,
    ts.tv_nsec / 1000000,
    level_str[lvl]);

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fputc('\n', stderr);
}

void gnmi_sr_log_cb(sr_log_level_t level, const char *msg)
{
  enum gnmi_log_level lvl;

  switch (level) {
  case SR_LL_ERR: lvl = GNMI_LOG_ERROR; break;
  case SR_LL_WRN: lvl = GNMI_LOG_WARNING; break;
  case SR_LL_INF: lvl = GNMI_LOG_INFO; break;
  case SR_LL_DBG: lvl = GNMI_LOG_DEBUG; break;
  default:        lvl = GNMI_LOG_DEBUG; break;
  }
  gnmi_log(lvl, "sysrepo: %s", msg);
}

/* - Transaction data logging -------------------------------------- */

void gnmi_log_data_init(const char *log_dir)
{
  free(txn_log_dir);
  txn_log_dir = NULL;
  if (log_dir && log_dir[0]) {
    txn_log_dir = strdup(log_dir);
    char raw_dir[512];
    snprintf(raw_dir, sizeof(raw_dir), "%s/raw", log_dir);
    mkdir(log_dir, 0755);
    mkdir(raw_dir, 0755);
  }
}

void gnmi_log_data(uint64_t txn_id, const char *label, const char *data, size_t len)
{
  if (!txn_log_dir || !data || len == 0)
    return;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  char path[512];
  snprintf(path, sizeof(path), "%s/raw/%ld.%09ld-pid.%d-txn.%lu.log", txn_log_dir, ts.tv_sec, ts.tv_nsec,
     getpid(), (unsigned long)txn_id);

  cJSON *root = cJSON_CreateObject();
  if (!root)
    return;

  char ts_str[64];
  snprintf(ts_str, sizeof(ts_str), "%ld.%09ld", ts.tv_sec, ts.tv_nsec);
  cJSON_AddStringToObject(root, "timestamp", ts_str);
  cJSON_AddNumberToObject(root, "pid", getpid());
  cJSON_AddNumberToObject(root, "txn_id", (double)txn_id);
  if (label)
    cJSON_AddStringToObject(root, "label", label);

  /* Try parsing data as JSON; if not valid, store as string */
  cJSON *jdata = cJSON_ParseWithLength(data, len);
  if (jdata)
    cJSON_AddItemToObject(root, "data", jdata);
  else
    cJSON_AddStringToObject(root, "data", data);

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (out) {
    FILE *f = fopen(path, "w");
    if (f) {
      fputs(out, f);
      fputc('\n', f);
      if (fflush(f) != 0)
        gnmi_log(GNMI_LOG_WARNING, "txn log write error: %s", strerror(errno));
      fclose(f);
    }
    cJSON_free(out);
  }
}

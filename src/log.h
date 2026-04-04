/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdint.h>
#include <sysrepo.h>
#include <libyang/libyang.h>

enum gnmi_log_level {
  GNMI_LOG_FATAL = 0,
  GNMI_LOG_ERROR,
  GNMI_LOG_WARNING,
  GNMI_LOG_INFO,
  GNMI_LOG_DEBUG,
};

void gnmi_log_init(int level);

/* Enable syslog output (call after gnmi_log_init) */
void gnmi_log_enable_syslog(const char *ident);

void gnmi_log(enum gnmi_log_level lvl, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* Transaction data logging (audit trail).
 * Writes data to a file in log_dir/raw/ for traceability.
 * Pass log_dir=NULL to disable. */
void gnmi_log_data_init(const char *log_dir);
void gnmi_log_data(uint64_t txn_id, const char *label, const char *data, size_t len);

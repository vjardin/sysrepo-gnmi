/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#pragma once

#include <stdint.h>
#include <libyang/libyang.h>
#include "gnmi.pb-c.h"

/* Convert gNMI Path to XPath string. Caller must free() the result.
 * Returns NULL on error (sets *err_msg if non-NULL). */
char *gnmi_to_xpath(const Gnmi__Path *path, char **err_msg);

/* Merge prefix + path into a full XPath. Caller must free(). */
char *gnmi_merge_xpath(const Gnmi__Path *prefix, const Gnmi__Path *path, char **err_msg);

/* Convert a libyang data node to a gNMI Path (allocates PathElems).
 * Returns 0 on success, -1 on error. */
int node_to_gnmi_path(const struct lyd_node *node, Gnmi__Path *out);

/* Validate origin == "rfc7951". Returns 0 on OK, -1 on error. */
int gnmi_check_origin(const Gnmi__Path *prefix, const Gnmi__Path *path, char **err_msg);

/* Time conversion helpers */
#define NSEC_PER_SEC   1000000000ULL
#define NSEC_PER_USEC  1000ULL
#define NSEC_PER_MSEC  1000000ULL

/* Current time in nanoseconds since Unix epoch. */
uint64_t get_time_nanosec(void);

/* Free a Gnmi__Path's dynamically allocated PathElems. */
void gnmi_path_free_elems(Gnmi__Path *path);

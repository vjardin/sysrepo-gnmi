/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * Compatibility layer for libyang v3/v4 and sysrepo v3/v4.
 * Detects the version at compile time and provides unified macros.
 */

#pragma once

#include <libyang/libyang.h>
#include <libyang/version.h>

/*
 * Detect libyang major version.
 * v3.x: LY_VERSION_MAJOR == 3  (SO version)
 * v4.x: LY_VERSION_MAJOR == 4
 */
#if LY_VERSION_MAJOR >= 4
#define GNMI_LIBYANG_V4 1
#else
#define GNMI_LIBYANG_V3 1
#endif

/*
 * lyd_new_path2() changed between v3 and v4:
 *   v3: (parent, ctx, path, value, value_len,      value_type, options, new_parent, new_node)
 *   v4: (parent, ctx, path, value, value_size_bits, value_type, options, new_parent, new_node)
 * The parameter name changed from size_t value_len to uint32_t value_size_bits.
 * In practice we always pass 0, so the difference is only the type.
 */
#ifdef GNMI_LIBYANG_V3
static inline LY_ERR gnmi_lyd_new_path2(struct lyd_node *parent, const struct ly_ctx *ctx,
    const char *path, const void *value, uint32_t options,
    struct lyd_node **new_parent, struct lyd_node **new_node)
{
  return lyd_new_path2(parent, ctx, path, value, 0, LYD_ANYDATA_STRING, options, new_parent, new_node);
}
#else
static inline LY_ERR gnmi_lyd_new_path2(struct lyd_node *parent, const struct ly_ctx *ctx,
    const char *path, const void *value, uint32_t options,
    struct lyd_node **new_parent, struct lyd_node **new_node)
{
  return lyd_new_path2(parent, ctx, path, value, 0, LYD_ANYDATA_STRING, options, new_parent, new_node);
}
#endif

/*
 * lyd_parse_op() gained an extra parse_options parameter in v4:
 *   v3: (ctx, parent, in, format, data_type,                tree, op)
 *   v4: (ctx, parent, in, format, data_type, parse_options, tree, op)
 */
#ifdef GNMI_LIBYANG_V3
static inline LY_ERR gnmi_lyd_parse_op(const struct ly_ctx *ctx, struct lyd_node *parent,
    struct ly_in *in, LYD_FORMAT format, enum lyd_type data_type, uint32_t parse_options,
    struct lyd_node **tree, struct lyd_node **op)
{
  (void)parse_options; /* v3 does not support parse_options on lyd_parse_op */
  return lyd_parse_op(ctx, parent, in, format, data_type, tree, op);
}
#else
static inline LY_ERR gnmi_lyd_parse_op(const struct ly_ctx *ctx, struct lyd_node *parent,
    struct ly_in *in, LYD_FORMAT format, enum lyd_type data_type, uint32_t parse_options,
    struct lyd_node **tree, struct lyd_node **op)
{
  return lyd_parse_op(ctx, parent, in, format, data_type, parse_options, tree, op);
}
#endif

/*
 * LYD_PRINT_WITHSIBLINGS was renamed to LYD_PRINT_SIBLINGS in v4.
 * Both have the same value (0x01).
 */
#ifdef GNMI_LIBYANG_V3
#define GNMI_LYD_PRINT_SIBLINGS LYD_PRINT_WITHSIBLINGS
#else
#define GNMI_LYD_PRINT_SIBLINGS LYD_PRINT_SIBLINGS
#endif

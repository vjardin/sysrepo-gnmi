/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * Compatibility layer for libyang v2/v3/v4/v5 and sysrepo v2/v3/v4.
 * Detects the version at compile time and provides unified wrappers
 * so the rest of the codebase can use a single API.
 */

#pragma once

#include <libyang/libyang.h>
#include <libyang/version.h>

/*
 * Detect libyang major version via SO version.
 *   v2.x: LY_VERSION_MAJOR == 2 or 3  (libyang 2.x used SO 2-3)
 *   v3.x: LY_VERSION_MAJOR == 3
 *   v4.x: LY_VERSION_MAJOR == 4
 *   v5.x: LY_VERSION_MAJOR == 5
 *
 * Disambiguate v2 vs v3 using LY_PROJ_VERSION_MAJOR when available
 * (introduced in libyang 3.x; absent in 2.x).
 */
#if LY_VERSION_MAJOR >= 5
#define GNMI_LIBYANG_V5 1
#elif LY_VERSION_MAJOR >= 4
#define GNMI_LIBYANG_V4 1
#elif defined(LY_PROJ_VERSION_MAJOR) && LY_PROJ_VERSION_MAJOR >= 3
#define GNMI_LIBYANG_V3 1
#else
#define GNMI_LIBYANG_V2 1
#endif

/* Convenience: "v4 or later" / "v3 or later" for common breakpoints */
#if defined(GNMI_LIBYANG_V5) || defined(GNMI_LIBYANG_V4)
#define GNMI_LIBYANG_V4_PLUS 1
#endif
#if defined(GNMI_LIBYANG_V5) || defined(GNMI_LIBYANG_V4) || defined(GNMI_LIBYANG_V3)
#define GNMI_LIBYANG_V3_PLUS 1
#endif

/*
 * lyd_new_path2() changed across versions:
 *
 *   v2: (parent, ctx, path, value, value_len,       value_type, options, new_parent, new_node)
 *       value_len is size_t, value_type is LYD_ANYDATA_VALUETYPE
 *   v3: same as v2
 *   v4: (parent, ctx, path, value, value_size_bits,  value_type, options, new_parent, new_node)
 *       value_size_bits is uint32_t, value_type is LYD_ANYDATA_VALUETYPE
 *   v5: (parent, ctx, path, value, value_size_bits,  any_hints,  options, new_parent, new_node)
 *       value_size_bits is uint64_t, any_hints is uint32_t (LYD_ANYDATA_VALUETYPE removed)
 *
 * Our wrapper always passes 0 for size and string-type semantics.
 */
static inline LY_ERR gnmi_lyd_new_path2(struct lyd_node *parent, const struct ly_ctx *ctx,
    const char *path, const void *value, uint32_t options,
    struct lyd_node **new_parent, struct lyd_node **new_node)
{
#if defined(GNMI_LIBYANG_V5)
  return lyd_new_path2(parent, ctx, path, value, 0, 0, options, new_parent, new_node);
#elif defined(GNMI_LIBYANG_V4)
  return lyd_new_path2(parent, ctx, path, value, 0, LYD_ANYDATA_STRING, options, new_parent, new_node);
#else /* v2, v3 */
  return lyd_new_path2(parent, ctx, path, value, 0, LYD_ANYDATA_STRING, options, new_parent, new_node);
#endif
}

/*
 * lyd_parse_op() gained a parse_options parameter in v4:
 *   v2/v3: (ctx, parent, in, format, data_type,                tree, op)
 *   v4/v5: (ctx, parent, in, format, data_type, parse_options, tree, op)
 */
static inline LY_ERR gnmi_lyd_parse_op(const struct ly_ctx *ctx, struct lyd_node *parent,
    struct ly_in *in, LYD_FORMAT format, enum lyd_type data_type, uint32_t parse_options,
    struct lyd_node **tree, struct lyd_node **op)
{
#ifdef GNMI_LIBYANG_V4_PLUS
  return lyd_parse_op(ctx, parent, in, format, data_type, parse_options, tree, op);
#else /* v2, v3 */
  (void)parse_options;
  return lyd_parse_op(ctx, parent, in, format, data_type, tree, op);
#endif
}

/*
 * LYD_PRINT_WITHSIBLINGS was renamed to LYD_PRINT_SIBLINGS in v4.
 * Both have the same value (0x01).
 */
#ifdef GNMI_LIBYANG_V4_PLUS
#define GNMI_LYD_PRINT_SIBLINGS LYD_PRINT_SIBLINGS
#else
#define GNMI_LYD_PRINT_SIBLINGS LYD_PRINT_WITHSIBLINGS
#endif

/*
 * The flag to select RPC output children in lyd_new_path was renamed:
 *   v2:    LYD_NEW_PATH_OUTPUT (0x02, for lyd_new_path options)
 *   v3+:   LYD_NEW_VAL_OUTPUT  (0x01, unified lyd_new_*() options)
 */
#ifdef GNMI_LIBYANG_V2
#define GNMI_LYD_NEW_VAL_OUTPUT LYD_NEW_PATH_OUTPUT
#else
#define GNMI_LYD_NEW_VAL_OUTPUT LYD_NEW_VAL_OUTPUT
#endif

/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 */

#include "capabilities.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sysrepo.h>
#include <libyang/libyang.h>

#include "gnmi.pb-c.h"

grpc_status_code handle_capabilities(sr_conn_ctx_t *sr_conn, grpc_byte_buffer *request_bb,
             grpc_byte_buffer **response_bb,
             char **status_msg)
{
  (void)request_bb; /* CapabilityRequest has no meaningful fields */

  sr_session_ctx_t *sess = NULL;
  sr_data_t *sr_data = NULL;
  Gnmi__CapabilityResponse resp = GNMI__CAPABILITY_RESPONSE__INIT;
  grpc_status_code ret = GRPC_STATUS_INTERNAL;

  /* Create a temporary session */
  int rc = sr_session_start(sr_conn, SR_DS_RUNNING, &sess);
  if (rc != SR_ERR_OK) {
    *status_msg = strdup("Failed to start sysrepo session");
    return GRPC_STATUS_INTERNAL;
  }

  /* Get module info from sysrepo */
  rc = sr_get_module_info(sr_conn, &sr_data);
  if (rc != SR_ERR_OK) {
    *status_msg = strdup("Failed to get module info");
    goto cleanup;
  }

  /* Count modules */
  size_t n_models = 0;
  const struct ly_ctx *ly_ctx = sr_session_acquire_context(sess);
  uint32_t idx = 0;
  const struct lys_module *mod;

  while ((mod = ly_ctx_get_module_iter(ly_ctx, &idx)) != NULL) {
    if (!mod->implemented)
      continue;
    n_models++;
  }

  /* Allocate model array */
  Gnmi__ModelData **models = NULL;
  if (n_models > 0) {
    models = calloc(n_models, sizeof(*models));
    if (!models) {
      sr_session_release_context(sess);
      *status_msg = strdup("Out of memory");
      goto cleanup;
    }
  }

  /* Fill models */
  idx = 0;
  size_t i = 0;
  while ((mod = ly_ctx_get_module_iter(ly_ctx, &idx)) != NULL) {
    if (!mod->implemented)
      continue;
    if (i >= n_models)
      break;

    Gnmi__ModelData *m = calloc(1, sizeof(*m));
    gnmi__model_data__init(m);
    m->name = strdup(mod->name);
    m->organization = mod->org ? strdup(mod->org) : strdup("");
    m->version = mod->revision ? strdup(mod->revision) : strdup("");
    models[i++] = m;
  }

  sr_session_release_context(sess);

  /* Fill response */
  resp.n_supported_models = i;
  resp.supported_models = models;

  /* Supported encodings: JSON_IETF and JSON (both use the same codec) */
  Gnmi__Encoding encs[] = { GNMI__ENCODING__JSON_IETF, GNMI__ENCODING__JSON };
  resp.n_supported_encodings = sizeof(encs) / sizeof(encs[0]);
  resp.supported_encodings = encs;

  /* gNMI version from proto descriptor */
  resp.gnmi_version = "0.7.0";

  /* Pack into grpc_byte_buffer */
  *response_bb = gnmi_pack((ProtobufCMessage *)&resp);
  ret = GRPC_STATUS_OK;

cleanup:
  /* Free models */
  if (models) {
    for (size_t j = 0; j < i; j++) {
      if (models[j]) {
        free(models[j]->name);
        free(models[j]->organization);
        free(models[j]->version);
        free(models[j]);
      }
    }
    free(models);
  }
  if (sr_data)
    sr_release_data(sr_data);
  if (sess)
    sr_session_stop(sess);

  return ret;
}

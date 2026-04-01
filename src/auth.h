/*
 * Copyright (c) 2026 Vincent Jardin, Free Mobile
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * TODO: Implement TLS authentication support:
 *   - grpc_ssl_server_credentials_create() for mutual TLS
 *   - grpc_auth_metadata_processor for username/password auth
 *   - Certificate loading from PEM files
 *   - Integration with server.c credential builder
 *   See sysrepo-gnmic-design.md section 4.12 for the design.
 */

#pragma once

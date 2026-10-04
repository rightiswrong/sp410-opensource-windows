/*
 * server.h - The IPP Everywhere printer that fronts the SP410.
 *
 * Copyright 2026 The sp410-opensource-windows contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SP410_SERVER_H
#define SP410_SERVER_H

#include "config.h"

typedef struct server_s server_t;

/* Create and bind; returns NULL (and logs why) if the port cannot be bound. */
server_t *server_create(const config_t *cfg);
int       server_port(const server_t *s);
/* Serve until server_stop() is called; returns 0 on clean shutdown. */
int       server_run(server_t *s);
/* Thread-safe; may be called from a signal/service control handler thread. */
void      server_stop(server_t *s);
void      server_destroy(server_t *s);

#endif /* SP410_SERVER_H */

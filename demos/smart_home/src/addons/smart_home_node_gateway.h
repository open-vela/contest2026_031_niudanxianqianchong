/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <pthread.h>
#include <stdint.h>

#include <agent.h>
#include "cagent_addons/node_gateway.h"

typedef struct smart_home_node_gateway smart_home_node_gateway_t;

/*
 * Starts the trusted Node gateway and its catalog-mutation worker.  The
 * caller retains ownership of agent_mutex; every registry mutation is made
 * while that mutex is held, so it is serialized with agent_run().
 */
int smart_home_node_gateway_start(smart_home_node_gateway_t **gateway_out,
                                  agent_t *agent,
                                  pthread_mutex_t *agent_mutex);
/* Same as start(), but uses an explicit listen port.  The gateway must be
 * stopped before reconfiguring; the caller is responsible for not calling
 * this from a context that cannot wait for worker shutdown. */
int smart_home_node_gateway_start_with_port(
    smart_home_node_gateway_t **gateway_out,
    agent_t *agent,
    pthread_mutex_t *agent_mutex,
    uint16_t listen_port);

/* Stop transport first, drain queued unregister operations, then free state. */
void smart_home_node_gateway_stop(smart_home_node_gateway_t **gateway_ptr);

uint16_t smart_home_node_gateway_port(const smart_home_node_gateway_t *gateway);

/* Copies the Node registry for read-only UI presentation. */
size_t smart_home_node_gateway_list(const smart_home_node_gateway_t *gateway,
                                    caddons_node_info_t *nodes,
                                    size_t capacity,
                                    uint32_t *revision_out);

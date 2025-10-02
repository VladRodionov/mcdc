/*
 * Copyright (c) 2025 Vladimir Rodionov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * mcz_cmd.h - Declarations for custom MCZ commands in memcached.
 *
 * This header defines prototypes and constants for the ASCII and binary
 * command extensions used by the MCZ module. These include "mcz stats",
 * "mcz ns", and "mcz config", which allow clients to query runtime
 * statistics, list active namespaces, and view current configuration.
 *
 * Responsibilities:
 *   - Declare command opcodes for the binary protocol.
 *   - Provide function prototypes for ASCII command dispatcher and
 *     binary protocol handlers.
 *   - Export accessors to retrieve runtime configuration for serialization.
 *
 * Naming:
 *   All exported functions start with "process_mcz_" and operate on
 *   memcached connection objects. Internal helpers remain in mcz_cmd.c.
 */
#pragma once
#include "memcached.h"     /* conn, out_string, write_* */
#include "proto_text.h"
#include "protocol_binary.h"  /* for protocol headers */

#define PROTOCOL_BINARY_CMD_MCZ_STATS 0xE1
#define PROTOCOL_BINARY_CMD_MCZ_NS 0xE2
#define PROTOCOL_BINARY_CMD_MCZ_CFG 0xE3

#define PROTOCOL_BINARY_CMD_MCZ_SAMPLER 0xE4

/* Binary: MCZ_STATS 0xE1 */
void process_mcz_stats_bin(conn *c);

/* Binary: MCZ_NS (0xE2) */
void process_mcz_ns_bin(conn *c);

/* Binary: MCZ_CFG (0xE3) */
void process_mcz_cfg_bin(conn *c);

/* Binary: MCZ_STATS 0xE4 */
void process_mcz_sampler_bin(conn *c);

/* Ascii handler */
void process_mcz_command_ascii(conn *c, token_t *tokens, const size_t ntokens);

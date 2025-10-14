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
 * This header defines prototypes and constants for the text
 * command extensions used by the MCZ module. These include "mcz stats",
 * "mcz ns", "mcz config" and "mcz sampler", which allow clients to query runtime
 * statistics, list active namespaces, view current configuration and control data sampling.
 *
 * Responsibilities:
 *   - Declare command opcodes for the binary protocol.
 *   - Provide function prototypes for ASCII command dispatcher
 *   - Export accessors to retrieve runtime configuration for serialization.
 *
 */
#pragma once
#include "memcached.h"     /* conn, out_string, write_* */
#include "proto_text.h"
#include "protocol_binary.h"  /* for protocol headers */

#ifdef __cplusplus
extern "C" {
#endif

/* Ascii handler */
void process_mcz_command_ascii(conn *c, token_t *tokens, const size_t ntokens);

#ifdef __cplusplus
}
#endif

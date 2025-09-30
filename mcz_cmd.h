/* mcz_cmd.h */
#pragma once
#include "memcached.h"     /* conn, out_string, write_* */
#include "proto_text.h"
#include "protocol_binary.h"  /* for protocol headers */

#define PROTOCOL_BINARY_CMD_MCZ_STATS 0xE1
#define PROTOCOL_BINARY_CMD_MCZ_NS 0xE2


/* Binary: MCZ_STATS 0xE1 */
void process_mcz_stats_bin(conn *c);

/* Binary: MCZ_NS (0xE2) */
void process_mcz_ns_bin(conn *c);

/* Ascii handler */
void process_mcz_command_ascii(conn *c, token_t *tokens, const size_t ntokens);

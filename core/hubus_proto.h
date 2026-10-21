/*
 * HUBUS protocol constants shared by master, slave, and tests.
 * See docs/MESSAGE_FORMAT.md and docs/PROTOCOL.md.
 */
#ifndef HUBUS_PROTO_H
#define HUBUS_PROTO_H

#include <stdint.h>

#define HUBUS_PROTOCOL_VERSION 0u

#define HUBUS_MCID_BITS 6u
#define HUBUS_MCID_MIN 1u  /* MCID 0 is reserved */
#define HUBUS_MCID_MAX 63u
#define HUBUS_TARGET_LEN_EXACT 6u
#define HUBUS_MAX_PAYLOAD 31u
#define HUBUS_HEADER_BYTES 2u
#define HUBUS_MAX_FRAME_BYTES (HUBUS_HEADER_BYTES + HUBUS_MAX_PAYLOAD + 1u)

/* Arbitration: one arbitration bit plus one handoff slot per MCID bit. */
#define HUBUS_ARB_SLOTS (2u * HUBUS_MCID_BITS)

/* Master -> slave message types (2 bit). */
#define HUBUS_M_POLL_PENDING 0u
#define HUBUS_M_REQUEST_INFO 1u
/* POLL_PENDING with LEN = 1 (one ignored payload byte) is a discovery poll: every addressed slave contends. */
#define HUBUS_POLL_DISCOVER 1u
#define HUBUS_M_REQUEST_DATA 2u
#define HUBUS_M_SEND_COMMAND 3u

/* Slave -> master message types (2 bit). Both start with 1. */
#define HUBUS_S_DATA 2u
#define HUBUS_S_INFO 3u

/* SEND_COMMAND opcodes. */
#define HUBUS_OP_STATUS_CAPTURE 0x00u
#define HUBUS_OP_RESERVED_MAX 0x0Fu /* 0x01..0x0F reserved for protocol use */
#define HUBUS_OP_APP_MIN 0x10u

/* SLAVE_INFO layout. */
#define HUBUS_INFO_VERSION 0u
#define HUBUS_INFO_CAPS 1u
#define HUBUS_INFO_MAX_CMD 2u
#define HUBUS_INFO_APP 3u
#define HUBUS_CAP_STATUS_CAPTURE 0x01u

#endif /* HUBUS_PROTO_H */

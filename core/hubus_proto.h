/*
 * HUBUS protocol constants shared by master, slave, and tests.
 * See docs/spec/ (frames: docs/spec/04-frames.md).
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

/* Arbitration pattern: one arbitration bit plus one handoff slot per MCID bit. A request with TARGET_LEN = L
 * skips the first 2 * L slots (the shared prefix); the winner answers in the same transaction. */
#define HUBUS_ARB_SLOTS (2u * HUBUS_MCID_BITS)

/* Master -> slave message types (2 bit). */
#define HUBUS_M_REQUEST_STATE 0u /* current state of the slave, live from the application */
#define HUBUS_M_REQUEST_INFO 1u
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
#define HUBUS_INFO_TIMING 3u /* speed class and blind class */
#define HUBUS_INFO_APP 4u
#define HUBUS_CAP_STATUS_CAPTURE 0x01u
#define HUBUS_CAP_STATE 0x02u /* answers REQUEST_STATE */
#define HUBUS_CAP_EVENTS 0x04u /* has an event buffer: announces events and answers REQUEST_DATA */
#define HUBUS_CAP_UPDATE 0x08u  /* application that switches to its updater on UPDATE_ENTER */
#define HUBUS_CAP_UPDATER 0x10u /* this is the updater: takes the update commands */
/* Byte 3, bits 3..0: speed class, the edge response time the firmware guarantees (docs/spec/01-physical-layer.md
 * section 1.5); 0 = not declared, 1..15 = slowest..fastest. */
#define HUBUS_SPEED_CLASS_MASK 0x0Fu
#define HUBUS_SPEED_CLASS_MAX 15u
/* Byte 3, bits 7..4: blind class, how long the slave may ignore the bus after a command addressed to it
 * (docs/spec/07-slave.md section 7.8); 0 = never blind, 1..15 = at most 250 us * 2^(class - 1). */
#define HUBUS_BLIND_CLASS_SHIFT 4u
#define HUBUS_BLIND_CLASS_MAX 15u
#define HUBUS_BLIND_CLASS(info3) ((uint8_t)((uint8_t)(info3) >> HUBUS_BLIND_CLASS_SHIFT))
/* Upper bound of the blind time of a class in microseconds, 0 for class 0. */
#define HUBUS_BLIND_US(cls) ((cls) ? 250uL << ((cls) - 1u) : 0uL)

/*
 * Firmware update (docs/spec/06-operation.md section 6.8). UPDATE_ENTER goes to the application, the other opcodes
 * to the updater; every other slave ignores them as unknown reserved opcodes.
 */
#define HUBUS_OP_UPDATE_ENTER 0x01u /* key[2], exact addressing only */
#define HUBUS_OP_UPDATE_LOAD 0x02u  /* offset, data: into the page buffer */
#define HUBUS_OP_UPDATE_WRITE 0x03u /* addr_hi addr_lo crc_hi crc_lo: page buffer to flash after the STOP */
#define HUBUS_OP_UPDATE_LEAVE 0x04u /* reset after the STOP; the updater starts a valid application */
#define HUBUS_UPDATE_KEY0 0x55u     /* 'U' */
#define HUBUS_UPDATE_KEY1 0x50u     /* 'P' */

/* Application bytes of the updater's SLAVE_INFO (offsets from HUBUS_INFO_APP), multi-byte values big-endian. */
#define HUBUS_UPD_INFO_SIGNATURE 0u    /* 3 bytes: device signature */
#define HUBUS_UPD_INFO_PAGE 3u         /* flash page size in bytes, 0 = 256 */
#define HUBUS_UPD_INFO_APP_START 4u    /* 2 bytes: first byte of the application area */
#define HUBUS_UPD_INFO_APP_SIZE 6u     /* 2 bytes: size of the application area including the info page */
#define HUBUS_UPD_INFO_WRITE_MS 8u     /* time the master waits after UPDATE_WRITE, in ms */
#define HUBUS_UPD_INFO_FLAGS 9u
#define HUBUS_UPD_INFO_UPDATER 10u     /* 2 bytes: byte address of the updater */
#define HUBUS_UPD_INFO_VERSION 12u     /* updater version */
#define HUBUS_UPD_INFO_LEN 13u
#define HUBUS_UPD_FLAG_RESET_VECTOR 0x01u /* word 0 of the application area is RJMP to the updater */

/* State of the updater: result of the last UPDATE_WRITE, its address, and a counter of executed writes. */
#define HUBUS_UPD_STATE_RESULT 0u
#define HUBUS_UPD_STATE_ADDR 1u /* 2 bytes */
#define HUBUS_UPD_STATE_SEQ 3u
#define HUBUS_UPD_STATE_LEN 4u
#define HUBUS_UPD_NONE 0u       /* no write yet */
#define HUBUS_UPD_OK 1u
#define HUBUS_UPD_ERR_CRC 2u    /* page buffer does not match the CRC */
#define HUBUS_UPD_ERR_ADDR 3u   /* not a page of the application area */
#define HUBUS_UPD_ERR_VECTOR 4u /* first page without RJMP to the updater */

/* Info page: the last page of the application area; the application counts as valid if it checks out. */
#define HUBUS_UPD_MAGIC0 0x48u /* 'H' */
#define HUBUS_UPD_MAGIC1 0x42u /* 'B' */
#define HUBUS_UPD_PAGE_MAGIC 0u /* 2 bytes */
#define HUBUS_UPD_PAGE_LEN 2u   /* 2 bytes: application length from the start of the area */
#define HUBUS_UPD_PAGE_CRC 4u   /* 2 bytes: CRC-16 over the application as stored */
#define HUBUS_UPD_PAGE_ENTRY 6u /* 2 bytes: word address of the application's reset code */

#endif /* HUBUS_PROTO_H */

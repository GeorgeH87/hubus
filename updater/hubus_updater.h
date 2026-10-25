/*
 * HUBUS updater: writes a new application into flash over the bus (docs/spec/06-operation.md section 6.8).
 *
 * The protocol part is hardware independent and runs on the device and in the host tests. It is a slave built with
 * HUBUS_UPDATER (capability bit 4), without event buffer, with a state that reports the result of the last page
 * write. It keeps one page buffer:
 *
 *   UPDATE_LOAD  offset data...         copy into the page buffer
 *   UPDATE_WRITE addr_hi addr_lo crc16  write the page buffer to flash after the STOP
 *   UPDATE_LEAVE                        reset after the STOP; a valid application is started
 *
 * Flash writes and the reset wait for the STOP of their transaction (hubus_updater_poll()); the master keeps the
 * bus quiet for the write time the updater announces, because a flash write halts the CPU on classic tinyAVR.
 *
 * The platform provides hubus_upd_flash_read(), hubus_upd_flash_write(), and hubus_upd_reset(), and the layout
 * below.
 */
#ifndef HUBUS_UPDATER_H
#define HUBUS_UPDATER_H

#include <stdint.h>

#include "hubus_slave_core.h"

#if !HUBUS_UPDATER || HUBUS_ENABLE_EVENTS || !HUBUS_ENABLE_STATE
#error "The updater core is built with HUBUS_UPDATER=1, HUBUS_ENABLE_EVENTS=0, HUBUS_ENABLE_STATE=1"
#endif

/* ---- flash layout ------------------------------------------------------------------------------------------- */

/* Byte address of the updater; the application area ends there. */
#ifndef HUBUS_UPD_ADDR
#error "Define HUBUS_UPD_ADDR, the byte address of the updater in flash"
#endif
#ifndef HUBUS_UPD_PAGE_SIZE
#ifdef SPM_PAGESIZE
#define HUBUS_UPD_PAGE_SIZE SPM_PAGESIZE
#else
#define HUBUS_UPD_PAGE_SIZE 64u
#endif
#endif
#ifndef HUBUS_UPD_APP_START
#define HUBUS_UPD_APP_START 0u
#endif
/* Application area including the info page, its last page. */
#define HUBUS_UPD_APP_SIZE ((uint16_t)(HUBUS_UPD_ADDR - HUBUS_UPD_APP_START))
#define HUBUS_UPD_INFO_ADDR ((uint16_t)(HUBUS_UPD_APP_START + HUBUS_UPD_APP_SIZE - HUBUS_UPD_PAGE_SIZE))
/* Time the master waits after UPDATE_WRITE: page erase and page write, 4.5 ms each on classic tinyAVR. */
#ifndef HUBUS_UPD_WRITE_MS
#define HUBUS_UPD_WRITE_MS 10u
#endif
#define HUBUS_UPD_VERSION 1u

/* Classic tinyAVR: the vector table at address 0 belongs to the application, and its reset vector jumps to the
 * updater (12-bit RJMP; wraps around on 8 KB devices). */
#ifndef HUBUS_UPD_FLAGS
#define HUBUS_UPD_FLAGS HUBUS_UPD_FLAG_RESET_VECTOR
#endif
#define HUBUS_UPD_RESET_RJMP ((uint16_t)(0xC000u | ((HUBUS_UPD_ADDR / 2u - 1u) & 0x0FFFu)))

#if (HUBUS_UPD_FLAGS & HUBUS_UPD_FLAG_RESET_VECTOR) && HUBUS_UPD_APP_START != 0
#error "The reset vector rule needs the application area at address 0"
#endif
#if HUBUS_UPD_ADDR % HUBUS_UPD_PAGE_SIZE || HUBUS_UPD_APP_START % HUBUS_UPD_PAGE_SIZE
#error "Updater and application area must start on a page boundary"
#endif

/* Device signature for the master's image check. */
#ifndef HUBUS_UPD_SIGNATURE
#ifdef SIGNATURE_0
#define HUBUS_UPD_SIGNATURE SIGNATURE_0, SIGNATURE_1, SIGNATURE_2
#else
#define HUBUS_UPD_SIGNATURE 0x1E, 0x93, 0x0B /* ATtiny85, for the host tests */
#endif
#endif

/* ---- platform ----------------------------------------------------------------------------------------------- */

uint8_t hubus_upd_flash_read(uint16_t addr);
void hubus_upd_flash_write(uint16_t addr, const uint8_t *page); /* erase and write one page */
void hubus_upd_reset(void);                                     /* does not return on a device */

/* ---- updater ------------------------------------------------------------------------------------------------ */

#define HUBUS_UPD_JOB_NONE 0u
#define HUBUS_UPD_JOB_WRITE 1u
#define HUBUS_UPD_JOB_LEAVE 2u

/* The protocol core of the updater; the caller feeds it the pin levels (hubus_core_pins). One per device. */
extern hubus_slave_core_t hubus_updater_core;

/* Starts the protocol engine with the given MCID and the current pin levels. Returns 0 for an invalid MCID. */
uint8_t hubus_updater_init(uint8_t mcid, uint8_t clk, uint8_t data);

/* Runs a pending page write or reset once the transaction has ended. Call it from the main loop. */
void hubus_updater_poll(void);

/* 1 if the info page describes an application whose CRC matches; `*entry` gets its word address. */
uint8_t hubus_updater_app_valid(uint16_t *entry);

#endif /* HUBUS_UPDATER_H */

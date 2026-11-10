/*
 * Firmware update of a slave through its updater (docs/spec/06-operation.md section 6.8).
 *
 * hubus_master_update() switches the slave to its updater (UPDATE_ENTER), checks the image against what the
 * updater announces, invalidates the old application, writes the image page by page with a CRC-16 per page and a
 * readback of the result, writes the info page last, and starts the new application (UPDATE_LEAVE). It owns the
 * bus while it runs: no other transactions, events wait. A failed update leaves the slave in its updater, from
 * where the update can be repeated.
 *
 * hubus_master_set_mcid() gives a slave another MCID through its updater (UPDATE_SET_MCID), if the updater offers it.
 *
 * hubus_ihex_parse() turns an Intel HEX file (avr-objcopy -O ihex) into an image.
 */
#ifndef HUBUS_UPDATE_H
#define HUBUS_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_master.h"
#include "hubus_module.h"

typedef struct {
    const uint8_t *data; /* application image, from the start of the application area (address 0 on classic tinyAVR) */
    uint32_t len;
    uint8_t signature[3]; /* expected device signature; all zero: not checked */
    /* The modules of the master; may be NULL. With them an application whose module has no update support is told apart from
     * a slave without an updater, and gets no UPDATE_ENTER. */
    const hubus_module_t *const *modules;
    size_t n_modules;
} hubus_update_image_t;

typedef enum {
    HUBUS_UPD_REASON_NONE = 0,
    HUBUS_UPD_REASON_NO_UPDATER,    /* the slave did not switch to its updater: none installed, or no update support */
    HUBUS_UPD_REASON_NO_SUPPORT,    /* its module says that this application has no update support; nothing was sent */
    HUBUS_UPD_REASON_BAD_INFO,      /* the updater's SLAVE_INFO or state is not usable */
    HUBUS_UPD_REASON_SIGNATURE,     /* the device signature differs from the image's */
    HUBUS_UPD_REASON_TOO_LARGE,     /* empty image, or larger than the application area without the info page */
    HUBUS_UPD_REASON_BAD_IMAGE,     /* the image does not start with RJMP (reset vector rule) */
    HUBUS_UPD_REASON_WRITE,         /* a page could not be written within the attempts */
    HUBUS_UPD_REASON_NOT_STARTED,   /* after UPDATE_LEAVE the updater answers again: the application is not valid */
    HUBUS_UPD_REASON_MCID_TAKEN,    /* set_mcid: another slave answers to the new MCID already; nothing was sent */
    HUBUS_UPD_REASON_NO_SET_MCID,   /* set_mcid: the updater does not take UPDATE_SET_MCID; the slave is left as it was */
    HUBUS_UPD_REASON_MCID_NOT_SET,  /* set_mcid: the updater did not answer to the new MCID within the attempts */
    HUBUS_UPD_REASON_MCID_NOT_KEPT  /* set_mcid: the application takes its MCID from elsewhere; the old one was restored */
} hubus_update_reason_t;

typedef struct {
    hubus_update_reason_t reason;
    uint16_t page;        /* HUBUS_UPD_REASON_WRITE: address of the page */
    uint8_t last_result;  /* last write result the updater reported (HUBUS_UPD_OK, HUBUS_UPD_ERR_...) */
    uint16_t pages;       /* pages written, the invalidated and the final info page included */
    uint16_t retries;     /* repeated page writes */
} hubus_update_result_t;

const char *hubus_update_reason_str(hubus_update_reason_t r);

/* Called after every written page. */
typedef void (*hubus_update_progress_fn)(void *ctx, uint16_t done, uint16_t total);

/*
 * Updates slave `mcid` with `img`. Returns HUBUS_OK when the new application answers, HUBUS_ERR_UPDATE with the
 * reason in `res`, or the bus error that stopped it. `progress` may be NULL; `res` may be NULL.
 */
hubus_status_t hubus_master_update(hubus_master_t *m, uint8_t mcid, const hubus_update_image_t *img,
                                   hubus_update_progress_fn progress, void *ctx, hubus_update_result_t *res);

/*
 * Changes the MCID of slave `mcid` to `new_mcid` (docs/spec/06-operation.md section 6.8, UPDATE_SET_MCID):
 *
 *   1. refuses a new MCID that answers already (REQUEST_INFO): HUBUS_UPD_REASON_MCID_TAKEN
 *   2. switches the application to its updater (UPDATE_ENTER), as hubus_master_update() does; `modules` (may be NULL)
 *      tell an application without update support (HUBUS_UPD_REASON_NO_SUPPORT)
 *   3. checks the flag of the updater (HUBUS_UPD_REASON_NO_SET_MCID), sends UPDATE_SET_MCID, and repeats it until the
 *      updater answers to the new MCID
 *   4. starts the application again (UPDATE_LEAVE) and expects it under the new MCID. An application that takes its
 *      MCID from elsewhere (pins, a constant) answers under the old one: the change is undone with UPDATE_ENTER and
 *      UPDATE_LEAVE, which hand the old MCID back to the updater (HUBUS_UPD_REASON_MCID_NOT_KEPT).
 *
 * A slave that waits in its updater (no valid application) stays there, under the new MCID. Returns HUBUS_OK,
 * HUBUS_ERR_UPDATE with the reason in `res` (may be NULL), HUBUS_ERR_ARG, or the bus error that stopped it.
 */
hubus_status_t hubus_master_set_mcid(hubus_master_t *m, uint8_t mcid, uint8_t new_mcid,
                                     const hubus_module_t *const *modules, size_t n_modules, hubus_update_result_t *res);

/*
 * Parses Intel HEX text (`text`, `text_len` bytes) into `out` (`out_size` bytes, filled with 0xFF where the file
 * has no data). `*len` gets one past the highest address with data. Returns 0, or -1 for a malformed file, a bad
 * checksum, or data beyond `out_size`. Record types 00 (data), 01 (end), 02 and 04 (address extensions) are
 * understood, 03 and 05 (start address) ignored.
 */
int hubus_ihex_parse(const char *text, size_t text_len, uint8_t *out, uint32_t out_size, uint32_t *len);

#endif /* HUBUS_UPDATE_H */

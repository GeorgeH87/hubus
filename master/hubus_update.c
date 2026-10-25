#include "hubus_update.h"

#include <string.h>

#include "hubus_crc.h"

#define MAX_PAGE 256u
#define ENTER_TRIES 20u    /* REQUEST_INFO attempts until the updater answers */
#define ENTER_DELAY_MS 10u
#define LEAVE_TRIES 20u    /* attempts until the application answers: reset plus image check at boot */
#define LEAVE_DELAY_MS 50u
#define WRITE_MARGIN_MS 2u /* on top of the write time the updater announces */

typedef struct {
    hubus_master_t *m;
    uint8_t mcid;
    const hubus_update_image_t *img;
    hubus_update_result_t *res;
    uint16_t page;      /* page size */
    uint16_t app_start;
    uint16_t app_size;
    uint16_t info_addr; /* the info page, last page of the application area */
    uint8_t write_ms;
    uint8_t chunk;      /* data bytes per UPDATE_LOAD */
    uint8_t vector;     /* reset vector rule: word 0 becomes RJMP to the updater */
    uint16_t rjmp;
    uint8_t seq;        /* write counter the updater reported last */
    uint8_t attempts;
} upd_t;

const char *hubus_update_reason_str(hubus_update_reason_t r)
{
    switch (r) {
    case HUBUS_UPD_REASON_NONE: return "ok";
    case HUBUS_UPD_REASON_NOT_SUPPORTED: return "slave has no firmware update";
    case HUBUS_UPD_REASON_NO_UPDATER: return "slave did not switch to its updater";
    case HUBUS_UPD_REASON_BAD_INFO: return "unusable updater info or state";
    case HUBUS_UPD_REASON_SIGNATURE: return "device signature does not match the image";
    case HUBUS_UPD_REASON_TOO_LARGE: return "image empty or larger than the application area";
    case HUBUS_UPD_REASON_BAD_IMAGE: return "image does not start with RJMP";
    case HUBUS_UPD_REASON_WRITE: return "page write failed";
    case HUBUS_UPD_REASON_NOT_STARTED: return "new application did not start";
    }
    return "unknown";
}

static void delay_ms(hubus_master_t *m, uint32_t ms)
{
    m->hal.delay_ns(m->hal.ctx, ms * 1000000u);
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static hubus_status_t fail(upd_t *u, hubus_update_reason_t reason)
{
    u->res->reason = reason;
    return HUBUS_ERR_UPDATE;
}

/* Waits until the slave answers REQUEST_INFO as updater (`updater` 1) or as application (0). */
static hubus_status_t wait_info(upd_t *u, uint8_t updater, unsigned tries, uint32_t delay, hubus_slave_frame_t *f)
{
    unsigned i;

    for (i = 0; i < tries; i++) {
        delay_ms(u->m, delay);
        if (hubus_master_request_info(u->m, u->mcid, f) == HUBUS_OK && f->len > HUBUS_INFO_CAPS &&
            !(f->payload[HUBUS_INFO_CAPS] & HUBUS_CAP_UPDATER) == !updater) {
            return HUBUS_OK;
        }
    }
    return HUBUS_ERR_NO_RESPONSE;
}

/* Byte `i` of the image as it is stored: padded with 0xFF, word 0 redirected to the updater. */
static uint8_t image_byte(const upd_t *u, uint32_t i)
{
    if (u->vector && i < 2u) {
        return (uint8_t)(i ? u->rjmp >> 8 : u->rjmp);
    }
    return i < u->img->len ? u->img->data[i] : 0xFFu;
}

static hubus_status_t read_seq(upd_t *u, uint8_t *result, uint16_t *addr)
{
    hubus_slave_frame_t f;
    hubus_status_t r = HUBUS_ERR_NO_RESPONSE;
    uint8_t i;

    for (i = 0; i < u->attempts; i++) {
        r = hubus_master_request_state(u->m, u->mcid, &f);
        if (r == HUBUS_OK && f.len >= HUBUS_UPD_STATE_LEN) {
            u->seq = f.payload[HUBUS_UPD_STATE_SEQ];
            *result = f.payload[HUBUS_UPD_STATE_RESULT];
            *addr = be16(&f.payload[HUBUS_UPD_STATE_ADDR]);
            return HUBUS_OK;
        }
        if (r == HUBUS_ERR_LINE_FAULT) {
            break;
        }
    }
    return r == HUBUS_OK ? HUBUS_ERR_MISMATCH : r;
}

/* Loads `data` (one page) into the page buffer, writes it to `addr`, and reads the result back. */
static hubus_status_t write_page(upd_t *u, uint16_t addr, const uint8_t *data)
{
    uint8_t args[HUBUS_MAX_PAYLOAD];
    uint16_t crc = hubus_crc16(data, u->page);
    uint8_t attempt;
    hubus_status_t r;

    for (attempt = 0; attempt < u->attempts; attempt++) {
        uint8_t seq = u->seq, result;
        uint16_t off, got;

        if (attempt) {
            u->res->retries++;
        }
        for (off = 0; off < u->page; off = (uint16_t)(off + u->chunk)) {
            uint8_t n = (uint8_t)(u->page - off < u->chunk ? u->page - off : u->chunk);

            args[0] = (uint8_t)off;
            memcpy(&args[1], &data[off], n);
            r = hubus_master_send_command(u->m, HUBUS_TARGET_LEN_EXACT, u->mcid, HUBUS_OP_UPDATE_LOAD, args,
                                          (uint8_t)(n + 1u));
            if (r != HUBUS_OK) {
                return r;
            }
        }
        args[0] = (uint8_t)(addr >> 8);
        args[1] = (uint8_t)addr;
        args[2] = (uint8_t)(crc >> 8);
        args[3] = (uint8_t)crc;
        r = hubus_master_send_command(u->m, HUBUS_TARGET_LEN_EXACT, u->mcid, HUBUS_OP_UPDATE_WRITE, args, 4);
        if (r != HUBUS_OK) {
            return r;
        }
        delay_ms(u->m, (uint32_t)u->write_ms + WRITE_MARGIN_MS); /* the updater halts while it writes */
        r = read_seq(u, &result, &got);
        if (r == HUBUS_ERR_LINE_FAULT) {
            return r;
        }
        if (r == HUBUS_OK) {
            u->res->last_result = result;
            /* A lost UPDATE_WRITE leaves the counter unchanged; a lost UPDATE_LOAD fails the CRC. */
            if (u->seq != seq && result == HUBUS_UPD_OK && got == addr) {
                u->res->pages++;
                return HUBUS_OK;
            }
        }
    }
    u->res->page = addr;
    return fail(u, HUBUS_UPD_REASON_WRITE);
}

static hubus_status_t enter(upd_t *u, hubus_slave_frame_t *f)
{
    static const uint8_t key[2] = {HUBUS_UPDATE_KEY0, HUBUS_UPDATE_KEY1};
    hubus_status_t r;

    r = wait_info(u, 0, u->attempts, 0, f);
    if (r == HUBUS_OK) {
        if (!(f->payload[HUBUS_INFO_CAPS] & HUBUS_CAP_UPDATE)) {
            return fail(u, HUBUS_UPD_REASON_NOT_SUPPORTED);
        }
        r = hubus_master_send_command(u->m, HUBUS_TARGET_LEN_EXACT, u->mcid, HUBUS_OP_UPDATE_ENTER, key, 2);
        if (r != HUBUS_OK) {
            return r;
        }
    }
    /* Either the application has been asked to switch, or the slave already runs its updater. */
    if (wait_info(u, 1, r == HUBUS_OK ? ENTER_TRIES : 1u, ENTER_DELAY_MS, f) != HUBUS_OK) {
        return fail(u, HUBUS_UPD_REASON_NO_UPDATER);
    }
    return HUBUS_OK;
}

/*
 * Starts the new application. If the updater answers again, UPDATE_LEAVE was lost or the application does not
 * check out at boot; UPDATE_LEAVE is repeated within the attempts.
 */
static hubus_status_t leave(upd_t *u, hubus_slave_frame_t *f)
{
    uint8_t attempt;
    unsigned i;
    hubus_status_t r;

    for (attempt = 0; attempt < u->attempts; attempt++) {
        r = hubus_master_send_command(u->m, HUBUS_TARGET_LEN_EXACT, u->mcid, HUBUS_OP_UPDATE_LEAVE, NULL, 0);
        if (r != HUBUS_OK) {
            return r;
        }
        for (i = 0; i < LEAVE_TRIES; i++) {
            delay_ms(u->m, LEAVE_DELAY_MS);
            if (hubus_master_request_info(u->m, u->mcid, f) == HUBUS_OK && f->len > HUBUS_INFO_CAPS) {
                if (!(f->payload[HUBUS_INFO_CAPS] & HUBUS_CAP_UPDATER)) {
                    return HUBUS_OK;
                }
                break;
            }
        }
    }
    return fail(u, HUBUS_UPD_REASON_NOT_STARTED);
}

static hubus_status_t check_info(upd_t *u, const hubus_slave_frame_t *f)
{
    const uint8_t *p = &f->payload[HUBUS_INFO_APP];
    const uint8_t *sig = u->img->signature;
    uint16_t updater;
    uint32_t k;

    if (f->len < HUBUS_INFO_APP + HUBUS_UPD_INFO_LEN || f->payload[HUBUS_INFO_MAX_CMD] < 3u) {
        return fail(u, HUBUS_UPD_REASON_BAD_INFO);
    }
    if ((sig[0] | sig[1] | sig[2]) && memcmp(sig, &p[HUBUS_UPD_INFO_SIGNATURE], 3) != 0) {
        return fail(u, HUBUS_UPD_REASON_SIGNATURE);
    }
    u->page = p[HUBUS_UPD_INFO_PAGE] ? p[HUBUS_UPD_INFO_PAGE] : MAX_PAGE;
    u->app_start = be16(&p[HUBUS_UPD_INFO_APP_START]);
    u->app_size = be16(&p[HUBUS_UPD_INFO_APP_SIZE]);
    u->write_ms = p[HUBUS_UPD_INFO_WRITE_MS];
    u->vector = (uint8_t)(p[HUBUS_UPD_INFO_FLAGS] & HUBUS_UPD_FLAG_RESET_VECTOR);
    updater = be16(&p[HUBUS_UPD_INFO_UPDATER]);
    u->chunk = (uint8_t)(f->payload[HUBUS_INFO_MAX_CMD] - 2u); /* opcode and offset */
    if (u->page < 2u || u->app_size < 2u * u->page || u->app_size % u->page || u->app_start % u->page) {
        return fail(u, HUBUS_UPD_REASON_BAD_INFO);
    }
    u->info_addr = (uint16_t)(u->app_start + u->app_size - u->page);
    if (u->vector) {
        k = updater / 2u - 1u; /* RJMP offset from word 0; 12 bits, wrapping around on 8 KB devices */
        if (u->app_start != 0u || updater == 0u || k > 0x0FFFu) {
            return fail(u, HUBUS_UPD_REASON_BAD_INFO);
        }
        u->rjmp = (uint16_t)(0xC000u | k);
    }
    return HUBUS_OK;
}

hubus_status_t hubus_master_update(hubus_master_t *m, uint8_t mcid, const hubus_update_image_t *img,
                                   hubus_update_progress_fn progress, void *ctx, hubus_update_result_t *res)
{
    hubus_update_result_t dummy;
    hubus_slave_frame_t f;
    uint8_t page[MAX_PAGE];
    uint16_t entry, crc, npages, p, done = 0;
    uint32_t i;
    upd_t u;
    hubus_status_t r;
    uint8_t result;
    uint16_t addr;

    if (!res) {
        res = &dummy;
    }
    memset(res, 0, sizeof *res);
    if (mcid < HUBUS_MCID_MIN || mcid > HUBUS_MCID_MAX || !img || (img->len && !img->data)) {
        return HUBUS_ERR_ARG;
    }
    memset(&u, 0, sizeof u);
    u.m = m;
    u.mcid = mcid;
    u.img = img;
    u.res = res;
    u.attempts = (uint8_t)(m->cfg.retry_budget < 2u ? 3u : m->cfg.retry_budget + 1u);

    r = enter(&u, &f);
    if (r == HUBUS_OK) {
        r = check_info(&u, &f);
    }
    if (r != HUBUS_OK) {
        return r;
    }
    if (img->len == 0u || img->len > (uint32_t)u.app_size - u.page) {
        return fail(&u, HUBUS_UPD_REASON_TOO_LARGE);
    }
    if (u.vector) {
        uint16_t w0 = (uint16_t)(img->data[0] | (img->len > 1u ? img->data[1] << 8 : 0xFF00));
        int16_t k = (int16_t)(w0 & 0x0FFFu);

        if ((w0 & 0xF000u) != 0xC000u) {
            return fail(&u, HUBUS_UPD_REASON_BAD_IMAGE);
        }
        if (k & 0x0800) {
            k = (int16_t)(k - 0x1000);
        }
        if (k < 0) {
            return fail(&u, HUBUS_UPD_REASON_BAD_IMAGE);
        }
        entry = (uint16_t)(1 + k); /* the application's own reset target */
    } else {
        entry = (uint16_t)(u.app_start / 2u);
    }
    crc = HUBUS_CRC16_INIT;
    for (i = 0; i < img->len; i++) {
        crc = hubus_crc16_update(crc, image_byte(&u, i));
    }
    npages = (uint16_t)((img->len + u.page - 1u) / u.page);

    r = read_seq(&u, &result, &addr);
    if (r != HUBUS_OK) {
        return r == HUBUS_ERR_LINE_FAULT ? r : fail(&u, HUBUS_UPD_REASON_BAD_INFO);
    }
    /* First the old application becomes invalid, then the pages from the second one on, the first page with the
     * vector table, and the info page at the end: an interrupted update leaves the slave in its updater. */
    memset(page, 0xFF, u.page);
    r = write_page(&u, u.info_addr, page);
    for (p = 1; r == HUBUS_OK && p <= npages; p++) {
        uint16_t n = p < npages ? p : 0u;

        for (i = 0; i < u.page; i++) {
            page[i] = image_byte(&u, (uint32_t)n * u.page + i);
        }
        r = write_page(&u, (uint16_t)(u.app_start + n * u.page), page);
        if (r == HUBUS_OK && progress) {
            progress(ctx, ++done, npages);
        }
    }
    if (r == HUBUS_OK) {
        memset(page, 0xFF, u.page);
        page[HUBUS_UPD_PAGE_MAGIC] = HUBUS_UPD_MAGIC0;
        page[HUBUS_UPD_PAGE_MAGIC + 1u] = HUBUS_UPD_MAGIC1;
        page[HUBUS_UPD_PAGE_LEN] = (uint8_t)(img->len >> 8);
        page[HUBUS_UPD_PAGE_LEN + 1u] = (uint8_t)img->len;
        page[HUBUS_UPD_PAGE_CRC] = (uint8_t)(crc >> 8);
        page[HUBUS_UPD_PAGE_CRC + 1u] = (uint8_t)crc;
        page[HUBUS_UPD_PAGE_ENTRY] = (uint8_t)(entry >> 8);
        page[HUBUS_UPD_PAGE_ENTRY + 1u] = (uint8_t)entry;
        r = write_page(&u, u.info_addr, page);
    }
    if (r != HUBUS_OK) {
        return r;
    }
    return leave(&u, &f);
}

/* ---- Intel HEX ------------------------------------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int hex_byte(const char *s)
{
    int hi = hex_nibble(s[0]), lo = hi < 0 ? -1 : hex_nibble(s[1]);

    return lo < 0 ? -1 : hi << 4 | lo;
}

int hubus_ihex_parse(const char *text, size_t text_len, uint8_t *out, uint32_t out_size, uint32_t *len)
{
    const char *p = text, *end = text + text_len;
    uint32_t base = 0;
    uint8_t rec[4 + 255 + 1];

    memset(out, 0xFF, out_size);
    *len = 0;
    while (p < end) {
        int n, i, b;
        uint8_t sum = 0;
        uint32_t addr;

        if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (*p != ':' || end - p < 11) {
            return -1;
        }
        p++;
        n = hex_byte(p);
        if (n < 0 || end - p < 2 * (n + 5)) {
            return -1;
        }
        for (i = 0; i < n + 5; i++) { /* count, address, type, data, checksum */
            b = hex_byte(p + 2 * i);
            if (b < 0) {
                return -1;
            }
            rec[i] = (uint8_t)b;
            sum = (uint8_t)(sum + b);
        }
        if (sum != 0u) {
            return -1;
        }
        p += 2 * (n + 5);
        addr = (uint32_t)rec[1] << 8 | rec[2];
        switch (rec[3]) {
        case 0x00:
            addr += base;
            if (addr + (uint32_t)n > out_size) {
                return -1;
            }
            memcpy(&out[addr], &rec[4], (size_t)n);
            if (addr + (uint32_t)n > *len) {
                *len = addr + (uint32_t)n;
            }
            break;
        case 0x01:
            return 0;
        case 0x02:
            if (n != 2) {
                return -1;
            }
            base = ((uint32_t)rec[4] << 8 | rec[5]) << 4;
            break;
        case 0x04:
            if (n != 2) {
                return -1;
            }
            base = ((uint32_t)rec[4] << 8 | rec[5]) << 16;
            break;
        case 0x03:
        case 0x05:
            break;
        default:
            return -1;
        }
    }
    return 0;
}

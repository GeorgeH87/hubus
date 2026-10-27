#include "hubus_updater.h"

#include <string.h>

#include "hubus_crc.h"

#ifdef __AVR__
#define HUBUS_UPD_FLASH_CONST PROGMEM
#else
#define HUBUS_UPD_FLASH_CONST
#endif

#define HI(v) ((uint8_t)((v) >> 8))
#define LO(v) ((uint8_t)((v)&0xFFu))

/* One updater per device: plain variables give the smallest code on AVR. */
hubus_slave_core_t hubus_updater_core;
static uint8_t page[HUBUS_UPD_PAGE_SIZE];
static volatile uint8_t state[HUBUS_UPD_STATE_LEN]; /* result, address of the last write, write counter */
static uint8_t job;
static uint16_t job_addr;
static uint16_t job_crc;

static const uint8_t info[HUBUS_UPD_INFO_LEN] HUBUS_UPD_FLASH_CONST = {
    HUBUS_UPD_SIGNATURE,
    (uint8_t)HUBUS_UPD_PAGE_SIZE, /* 256 becomes 0 */
    HI(HUBUS_UPD_APP_START), LO(HUBUS_UPD_APP_START),
    HI(HUBUS_UPD_APP_SIZE), LO(HUBUS_UPD_APP_SIZE),
    HUBUS_UPD_WRITE_MS,
    HUBUS_UPD_FLAGS,
    HI(HUBUS_UPD_ADDR), LO(HUBUS_UPD_ADDR),
    HUBUS_UPD_VERSION,
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint16_t read16(uint16_t addr)
{
    uint8_t b[2];

    b[0] = hubus_upd_flash_read(addr);
    b[1] = hubus_upd_flash_read((uint16_t)(addr + 1u));
    return be16(b);
}

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    if (opcode == HUBUS_OP_UPDATE_LOAD) {
        /* A load that does not fit is dropped; the CRC of the next write then fails. */
        if (len >= 2u && (uint16_t)args[0] + (uint8_t)(len - 1u) <= HUBUS_UPD_PAGE_SIZE) {
            memcpy(&page[args[0]], &args[1], (uint8_t)(len - 1u));
        }
    } else if (opcode == HUBUS_OP_UPDATE_WRITE && len == 4u) {
        job_addr = be16(&args[0]);
        job_crc = be16(&args[2]);
        job = HUBUS_UPD_JOB_WRITE;
    } else if (opcode == HUBUS_OP_UPDATE_LEAVE && len == 0u) {
        job = HUBUS_UPD_JOB_LEAVE;
    }
}

static void write_page(void)
{
    uint16_t addr = job_addr;
    uint8_t result;

    /* unsigned wrap-around also rejects addresses below the area */
    if (addr % HUBUS_UPD_PAGE_SIZE || (uint16_t)(addr - HUBUS_UPD_APP_START) >= HUBUS_UPD_APP_SIZE) {
        result = HUBUS_UPD_ERR_ADDR;
    } else if (hubus_crc16(page, HUBUS_UPD_PAGE_SIZE) != job_crc) {
        result = HUBUS_UPD_ERR_CRC;
    }
#if HUBUS_UPD_FLAGS & HUBUS_UPD_FLAG_RESET_VECTOR
    else if (addr == 0u && (page[0] != LO(HUBUS_UPD_RESET_RJMP) || page[1] != HI(HUBUS_UPD_RESET_RJMP))) {
        result = HUBUS_UPD_ERR_VECTOR; /* the reset vector must keep pointing to the updater */
    }
#endif
    else {
        hubus_upd_flash_write(addr, page);
        result = HUBUS_UPD_OK;
    }
    state[HUBUS_UPD_STATE_RESULT] = result;
    state[HUBUS_UPD_STATE_ADDR] = HI(addr);
    state[HUBUS_UPD_STATE_ADDR + 1u] = LO(addr);
    state[HUBUS_UPD_STATE_SEQ] = (uint8_t)(state[HUBUS_UPD_STATE_SEQ] + 1u);
}

uint8_t hubus_updater_init(uint8_t mcid, uint8_t clk, uint8_t data)
{
    hubus_core_config_t cfg;

    memset((void *)state, 0, sizeof state);
    job = HUBUS_UPD_JOB_NONE;
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = info;
    cfg.info_len = sizeof info;
    cfg.state = state;
    cfg.state_len = HUBUS_UPD_STATE_LEN;
    cfg.on_command = on_command;
    return hubus_core_init(&hubus_updater_core, &cfg, clk, data);
}

void hubus_updater_poll(void)
{
    if (job == HUBUS_UPD_JOB_NONE || !hubus_core_is_idle(&hubus_updater_core)) {
        return;
    }
    if (job == HUBUS_UPD_JOB_WRITE) {
        job = HUBUS_UPD_JOB_NONE;
        write_page();
    } else {
        job = HUBUS_UPD_JOB_NONE;
        hubus_upd_reset();
    }
}

uint8_t hubus_updater_app_valid(uint16_t *entry)
{
    uint16_t len, crc, i;

    if (read16(HUBUS_UPD_INFO_ADDR + HUBUS_UPD_PAGE_MAGIC) != (uint16_t)(HUBUS_UPD_MAGIC0 << 8 | HUBUS_UPD_MAGIC1)) {
        return 0;
    }
    len = read16(HUBUS_UPD_INFO_ADDR + HUBUS_UPD_PAGE_LEN);
    *entry = read16(HUBUS_UPD_INFO_ADDR + HUBUS_UPD_PAGE_ENTRY);
    if (len == 0u || len > HUBUS_UPD_APP_SIZE - HUBUS_UPD_PAGE_SIZE ||
        (uint16_t)(*entry - HUBUS_UPD_APP_START / 2u) >= len / 2u) {
        return 0;
    }
    crc = HUBUS_CRC16_INIT;
    for (i = 0; i < len; i++) {
        crc = hubus_crc16_update(crc, hubus_upd_flash_read((uint16_t)(HUBUS_UPD_APP_START + i)));
    }
    return crc == read16(HUBUS_UPD_INFO_ADDR + HUBUS_UPD_PAGE_CRC);
}

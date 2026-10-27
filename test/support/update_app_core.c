/*
 * The slave core built as an application with firmware update (HUBUS_ENABLE_UPDATE), under other names, so that
 * test_update can run it on the same simulated bus as the updater core (HUBUS_UPDATER) and switch between the two
 * like a device does. Both are built without event buffer and with a state, which keeps `data_low` at the same
 * place in the structure, where the simulation reads it.
 */
#undef HUBUS_UPDATER
#define HUBUS_UPDATER 0
#undef HUBUS_ENABLE_UPDATE
#define HUBUS_ENABLE_UPDATE 1
#define hubus_core_init app_core_init
#define hubus_core_pins app_core_pins
#include "../../core/hubus_slave_core.c"

#include "update_app_core.h"

static hubus_slave_core_t app;
static volatile uint8_t app_state = 0x5A;

void *app_core(void)
{
    return &app;
}

uint8_t app_start(uint8_t mcid, const uint8_t *info, uint8_t info_len, uint8_t clk, uint8_t data)
{
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = info;
    cfg.info_len = info_len;
    cfg.state = &app_state;
    cfg.state_len = 1;
    return app_core_init(&app, &cfg, clk, data);
}

void app_feed(uint8_t clk, uint8_t data)
{
    app_core_pins(&app, clk, data);
}

uint8_t app_take_update_request(void)
{
    uint8_t r = hubus_core_update_requested(&app);

    app.update_req = 0;
    return r;
}

uint8_t app_mcid(void)
{
    return app.mcid;
}

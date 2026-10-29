#include "hubus_slave.h"

#include "hal/hubus_hal.h"

static hubus_slave_core_t g_core;

static inline void apply_data_drive(void)
{
    hubus_hal_data_low(hubus_core_data_low(&g_core));
}

void hubus_slave_isr(void)
{
    uint8_t clk = hubus_hal_read_clk();
    uint8_t data = hubus_hal_read_data();

    hubus_core_pins(&g_core, clk, data);
    apply_data_drive();
#if HUBUS_ENABLE_UPDATE
    if (hubus_core_update_requested(&g_core)) {
        g_core.update_req = 0;
        hubus_hal_enter_updater(g_core.mcid); /* returns only without an installed updater */
    }
#endif
}

HUBUS_HAL_ISRS()

uint8_t hubus_slave_init(const hubus_slave_config_t *cfg)
{
    uint8_t ok;

    hubus_hal_init();
    ok = hubus_core_init(&g_core, cfg, hubus_hal_read_clk(), hubus_hal_read_data());
    if (ok) {
        hubus_hal_irq_enable();
    }
    return ok;
}

uint8_t hubus_slave_blind_begin(void)
{
    hubus_hal_cli();
    if (!hubus_core_can_blind(&g_core)) {
        hubus_hal_sei();
        return 0;
    }
    return 1;
}

void hubus_slave_blind_end(void)
{
    hubus_core_resync(&g_core, hubus_hal_read_clk(), hubus_hal_read_data());
    apply_data_drive();
    hubus_hal_sei(); /* a pin change flag raised meanwhile runs the ISR, which finds the levels unchanged */
}

uint8_t hubus_slave_short_begin(void)
{
    hubus_hal_cli();
    if (!hubus_core_is_idle(&g_core) || hubus_hal_edge_pending()) {
        hubus_hal_sei();
        return 0;
    }
    return 1;
}

uint8_t hubus_slave_short_interrupted(void)
{
    return hubus_hal_edge_pending();
}

void hubus_slave_short_end(void)
{
    hubus_hal_sei(); /* the ISR handles a waiting pin change now */
}

#if HUBUS_ENABLE_EVENTS
uint8_t hubus_slave_write(const uint8_t *data, uint8_t len, uint8_t more)
{
    uint8_t r;
    HUBUS_ATOMIC_BEGIN();

    r = hubus_core_write(&g_core, data, len, more);
    apply_data_drive();
    HUBUS_ATOMIC_END();
    return r;
}

uint8_t hubus_slave_is_pending(void)
{
    return hubus_core_is_pending(&g_core);
}
#endif

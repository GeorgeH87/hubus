/**
 * HuBus Slave Library - Public API Implementation (ISR-Only)
 * 
 * All event handling driven by CLK interrupts.
 * No main-loop involvement needed beyond initialization.
 */

#include "hubus_slave.h"
#include "hubus_gpio_hal.h"
#include "hubus_buffer.h"
#include "hubus_state.h"

// Module initialization flag
static uint8_t g_hubus_initialized = 0;

void hubus_slave_init(uint8_t mcid, hubus_callback_t callback)
{
    // Initialize all sub-modules
    hubus_gpio_init();          // GPIO pins
    hubus_buffer_init();        // Buffers
    hubus_state_init(mcid, callback);  // State machine (enables PCINT ISR)
    
    g_hubus_initialized = 1;
}

uint8_t hubus_slave_get_mcid(void)
{
    hubus_state_machine_t *sm = hubus_state_get();
    return sm->mcid;
}

uint8_t hubus_slave_set_buffered_data(const uint8_t *data, uint8_t len)
{
    return hubus_buffer_set_data(data, len);
}

uint8_t hubus_slave_set_status(const uint8_t *status, uint8_t len)
{
    return hubus_buffer_set_status(status, len);
}

uint8_t hubus_slave_has_buffered_data(void)
{
    return hubus_buffer_has_data();
}

uint8_t hubus_slave_has_status(void)
{
    return hubus_buffer_has_status();
}

void hubus_slave_signal_pending(void)
{
    hubus_state_set_pending();
}

void hubus_slave_clear_pending(void)
{
    hubus_state_clear_pending();
}

uint8_t hubus_slave_is_pending(void)
{
    hubus_state_machine_t *sm = hubus_state_get();
    return sm->pending_event;
}

hubus_state_t hubus_slave_get_state(void)
{
    hubus_state_machine_t *sm = hubus_state_get();
    return sm->state;
}

const char *hubus_slave_get_state_name(void)
{
    hubus_state_machine_t *sm = hubus_state_get();
    return hubus_state_name(sm->state);
}

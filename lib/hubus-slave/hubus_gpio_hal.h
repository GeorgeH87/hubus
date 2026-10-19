/**
 * HuBus Slave Library - GPIO Hardware Abstraction Layer
 * 
 * Provides portable open-drain GPIO control for CLK and DATA lines.
 * No ISR; purely polling-based for ATtiny compatibility.
 */

#ifndef HUBUS_GPIO_HAL_H
#define HUBUS_GPIO_HAL_H

#include <stdint.h>
#include "hubus_slave_config.h"

/**
 * Initialize GPIO pins as inputs (inactive state).
 * Both CLK and DATA lines default to high-impedance (released).
 */
void hubus_gpio_init(void);

/**
 * Read CLK line state
 * @return 1 if CLK is high (pulled by external resistor), 0 if low
 */
uint8_t hubus_clk_is_high(void);

/**
 * Read CLK line state (inverse of hubus_clk_is_high)
 * @return 1 if CLK is low, 0 if high
 */
uint8_t hubus_clk_is_low(void);

/**
 * Read DATA line state
 * @return 1 if DATA is high (released), 0 if low (actively pulled)
 */
uint8_t hubus_data_is_high(void);

/**
 * Read DATA line state (inverse of hubus_data_is_high)
 * @return 1 if DATA is low, 0 if high
 */
uint8_t hubus_data_is_low(void);

/**
 * Pull DATA line low (open-drain: set DDR bit = output)
 * Actively drives the line to ground.
 */
void hubus_data_pull_low(void);

/**
 * Release DATA line (open-drain: clear DDR bit = input)
 * Allows external pullup resistor to pull line high.
 */
void hubus_data_release(void);

/**
 * Busy-wait for a short delay (sample stabilization).
 * Uses hubus_slave_config.h HUBUS_SAMPLE_DELAY_CYCLES.
 */
void hubus_gpio_sample_delay(void);

/* ====================================================================
   PCINT Setup (Interrupt-Driven Mode)
   ==================================================================== */

#if HUBUS_USE_PCINT

/**
 * Initialize Pin Change Interrupt on CLK line
 * Enables PCINT for precise bit-level synchronisation
 */
void hubus_gpio_pcint_init(void);

/**
 * Disable Pin Change Interrupt (for sleep modes, etc.)
 */
void hubus_gpio_pcint_disable(void);

/**
 * Check if CLK pin change interrupt is pending
 * (call from main loop after ISR sets flag)
 * @return  1 if CLK edge detected, 0 otherwise
 */
uint8_t hubus_gpio_clk_edge_pending(void);

/**
 * Clear CLK edge pending flag
 */
void hubus_gpio_clk_edge_clear(void);

#endif

#endif // HUBUS_GPIO_HAL_H

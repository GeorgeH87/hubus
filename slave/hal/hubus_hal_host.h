/*
 * Host HAL: pins are plain variables so that the slave library API can be
 * tested on the development PC. A test changes `hubus_host_clk` and
 * `hubus_host_data` and then calls hubus_slave_isr(), like the interrupt on
 * a real device. `hubus_host_data_low` holds the DATA drive of the slave.
 */
#ifndef HUBUS_HAL_HOST_H
#define HUBUS_HAL_HOST_H

#include <stdint.h>

extern uint8_t hubus_host_clk;
extern uint8_t hubus_host_data;     /* resolved DATA line level, 1 = high */
extern uint8_t hubus_host_data_low; /* DATA drive of the slave, 1 = pulled low */
extern uint8_t hubus_host_irq_on;
extern uint8_t hubus_host_cli;          /* 1 while the library has disabled all interrupts (blind section) */
extern uint8_t hubus_host_updater_mcid; /* MCID handed to the updater, 0 = none */

static inline void hubus_hal_init(void) { hubus_host_data_low = 0; }
static inline uint8_t hubus_hal_read_clk(void) { return hubus_host_clk; }
static inline uint8_t hubus_hal_read_data(void) { return hubus_host_data; }
static inline void hubus_hal_data_low(uint8_t low) { hubus_host_data_low = low; }
static inline void hubus_hal_irq_enable(void) { hubus_host_irq_on = 1; }
static inline void hubus_hal_enter_updater(uint8_t mcid) { hubus_host_updater_mcid = mcid; }

#define HUBUS_ATOMIC_BEGIN() ((void)0)
#define HUBUS_ATOMIC_END() ((void)0)
static inline void hubus_hal_cli(void) { hubus_host_cli = 1; }
static inline void hubus_hal_sei(void) { hubus_host_cli = 0; }
#define HUBUS_HAL_ISRS()

#endif /* HUBUS_HAL_HOST_H */

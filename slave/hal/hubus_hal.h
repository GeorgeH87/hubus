/*
 * Hardware abstraction layer selection for the slave library.
 *
 * Every HAL provides:
 *   hubus_hal_init()          configure CLK and DATA as inputs, no pull-ups
 *   hubus_hal_read_clk()      CLK level, 1 = high
 *   hubus_hal_read_data()     DATA level, 1 = high
 *   hubus_hal_data_low(low)   pull DATA low (1) or release it (0); never drives high
 *   hubus_hal_irq_enable()    enable the pin change interrupts for CLK and DATA
 *   HUBUS_HAL_ISRS()          defines the interrupt vectors; each calls hubus_slave_isr()
 *   HUBUS_ATOMIC_BEGIN/END()  critical section against the ISR
 *   hubus_hal_cli/sei()       disable / enable all interrupts (blind and short sections)
 *   hubus_hal_edge_pending()  1 if a CLK or DATA change waits for the ISR (short sections)
 *
 * The HAL is chosen from the compiler device macro (-mmcu=...):
 *   classic tinyAVR (ATtiny25/85, 24/84, 441/841, 48/88, 261/861, ...)  hubus_hal_classic.h
 *   tinyAVR 0/1/2-series (ATtiny202, 1614, 3216, ...)                    hubus_hal_modern.h
 *   anything else (host tests)                                           hubus_hal_host.h
 */
#ifndef HUBUS_HAL_H
#define HUBUS_HAL_H

#define HUBUS_CAT_(a, b) a##b
#define HUBUS_CAT(a, b) HUBUS_CAT_(a, b)
#define HUBUS_CAT3_(a, b, c) a##b##c
#define HUBUS_CAT3(a, b, c) HUBUS_CAT3_(a, b, c)

#if defined(__AVR__)
#include "../hubus_slave_config.h"
#if defined(__AVR_ARCH__) && __AVR_ARCH__ >= 100
#include "hubus_hal_modern.h"
#else
#include "hubus_hal_classic.h"
#endif
#else
#include "hubus_hal_host.h"
#endif

#endif /* HUBUS_HAL_H */

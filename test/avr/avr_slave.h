/*
 * AVR slaves on the bus simulation: real slave firmware (ELF, built with avr-gcc) on microcontrollers simulated by
 * simavr, cycle by cycle, next to the real master and host slave cores.
 *
 * Each simulated device is a slave of the bus simulation through a proxy core: the simulation reads only its
 * `data_low`, which follows the DATA bit of the DDR register (the HAL pulls DATA low by switching the pin to output).
 * The line levels go to the CLK and DATA pins of the device, so pin change interrupts, the ISR, and the HAL run as on
 * hardware. Time comes from the master's delays (hubus_sim_t.advance): all devices run in lock step up to the end of
 * each delay, and the line is settled after every DATA change of a device. A device that answers too slowly therefore
 * shows up as a protocol violation of the simulation, or as a failed transaction.
 *
 * Measurements per device: the longest ISR (cycles from the vector to the end of RETI), the longest response (cycles
 * from a pin change to the end of the ISR that handled it, including the interrupt latency and the wait for an ISR
 * already running), and the deepest stack.
 */
#ifndef AVR_SLAVE_H
#define AVR_SLAVE_H

#include <stdint.h>

#include "hubus_sim.h"

#define AVR_BUS_MAX 4u

struct avr_t;
struct avr_irq_t;

typedef struct {
    hubus_slave_core_t proxy; /* first member: the bus simulation sees the device through this core */
    struct avr_t *avr;
    struct avr_irq_t *clk_irq;
    struct avr_irq_t *data_irq;
    uint8_t data_pin;
    uint8_t clk_in;  /* CLK level last presented to the device */
    uint8_t data_in; /* DATA level last presented to the device */
    uint32_t isr_vector; /* byte address of the pin change vector */
    unsigned idx;        /* index in the bus simulation */
    uint64_t t0_ns;      /* simulation time of cycle 0 */

    /* measurements */
    unsigned isr_count;
    uint32_t isr_max_cycles;
    uint32_t response_max_cycles;
    uint16_t sp_min;     /* lowest stack pointer seen */
    uint16_t isr_sp_min; /* lowest stack pointer seen inside the ISR */
    uint16_t isr_sp_entry_max; /* highest stack pointer at an ISR entry (before the return address was pushed) */

    /* internal */
    int in_isr;
    uint64_t isr_entry;
    int edge_pending;
    uint64_t edge_cycle;   /* first pin change not yet seen by an ISR */
    uint64_t handled_edge; /* pin change the running ISR handles */
    int handling;
} avr_slave_t;

typedef struct {
    hubus_sim_t *sim;
    avr_slave_t dev[AVR_BUS_MAX];
    unsigned n;
} avr_bus_t;

/* Installs the time base on the simulation. */
void avr_bus_init(avr_bus_t *bus, hubus_sim_t *sim);

/* Loads `elf` on a new simulated `mcu` at `freq` Hz (ATtiny85: CLK = PB2, DATA = PB3) and adds it to the bus. The
 * device starts at the current simulation time. Fails the test if the firmware cannot be loaded. */
avr_slave_t *avr_bus_add(avr_bus_t *bus, const char *elf, const char *mcu, uint32_t freq);

/* Lets the bus idle for `ns`: the devices run, the master does nothing. */
void avr_bus_idle(avr_bus_t *bus, uint64_t ns);

/* Frees the simulated devices and removes the time base. */
void avr_bus_free(avr_bus_t *bus);

/* Deepest stack in bytes, measured from the end of RAM. */
unsigned avr_slave_stack_depth(const avr_slave_t *dev);

/* Duration of one cycle of the device in ns. */
uint32_t avr_slave_cycle_ns(const avr_slave_t *dev);

#endif /* AVR_SLAVE_H */

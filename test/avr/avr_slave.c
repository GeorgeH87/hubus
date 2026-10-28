#include "avr_slave.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avr_ioport.h"
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_interrupts.h"
#include "unity.h"

/* Pins and pin change vector per device: the defaults of cmake/avr-devices.cmake. */
typedef struct {
    const char *mcu;
    char port;
    uint8_t clk_pin;
    uint8_t data_pin;
    uint32_t isr_vector; /* byte address of the PCINT vector of the port */
} avr_device_t;

static const avr_device_t devices[] = {
    {"attiny25", 'B', 2, 3, 2 * 2},
    {"attiny45", 'B', 2, 3, 2 * 2},
    {"attiny85", 'B', 2, 3, 2 * 2},
};

#define OPCODE_RETI 0x9518u

/* simavr logs every firmware load; only its warnings and errors are of interest. */
static void quiet_logger(avr_t *avr, const int level, const char *format, va_list ap)
{
    (void)avr;
    if (level == LOG_ERROR || level == LOG_WARNING) {
        vfprintf(stderr, format, ap);
    }
}

static uint64_t dev_time_ns(const avr_slave_t *d)
{
    return d->t0_ns + d->avr->cycle * 1000000000ull / d->avr->frequency;
}

static uint16_t dev_sp(const avr_slave_t *d)
{
    return (uint16_t)(d->avr->data[R_SPL] | d->avr->data[R_SPH] << 8);
}

/* DDR of the DATA port written: DATA is pulled low while its pin is an output (the output latch stays 0). */
static void on_ddr(struct avr_irq_t *irq, uint32_t value, void *param)
{
    avr_slave_t *d = param;

    (void)irq;
    d->proxy.data_low = (uint8_t)((value >> d->data_pin) & 1u);
}

static void note_edge(avr_slave_t *d)
{
    if (!d->edge_pending) {
        d->edge_pending = 1;
        d->edge_cycle = d->avr->cycle;
    }
}

/* Pin handler of the proxy core: presents the line levels to the device pins. */
static void dev_pins(hubus_slave_core_t *core, uint8_t clk, uint8_t data)
{
    avr_slave_t *d = (avr_slave_t *)core;

    if (clk != d->clk_in) {
        d->clk_in = clk;
        note_edge(d);
        avr_raise_irq(d->clk_irq, clk);
    }
    if (data != d->data_in) {
        d->data_in = data;
        note_edge(d);
        avr_raise_irq(d->data_irq, data);
    }
}

/* Runs one instruction (or one cycle of sleep) and updates the measurements. */
static void dev_step(avr_slave_t *d)
{
    avr_t *avr = d->avr;
    uint16_t sp;
    int reti = 0;
    int state;

    if (avr->state == cpu_Sleeping && !avr_has_pending_interrupts(avr)) {
        avr->cycle++; /* nothing happens until a pin change: skip time cycle by cycle */
        return;
    }
    if (d->in_isr && avr->pc + 1u < avr->flashend + 1u) {
        reti = (uint16_t)(avr->flash[avr->pc] | avr->flash[avr->pc + 1u] << 8) == OPCODE_RETI;
    }
    sp = dev_sp(d);
    state = avr_run(avr);
    if (state == cpu_Done || state == cpu_Crashed) {
        char msg[96];

        snprintf(msg, sizeof msg, "simulated device stopped (state %d) at pc 0x%04x", state, (unsigned)avr->pc);
        TEST_FAIL_MESSAGE(msg);
    }
    /* The step that executes RETI may also enter the next interrupt: end the ISR first, then look for an entry.
     * Cycles of the interrupt entry itself belong to the next ISR, which is not exact to the cycle but close. */
    if (reti) {
        uint32_t cycles = (uint32_t)(avr->cycle - d->isr_entry);

        if (cycles > d->isr_max_cycles) {
            d->isr_max_cycles = cycles;
        }
        if (d->handling) {
            uint32_t response = (uint32_t)(avr->cycle - d->handled_edge);

            if (response > d->response_max_cycles) {
                d->response_max_cycles = response;
            }
        }
        d->in_isr = 0;
        d->handling = 0;
    }
    if (!d->in_isr && avr->pc == d->isr_vector) {
        d->in_isr = 1;
        d->isr_entry = avr->cycle;
        d->isr_count++;
        if (!reti && sp > d->isr_sp_entry_max) {
            d->isr_sp_entry_max = sp;
        }
        d->handling = d->edge_pending;
        d->handled_edge = d->edge_cycle;
        d->edge_pending = 0;
    }
    sp = dev_sp(d);
    if (sp < d->sp_min) {
        d->sp_min = sp;
    }
    if (d->in_isr && sp < d->isr_sp_min) {
        d->isr_sp_min = sp;
    }
}

/* Time base of the simulation: runs all devices in lock step up to `until_ns`. */
static void bus_advance(hubus_sim_t *sim, uint64_t until_ns, void *ctx)
{
    avr_bus_t *bus = ctx;

    for (;;) {
        avr_slave_t *next = NULL;
        uint64_t t_next = until_ns;
        unsigned i;

        for (i = 0; i < bus->n; i++) {
            uint64_t t = dev_time_ns(&bus->dev[i]);

            if (t < t_next) {
                t_next = t;
                next = &bus->dev[i];
            }
        }
        if (!next) {
            return;
        }
        {
            uint8_t low = next->proxy.data_low;

            dev_step(next);
            if (next->proxy.data_low != low) {
                uint64_t t = dev_time_ns(next);

                if (t > until_ns) {
                    t = until_ns;
                }
                if (t > sim->time_ns) {
                    sim->time_ns = t;
                }
                hubus_sim_settle(sim);
            }
        }
    }
}

void avr_bus_init(avr_bus_t *bus, hubus_sim_t *sim)
{
    memset(bus, 0, sizeof *bus);
    bus->sim = sim;
    sim->advance = bus_advance;
    sim->advance_ctx = bus;
}

avr_slave_t *avr_bus_add(avr_bus_t *bus, const char *elf, const char *mcu, uint32_t freq)
{
    const avr_device_t *dev = NULL;
    elf_firmware_t fw;
    avr_slave_t *d;
    avr_t *avr;
    unsigned i;

    for (i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        if (strcmp(devices[i].mcu, mcu) == 0) {
            dev = &devices[i];
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(dev, "no pin table for this device");
    TEST_ASSERT_TRUE(bus->n < AVR_BUS_MAX);

    avr_global_logger_set(quiet_logger);
    memset(&fw, 0, sizeof fw);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, elf_read_firmware(elf, &fw), elf);
    avr = avr_make_mcu_by_name(mcu);
    TEST_ASSERT_NOT_NULL_MESSAGE(avr, "simavr does not know the device");
    avr_init(avr);
    avr->log = LOG_ERROR;
    avr->frequency = freq;
    avr_load_firmware(avr, &fw);
    free(fw.flash);
    free(fw.eeprom);

    d = &bus->dev[bus->n++];
    memset(d, 0, sizeof *d);
    d->avr = avr;
    d->data_pin = dev->data_pin;
    d->isr_vector = dev->isr_vector;
    d->t0_ns = bus->sim->time_ns;
    d->sp_min = 0xFFFFu;
    d->isr_sp_min = 0xFFFFu;
    d->clk_irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(dev->port), dev->clk_pin);
    d->data_irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(dev->port), dev->data_pin);
    avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(dev->port), IOPORT_IRQ_DIRECTION_ALL), on_ddr,
                            d);
    d->clk_in = bus->sim->clk;
    d->data_in = bus->sim->data;
    avr_raise_irq(d->clk_irq, d->clk_in);
    avr_raise_irq(d->data_irq, d->data_in);

    TEST_ASSERT_TRUE(bus->sim->n_slaves < HUBUS_SIM_MAX_SLAVES);
    bus->sim->pins[bus->sim->n_slaves] = dev_pins; /* the index hubus_sim_add() assigns */
    d->idx = hubus_sim_add(bus->sim, &d->proxy);
    return d;
}

void avr_bus_idle(avr_bus_t *bus, uint64_t ns)
{
    uint64_t until = bus->sim->time_ns + ns;

    bus_advance(bus->sim, until, bus);
    bus->sim->time_ns = until;
}

void avr_bus_free(avr_bus_t *bus)
{
    unsigned i;

    for (i = 0; i < bus->n; i++) {
        avr_terminate(bus->dev[i].avr);
        free(bus->dev[i].avr);
    }
    bus->sim->advance = NULL;
    bus->sim->advance_ctx = NULL;
    bus->n = 0;
}

unsigned avr_slave_stack_depth(const avr_slave_t *dev)
{
    return dev->sp_min > dev->avr->ramend ? 0u : (unsigned)(dev->avr->ramend - dev->sp_min);
}

uint32_t avr_slave_cycle_ns(const avr_slave_t *dev)
{
    return 1000000000u / dev->avr->frequency;
}

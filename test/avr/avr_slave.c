#include "avr_slave.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avr_flash.h"
#include "avr_ioport.h"
#include "hubus_update.h"
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
    uint16_t spm_page;   /* flash page in bytes */
    uint16_t mcucr;      /* data address of MCUCR (sleep enable) */
} avr_device_t;

static const avr_device_t devices[] = {
    {"attiny25", 'B', 2, 3, 2 * 2, 32, 0x55},
    {"attiny45", 'B', 2, 3, 2 * 2, 64, 0x55},
    {"attiny85", 'B', 2, 3, 2 * 2, 64, 0x55},
};

/* SPMCSR of the ATtiny25/45/85 (data address), and its bits. simavr 1.6 declares no self-programming for these
 * devices, so SPM would do nothing; the harness adds the flash module of simavr with the register of the device. */
#define TINYX5_SPMCSR 0x57u
#define SPM_SPMEN 0
#define SPM_PGERS 1
#define SPM_PGWRT 2
#define SPM_RFLB 3

#define OPCODE_RETI 0x9518u
#define OPCODE_SLEEP 0x9588u
#define MCUCR_SE 0x20u /* sleep enable, bit 5 of MCUCR on the ATtiny25/45/85 */

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

    /* Sleep is handled here: simavr would jump ahead to its next cycle timer (up to 1000 cycles) when the device falls
     * asleep or wakes up, and the device would miss the edges of that time. */
    if (avr->state == cpu_Sleeping) {
        if (!avr_has_pending_interrupts(avr)) {
            avr->cycle++; /* nothing happens until a pin change: skip time cycle by cycle */
            return;
        }
        avr->state = cpu_Running; /* the interrupt is served after the next instruction */
    } else if (avr->state == cpu_Running && avr->pc + 1u <= avr->flashend &&
               (uint16_t)(avr->flash[avr->pc] | avr->flash[avr->pc + 1u] << 8) == OPCODE_SLEEP) {
        avr->pc += 2u;
        avr->cycle++;
        if (!avr_has_pending_interrupts(avr) && (avr->data[d->mcucr] & MCUCR_SE)) {
            avr->state = cpu_Sleeping;
        }
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

static const avr_device_t *find_device(const char *mcu)
{
    unsigned i;

    for (i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        if (strcmp(devices[i].mcu, mcu) == 0) {
            return &devices[i];
        }
    }
    TEST_FAIL_MESSAGE("no pin table for this device");
    return NULL;
}

static avr_t *make_avr(const char *mcu, uint32_t freq)
{
    avr_t *avr;

    avr_global_logger_set(quiet_logger);
    avr = avr_make_mcu_by_name(mcu);
    TEST_ASSERT_NOT_NULL_MESSAGE(avr, "simavr does not know the device");
    avr_init(avr);
    avr->log = LOG_ERROR;
    avr->frequency = freq;
    return avr;
}

/* Self-programming for a device that simavr declares without: the flash module of simavr on SPMCSR. The device has no
 * SPM interrupt. */
static void *add_selfprog(avr_t *avr, const avr_device_t *dev)
{
    avr_flash_t *f;
    avr_io_t *io;

    for (io = avr->io_port; io; io = io->next) {
        if (strcmp(io->kind, "flash") == 0) {
            return NULL; /* simavr has its own */
        }
    }
    f = calloc(1, sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    f->r_spm = TINYX5_SPMCSR;
    f->spm_pagesize = dev->spm_page;
    f->selfprgen = (avr_regbit_t)AVR_IO_REGBIT(TINYX5_SPMCSR, SPM_SPMEN);
    f->pgers = (avr_regbit_t)AVR_IO_REGBIT(TINYX5_SPMCSR, SPM_PGERS);
    f->pgwrt = (avr_regbit_t)AVR_IO_REGBIT(TINYX5_SPMCSR, SPM_PGWRT);
    f->blbset = (avr_regbit_t)AVR_IO_REGBIT(TINYX5_SPMCSR, SPM_RFLB);
    avr_flash_init(avr, f);
    return f;
}

static avr_slave_t *add_device(avr_bus_t *bus, avr_t *avr, const avr_device_t *dev)
{
    avr_slave_t *d;

    TEST_ASSERT_TRUE(bus->n < AVR_BUS_MAX);
    d = &bus->dev[bus->n++];
    memset(d, 0, sizeof *d);
    d->avr = avr;
    d->selfprog = add_selfprog(avr, dev);
    d->data_pin = dev->data_pin;
    d->isr_vector = dev->isr_vector;
    d->mcucr = dev->mcucr;
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

avr_slave_t *avr_bus_add(avr_bus_t *bus, const char *elf, const char *mcu, uint32_t freq)
{
    const avr_device_t *dev = find_device(mcu);
    elf_firmware_t fw;
    avr_t *avr;

    memset(&fw, 0, sizeof fw);
    avr_global_logger_set(quiet_logger);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, elf_read_firmware(elf, &fw), elf);
    avr = make_avr(mcu, freq);
    avr_load_firmware(avr, &fw);
    free(fw.flash);
    free(fw.eeprom);
    return add_device(bus, avr, dev);
}

avr_slave_t *avr_bus_add_flash(avr_bus_t *bus, const uint8_t *flash, uint32_t len, const char *mcu, uint32_t freq)
{
    const avr_device_t *dev = find_device(mcu);
    avr_t *avr = make_avr(mcu, freq);

    TEST_ASSERT_TRUE(len <= avr->flashend + 1u);
    memset(avr->flash, 0xFF, avr->flashend + 1u);
    memcpy(avr->flash, flash, len);
    return add_device(bus, avr, dev);
}

uint32_t avr_read_hex(const char *path, uint8_t *flash, uint32_t size)
{
    static char text[256 * 1024];
    FILE *f = fopen(path, "r");
    size_t n;
    uint32_t len = 0;

    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    n = fread(text, 1, sizeof text, f);
    fclose(f);
    TEST_ASSERT_TRUE_MESSAGE(n < sizeof text, path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubus_ihex_parse(text, n, flash, size, &len), path);
    return len;
}

void avr_slave_power_cycle(avr_slave_t *dev)
{
    avr_t *avr = dev->avr;
    uint64_t cycle = avr->cycle;

    avr_reset(avr);
    avr->cycle = cycle; /* the time base keeps running */
    /* the pins see the line levels again, which the reset of the ports forgot */
    avr_raise_irq(dev->clk_irq, !dev->clk_in);
    avr_raise_irq(dev->clk_irq, dev->clk_in);
    avr_raise_irq(dev->data_irq, !dev->data_in);
    avr_raise_irq(dev->data_irq, dev->data_in);
    dev->in_isr = 0;
    dev->handling = 0;
    dev->edge_pending = 0;
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
        free(bus->dev[i].selfprog);
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

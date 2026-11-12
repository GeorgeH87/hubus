/*
 * Updater firmware for the tinyAVR 0/1/2-series (reference: ATtiny1614), linked to address 0: the boot section.
 *
 * The device always starts in the boot section, so no reset vector rule is needed: the application area starts at the
 * end of the boot section (HUBUS_UPD_APP_START = BOOTEND * 256) with the application's own vector table, which the CPU
 * uses there by itself (CPUINT.CTRLA IVSEL = 0, the reset value). Only code in the boot section can write the
 * application area, and no code can write the boot section: the application can never damage the updater.
 *
 * It runs without interrupts: the main loop reads the pins and feeds the protocol core directly.
 *
 *   reset          start the application if the info page checks out, otherwise stay
 *   UPDATE_ENTER   the application jumps to address 0 with HUBUS_UPDATE_KEY0 in GPIOR0 and its MCID in GPIOR1
 *
 * MCID: the last EEPROM byte, as on classic tinyAVR (updater/hubus_updater_avr.c, hubus_slave_mcid_from_eeprom()).
 *
 * Clock: the reset value (20 or 16 MHz divided by 6) is too slow to poll the bus, so the updater runs from the
 * internal oscillator divided by 2 (10 or 8 MHz, within the limits at 3.3 V). It starts the application before that,
 * at the reset value the application expects.
 *
 * Fuses: BOOTEND = HUBUS_UPD_APP_START / 256, APPEND = 0 (the application code section reaches to the end of flash),
 * EESAVE recommended (a chip erase then keeps the MCID). The watchdog must not be locked on (WDTCFG with LOCK),
 * because the updater switches it off. Programmed over UPDI.
 */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/xmega.h>

#include "hal/hubus_hal.h"
#include "hubus_slave.h"
#include "hubus_updater.h"

#ifndef MAPPED_PROGMEM_START
#error "hubus_updater_modern.c is the updater of the tinyAVR 0/1/2-series; classic tinyAVR use hubus_updater_avr.c"
#endif
#if HUBUS_UPD_ADDR != 0 || HUBUS_UPD_APP_START == 0 || HUBUS_UPD_APP_START % 256
#error "The updater lies in the boot section at 0, and the application area starts at BOOTEND * 256"
#endif
#if HUBUS_UPD_APP_START + HUBUS_UPD_APP_SIZE > PROGMEM_SIZE
#error "The application area ends beyond the flash"
#endif

#ifndef HUBUS_UPDATER_MCID
#define HUBUS_UPDATER_MCID 63
#endif
/* The last EEPROM byte, read and written through the data space where the EEPROM is mapped. */
#define MCID_EEPROM (*(volatile uint8_t *)(MAPPED_EEPROM_START + HUBUS_MCID_EEPROM_ADDR))

/*
 * Start without the C runtime (linked with -nostartfiles): the updater uses no interrupts, so it needs no vector table,
 * which would take 124 of the 2 KB on devices with 16 KB and more. Word 0 jumps to .init0, behind the constants; libgcc
 * clears .bss in .init4, and .init9 starts main. The application also jumps to word 0, with its own stack pointer and
 * registers, so .init0 sets them up as after a reset.
 */
__attribute__((naked, used, section(".init0"))) static void hubus_upd_init0(void)
{
    __asm__ volatile("clr __zero_reg__");
    SREG = 0;
    SP = RAMEND;
}

/* Not inlined into the naked function below, which has no stack frame. */
__attribute__((noinline)) int main(void);

__attribute__((naked, used, section(".init9"))) static void hubus_upd_init9(void)
{
    main(); /* never returns; called from C, so that link-time optimisation keeps it */
}

__attribute__((naked, used, section(".vectors"))) static void hubus_upd_reset_vector(void)
{
    __asm__ volatile("rjmp %x0" ::"i"(hubus_upd_init0));
}

uint8_t hubus_upd_flash_read(uint16_t addr)
{
    return pgm_read_byte(addr);
}

static void nvm_wait(void)
{
    while (NVMCTRL.STATUS & (NVMCTRL_FBUSY_bm | NVMCTRL_EEBUSY_bm)) {
    }
}

/* Flash and EEPROM share one page buffer; a command applies to the memory last loaded into it. */
static void nvm_cmd(uint8_t cmd)
{
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, cmd);
}

/* Waits for an earlier write and clears the page buffer. */
static void nvm_begin(void)
{
    nvm_wait();
    nvm_cmd(NVMCTRL_CMD_PAGEBUFCLR_gc);
    nvm_wait();
}

/* Fills the page buffer through the mapped flash, then erases and writes the page in one command. The CPU stalls while
 * the flash is busy. */
void hubus_upd_flash_write(uint16_t addr, const uint8_t *page)
{
    volatile uint8_t *dst = (volatile uint8_t *)(MAPPED_PROGMEM_START + addr);
    uint8_t i;

    nvm_begin();
    for (i = 0; i < (uint8_t)(HUBUS_UPD_PAGE_SIZE - 1u); i++) {
        dst[i] = page[i];
    }
    dst[i] = page[i]; /* the loop counter stays 8 bit also for 128-byte pages */
    nvm_cmd(NVMCTRL_CMD_PAGEERASEWRITE_gc);
    nvm_wait();
}

/* Starts the EEPROM write (about 4 ms, the CPU keeps running) and returns; a following page write waits for it. */
static void store_mcid(uint8_t mcid)
{
    if (MCID_EEPROM != mcid) {
        nvm_begin();
        MCID_EEPROM = mcid;
        nvm_cmd(NVMCTRL_CMD_PAGEERASEWRITE_gc);
    }
}

#if HUBUS_UPD_SET_MCID
void hubus_upd_store_mcid(uint8_t mcid)
{
    store_mcid(mcid);
}
#endif

void hubus_upd_reset(void)
{
    nvm_wait();
    _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRE_bm);
    for (;;) {
    }
}

/* The application may have left outputs, pull-ups, and pin interrupts on; start from the reset state of the ports. Not
 * inlined: unrolled, the loops take 80 bytes. */
__attribute__((noinline)) static void reset_port(PORT_t *port)
{
    volatile uint8_t *ctrl = &port->PIN0CTRL;
    uint8_t i;

    port->DIR = 0;
    port->OUT = 0;
    for (i = 0; i < 8u; i++) {
        ctrl[i] = 0;
    }
}

static void reset_ports(void)
{
    reset_port(&PORTA);
#ifdef PORTB
    reset_port(&PORTB);
#endif
#ifdef PORTC
    reset_port(&PORTC);
#endif
}

int main(void)
{
    uint8_t entered = (uint8_t)(GPIOR0 == HUBUS_UPDATE_KEY0);
    uint8_t mcid = GPIOR1;
    uint16_t entry;

    GPIOR0 = 0;
    while (WDT.STATUS & WDT_SYNCBUSY_bm) { /* a write of the application is still on its way */
    }
    _PROTECTED_WRITE(WDT.CTRLA, 0); /* a watchdog the application started */
    if (!entered && hubus_updater_app_valid(&entry)) {
        ((void (*)(void))entry)();
    }
    if (entered) {
        reset_ports();
    }
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLA, CLKCTRL_CLKSEL_OSC20M_gc);
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PDIV_2X_gc | CLKCTRL_PEN_bm);
    /* unsigned wrap-around: 0 and everything above HUBUS_MCID_MAX are no MCID */
    if (entered && (uint8_t)(mcid - HUBUS_MCID_MIN) <= HUBUS_MCID_MAX - HUBUS_MCID_MIN) {
        store_mcid(mcid);
    } else {
        mcid = hubus_slave_mcid_or(MCID_EEPROM, HUBUS_UPDATER_MCID);
    }

    hubus_hal_init();
    hubus_updater_init(mcid, hubus_hal_read_clk(), hubus_hal_read_data());
    for (;;) {
        hubus_core_pins(&hubus_updater_core, hubus_hal_read_clk(), hubus_hal_read_data());
        hubus_hal_data_low(hubus_core_data_low(&hubus_updater_core));
        hubus_updater_poll();
    }
}

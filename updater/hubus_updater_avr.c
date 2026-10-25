/*
 * Updater firmware for classic tinyAVR (reference: ATtiny85), linked to HUBUS_UPD_ADDR at the end of flash.
 *
 * It runs without interrupts: the main loop reads the pins and feeds the protocol core directly, so the updater
 * needs no vector table of its own. The vector table at address 0 belongs to the application; its reset vector
 * jumps here.
 *
 *   reset          start the application if the info page checks out, otherwise stay
 *   UPDATE_ENTER   the application jumps here with HUBUS_UPDATE_KEY0 in GPIOR0 and its MCID in GPIOR1
 *
 * The MCID comes from the application, which stores it in the last EEPROM byte for the next time the updater
 * starts without one (no valid application). Without that byte the build option HUBUS_UPDATER_MCID applies.
 *
 * Fuses: SELFPRGEN programmed (self-programming), brown-out detection recommended. The watchdog must not be
 * forced on (WDTON), because the updater switches it off.
 */
#include <avr/boot.h>
#include <avr/eeprom.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>

#include "hal/hubus_hal.h"
#include "hubus_updater.h"

#ifndef HUBUS_UPDATER_MCID
#define HUBUS_UPDATER_MCID 63
#endif
#define MCID_EEPROM ((uint8_t *)E2END)

uint8_t hubus_upd_flash_read(uint16_t addr)
{
    return pgm_read_byte(addr);
}

void hubus_upd_flash_write(uint16_t addr, const uint8_t *page)
{
    uint16_t i;

    eeprom_busy_wait();
    boot_page_erase(addr);
    boot_spm_busy_wait();
    for (i = 0; i < HUBUS_UPD_PAGE_SIZE; i += 2u) {
        boot_page_fill(addr + i, (uint16_t)(page[i] | (uint16_t)page[i + 1u] << 8));
    }
    boot_page_write(addr);
    boot_spm_busy_wait();
}

void hubus_upd_reset(void)
{
    wdt_enable(WDTO_15MS);
    for (;;) {
    }
}

/* The application may have left outputs and peripherals running; start from the reset state of the ports. */
static void reset_ports(void)
{
#ifdef DDRA
    DDRA = 0;
    PORTA = 0;
#endif
#ifdef DDRB
    DDRB = 0;
    PORTB = 0;
#endif
#ifdef DDRC
    DDRC = 0;
    PORTC = 0;
#endif
#ifdef DDRD
    DDRD = 0;
    PORTD = 0;
#endif
}

int main(void)
{
    uint8_t entered = (uint8_t)(GPIOR0 == HUBUS_UPDATE_KEY0);
    uint8_t mcid = GPIOR1;
    uint16_t entry;

    GPIOR0 = 0;
    MCUSR = 0; /* a watchdog reset keeps the watchdog running until WDRF is cleared */
    wdt_disable();
    if (!entered && hubus_updater_app_valid(&entry)) {
        ((void (*)(void))entry)();
    }
    if (entered) {
        reset_ports();
        if (mcid >= HUBUS_MCID_MIN && mcid <= HUBUS_MCID_MAX) {
            eeprom_update_byte(MCID_EEPROM, mcid);
        }
    }
    if (!entered || mcid < HUBUS_MCID_MIN || mcid > HUBUS_MCID_MAX) {
        mcid = eeprom_read_byte(MCID_EEPROM);
        if (mcid < HUBUS_MCID_MIN || mcid > HUBUS_MCID_MAX) {
            mcid = HUBUS_UPDATER_MCID;
        }
    }

    hubus_hal_init();
    hubus_updater_init(mcid, hubus_hal_read_clk(), hubus_hal_read_data());
    for (;;) {
        hubus_core_pins(&hubus_updater_core, hubus_hal_read_clk(), hubus_hal_read_data());
        hubus_hal_data_low(hubus_core_data_low(&hubus_updater_core));
        hubus_updater_poll();
    }
}

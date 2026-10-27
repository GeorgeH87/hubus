/*
 * HAL for classic tinyAVR (DDRx/PORTx/PINx, pin change interrupts).
 *
 * Pins are configured with HUBUS_CLK_PORT/HUBUS_CLK_PIN and
 * HUBUS_DATA_PORT/HUBUS_DATA_PIN (see hubus_slave_config.h). The pin change
 * interrupt group (PCMSKn, PCIEn, PCINTn_vect) follows from the port:
 *
 *   ATtiny25/45/85, 2313                   PB -> group 0 (single PCMSK)
 *   ATtiny24/44/84, 441/841, 261/461/861   PA -> group 0, PB -> group 1
 *   ATtiny48/88                            PB -> 0, PC -> 1, PD -> 2, PA -> 3
 *   ATtiny2313A/4313                       PB -> 0, PA -> 1, PD -> 2
 *   ATtiny1634                             PA -> 0, PB -> 1, PC -> 2
 *
 * For other classic devices define HUBUS_CLK_PCGROUP and HUBUS_DATA_PCGROUP
 * (the index n of the PCMSKn register that contains the pin) yourself.
 *
 * Both lines raise a pin change interrupt. The core compares the pins with
 * the previous levels, so one vector per group is enough.
 */
#ifndef HUBUS_HAL_CLASSIC_H
#define HUBUS_HAL_CLASSIC_H

#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

#define HUBUS_CLK_PORTREG HUBUS_CAT(PORT, HUBUS_CLK_PORT)
#define HUBUS_CLK_DDRREG HUBUS_CAT(DDR, HUBUS_CLK_PORT)
#define HUBUS_CLK_PINREG HUBUS_CAT(PIN, HUBUS_CLK_PORT)
#define HUBUS_DATA_PORTREG HUBUS_CAT(PORT, HUBUS_DATA_PORT)
#define HUBUS_DATA_DDRREG HUBUS_CAT(DDR, HUBUS_DATA_PORT)
#define HUBUS_DATA_PINREG HUBUS_CAT(PIN, HUBUS_DATA_PORT)

/* ---- pin change group per port ---------------------------------------- */

#if defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__) || defined(__AVR_ATtiny2313__)
#define HUBUS_PCGROUP_B 0
#elif defined(__AVR_ATtiny24__) || defined(__AVR_ATtiny24A__) || defined(__AVR_ATtiny44__) ||   \
    defined(__AVR_ATtiny44A__) || defined(__AVR_ATtiny84__) || defined(__AVR_ATtiny84A__) ||    \
    defined(__AVR_ATtiny441__) || defined(__AVR_ATtiny841__) || defined(__AVR_ATtiny261__) ||   \
    defined(__AVR_ATtiny261A__) || defined(__AVR_ATtiny461__) || defined(__AVR_ATtiny461A__) || \
    defined(__AVR_ATtiny861__) || defined(__AVR_ATtiny861A__)
#define HUBUS_PCGROUP_A 0
#define HUBUS_PCGROUP_B 1
#elif defined(__AVR_ATtiny48__) || defined(__AVR_ATtiny88__)
#define HUBUS_PCGROUP_B 0
#define HUBUS_PCGROUP_C 1
#define HUBUS_PCGROUP_D 2
#define HUBUS_PCGROUP_A 3
#elif defined(__AVR_ATtiny2313A__) || defined(__AVR_ATtiny4313__)
#define HUBUS_PCGROUP_B 0
#define HUBUS_PCGROUP_A 1
#define HUBUS_PCGROUP_D 2
#elif defined(__AVR_ATtiny1634__)
#define HUBUS_PCGROUP_A 0
#define HUBUS_PCGROUP_B 1
#define HUBUS_PCGROUP_C 2
#endif

#ifndef HUBUS_CLK_PCGROUP
#define HUBUS_CLK_PCGROUP HUBUS_CAT(HUBUS_PCGROUP_, HUBUS_CLK_PORT)
#endif
#ifndef HUBUS_DATA_PCGROUP
#define HUBUS_DATA_PCGROUP HUBUS_CAT(HUBUS_PCGROUP_, HUBUS_DATA_PORT)
#endif

/* Register, bit, and vector names of a group. The name is pasted in the macro body, so an alias such as PCMSK
 * (defined next to PCMSK0 on the ATtiny2313A/4313) is not expanded before the group number is appended. */
#define HUBUS_PCMSK_N_(group) PCMSK##group
#define HUBUS_PCMSK_N(group) HUBUS_PCMSK_N_(group)
#define HUBUS_PCIE_N_(group) PCIE##group
#define HUBUS_PCIE_N(group) HUBUS_PCIE_N_(group)
#define HUBUS_PCINT_VECT_N_(group) PCINT##group##_vect
#define HUBUS_PCINT_VECT_N(group) HUBUS_PCINT_VECT_N_(group)

/* PCMSKn register of a group (devices with a single PCMSK have only group 0). */
#if defined(PCMSK0)
#define HUBUS_PCMSK(group) HUBUS_PCMSK_N(group)
#else
#define HUBUS_PCMSK(group) PCMSK
#endif

/* Interrupt enable bit of a group. */
#if defined(PCICR)
#define HUBUS_PC_ENABLE(group) (PCICR |= (uint8_t)_BV(HUBUS_PCIE_N(group)))
#elif defined(PCIE0)
#define HUBUS_PC_ENABLE(group) (GIMSK |= (uint8_t)_BV(HUBUS_PCIE_N(group)))
#else
#define HUBUS_PC_ENABLE(group) (GIMSK |= (uint8_t)_BV(PCIE))
#endif

/* Interrupt vector of a group. */
#if defined(PCINT0_vect)
#define HUBUS_PC_VECT(group) HUBUS_PCINT_VECT_N(group)
#else
#define HUBUS_PC_VECT(group) PCINT_vect
#endif

/* ---- HAL functions ----------------------------------------------------- */

static inline void hubus_hal_init(void)
{
    HUBUS_CLK_DDRREG &= (uint8_t)~_BV(HUBUS_CLK_PIN);
    HUBUS_CLK_PORTREG &= (uint8_t)~_BV(HUBUS_CLK_PIN);
    /* DATA released: input without pull-up (external pull-up). The output
     * latch stays 0, so setting DDR later pulls the line low. */
    HUBUS_DATA_DDRREG &= (uint8_t)~_BV(HUBUS_DATA_PIN);
    HUBUS_DATA_PORTREG &= (uint8_t)~_BV(HUBUS_DATA_PIN);
}

static inline uint8_t hubus_hal_read_clk(void)
{
    return (HUBUS_CLK_PINREG & _BV(HUBUS_CLK_PIN)) ? 1u : 0u;
}

static inline uint8_t hubus_hal_read_data(void)
{
    return (HUBUS_DATA_PINREG & _BV(HUBUS_DATA_PIN)) ? 1u : 0u;
}

static inline void hubus_hal_data_low(uint8_t low)
{
    if (low) {
        HUBUS_DATA_DDRREG |= (uint8_t)_BV(HUBUS_DATA_PIN);
    } else {
        HUBUS_DATA_DDRREG &= (uint8_t)~_BV(HUBUS_DATA_PIN);
    }
}

static inline void hubus_hal_irq_enable(void)
{
    HUBUS_PCMSK(HUBUS_CLK_PCGROUP) |= (uint8_t)_BV(HUBUS_CLK_PIN);
    HUBUS_PCMSK(HUBUS_DATA_PCGROUP) |= (uint8_t)_BV(HUBUS_DATA_PIN);
    HUBUS_PC_ENABLE(HUBUS_CLK_PCGROUP);
#if HUBUS_DATA_PCGROUP != HUBUS_CLK_PCGROUP
    HUBUS_PC_ENABLE(HUBUS_DATA_PCGROUP);
#endif
}

#if HUBUS_ENABLE_UPDATE
#ifndef HUBUS_UPDATER_ADDR
#error "HUBUS_ENABLE_UPDATE needs HUBUS_UPDATER_ADDR, the byte address of the updater in flash"
#endif
#include <avr/pgmspace.h>

#include "hubus_proto.h"

/*
 * Hands over to the updater (docs/spec/06-operation.md section 6.8): key and MCID in GPIOR0 and GPIOR1, then a jump
 * with interrupts disabled. The updater sets up its own stack. Returns only if no updater is installed (erased
 * flash at its address).
 */
static inline void hubus_hal_enter_updater(uint8_t mcid)
{
    if (pgm_read_word(HUBUS_UPDATER_ADDR) == 0xFFFFu) {
        return;
    }
    cli();
    GPIOR0 = HUBUS_UPDATE_KEY0;
    GPIOR1 = mcid;
    ((void (*)(void))(HUBUS_UPDATER_ADDR / 2u))();
}
#endif

#define HUBUS_ATOMIC_BEGIN()         \
    uint8_t hubus_sreg_saved = SREG; \
    cli()
#define HUBUS_ATOMIC_END() (SREG = hubus_sreg_saved)

static inline void hubus_hal_cli(void) { cli(); }
static inline void hubus_hal_sei(void) { sei(); }

/* Defined in hubus_slave.c; called from every vector. */
void hubus_slave_isr(void);

#if HUBUS_DATA_PCGROUP == HUBUS_CLK_PCGROUP
#define HUBUS_HAL_ISRS()                  \
    ISR(HUBUS_PC_VECT(HUBUS_CLK_PCGROUP)) \
    {                                     \
        hubus_slave_isr();                \
    }
#else
#define HUBUS_HAL_ISRS()                   \
    ISR(HUBUS_PC_VECT(HUBUS_CLK_PCGROUP))  \
    {                                      \
        hubus_slave_isr();                 \
    }                                      \
    ISR(HUBUS_PC_VECT(HUBUS_DATA_PCGROUP)) \
    {                                      \
        hubus_slave_isr();                 \
    }
#endif

#endif /* HUBUS_HAL_CLASSIC_H */

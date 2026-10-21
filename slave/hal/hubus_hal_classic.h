/*
 * HAL for classic tinyAVR (DDRx/PORTx/PINx, pin change interrupts).
 *
 * Pins are configured with HUBUS_CLK_PORT/HUBUS_CLK_PIN and
 * HUBUS_DATA_PORT/HUBUS_DATA_PIN (see hubus_slave_config.h). The pin change
 * interrupt group (PCMSKn, PCIEn, PCINTn_vect) follows from the port:
 *
 *   ATtiny25/45/85                         PB -> group 0 (single PCMSK)
 *   ATtiny24/44/84, 441/841, 261/461/861   PA -> group 0, PB -> group 1
 *   ATtiny48/88                            PB -> 0, PC -> 1, PD -> 2, PA -> 3
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

#if defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
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
#endif

#ifndef HUBUS_CLK_PCGROUP
#define HUBUS_CLK_PCGROUP HUBUS_CAT(HUBUS_PCGROUP_, HUBUS_CLK_PORT)
#endif
#ifndef HUBUS_DATA_PCGROUP
#define HUBUS_DATA_PCGROUP HUBUS_CAT(HUBUS_PCGROUP_, HUBUS_DATA_PORT)
#endif

/* PCMSKn register of a group (devices with a single PCMSK have only group 0). */
#if defined(PCMSK0)
#define HUBUS_PCMSK(group) HUBUS_CAT(PCMSK, group)
#else
#define HUBUS_PCMSK(group) PCMSK
#endif

/* Interrupt enable bit of a group. */
#if defined(PCICR)
#define HUBUS_PC_ENABLE(group) (PCICR |= (uint8_t)_BV(HUBUS_CAT(PCIE, group)))
#elif defined(PCIE0)
#define HUBUS_PC_ENABLE(group) (GIMSK |= (uint8_t)_BV(HUBUS_CAT(PCIE, group)))
#else
#define HUBUS_PC_ENABLE(group) (GIMSK |= (uint8_t)_BV(PCIE))
#endif

/* Interrupt vector of a group. */
#if defined(PCINT0_vect)
#define HUBUS_PC_VECT(group) HUBUS_CAT3(PCINT, group, _vect)
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

#define HUBUS_ATOMIC_BEGIN()         \
    uint8_t hubus_sreg_saved = SREG; \
    cli()
#define HUBUS_ATOMIC_END() (SREG = hubus_sreg_saved)

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

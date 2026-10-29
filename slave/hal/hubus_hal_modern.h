/*
 * HAL for tinyAVR 0/1/2-series (PORTx/VPORTx, per-pin interrupt sense).
 *
 * Pins are configured with HUBUS_CLK_PORT/HUBUS_CLK_PIN and
 * HUBUS_DATA_PORT/HUBUS_DATA_PIN (see hubus_slave_config.h), for example
 * HUBUS_CLK_PORT = A and HUBUS_CLK_PIN = 2 for PA2.
 *
 * Both lines sense both edges. The vector is PORTx_PORT_vect; pins on two
 * different ports get two vectors, both calling the same handler.
 */
#ifndef HUBUS_HAL_MODERN_H
#define HUBUS_HAL_MODERN_H

#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

#define HUBUS_CLK_PORTSTRUCT HUBUS_CAT(PORT, HUBUS_CLK_PORT)
#define HUBUS_CLK_VPORT HUBUS_CAT(VPORT, HUBUS_CLK_PORT)
#define HUBUS_DATA_PORTSTRUCT HUBUS_CAT(PORT, HUBUS_DATA_PORT)
#define HUBUS_DATA_VPORT HUBUS_CAT(VPORT, HUBUS_DATA_PORT)

#define HUBUS_PORTID_A 0
#define HUBUS_PORTID_B 1
#define HUBUS_PORTID_C 2
#define HUBUS_CLK_PORT_ID HUBUS_CAT(HUBUS_PORTID_, HUBUS_CLK_PORT)
#define HUBUS_DATA_PORT_ID HUBUS_CAT(HUBUS_PORTID_, HUBUS_DATA_PORT)

static inline void hubus_hal_init(void)
{
    HUBUS_CLK_VPORT.DIR &= (uint8_t)~_BV(HUBUS_CLK_PIN);
    /* DATA released: input. The output latch stays 0, so setting DIR later pulls the line low. */
    HUBUS_DATA_VPORT.DIR &= (uint8_t)~_BV(HUBUS_DATA_PIN);
    HUBUS_DATA_VPORT.OUT &= (uint8_t)~_BV(HUBUS_DATA_PIN);
}

static inline uint8_t hubus_hal_read_clk(void)
{
    return (HUBUS_CLK_VPORT.IN & _BV(HUBUS_CLK_PIN)) ? 1u : 0u;
}

static inline uint8_t hubus_hal_read_data(void)
{
    return (HUBUS_DATA_VPORT.IN & _BV(HUBUS_DATA_PIN)) ? 1u : 0u;
}

static inline void hubus_hal_data_low(uint8_t low)
{
    if (low) {
        HUBUS_DATA_VPORT.DIR |= (uint8_t)_BV(HUBUS_DATA_PIN);
    } else {
        HUBUS_DATA_VPORT.DIR &= (uint8_t)~_BV(HUBUS_DATA_PIN);
    }
}

static inline void hubus_hal_irq_enable(void)
{
    HUBUS_CLK_PORTSTRUCT.HUBUS_CAT3(PIN, HUBUS_CLK_PIN, CTRL) = PORT_ISC_BOTHEDGES_gc;
    HUBUS_DATA_PORTSTRUCT.HUBUS_CAT3(PIN, HUBUS_DATA_PIN, CTRL) = PORT_ISC_BOTHEDGES_gc;
}

#if HUBUS_ENABLE_UPDATE
#error "HUBUS_ENABLE_UPDATE: no updater for the tinyAVR 0/1/2-series yet (classic tinyAVR only)"
#endif

#define HUBUS_ATOMIC_BEGIN()         \
    uint8_t hubus_sreg_saved = SREG; \
    cli()
#define HUBUS_ATOMIC_END() (SREG = hubus_sreg_saved)

static inline void hubus_hal_cli(void) { cli(); }
static inline void hubus_hal_sei(void) { sei(); }

/* 1 if a CLK or DATA change is waiting for the ISR (with interrupts disabled). */
static inline uint8_t hubus_hal_edge_pending(void)
{
#if HUBUS_CLK_PORT_ID == HUBUS_DATA_PORT_ID
    return (HUBUS_CLK_VPORT.INTFLAGS & (uint8_t)(_BV(HUBUS_CLK_PIN) | _BV(HUBUS_DATA_PIN))) ? 1u : 0u;
#else
    return ((HUBUS_CLK_VPORT.INTFLAGS & (uint8_t)_BV(HUBUS_CLK_PIN)) ||
            (HUBUS_DATA_VPORT.INTFLAGS & (uint8_t)_BV(HUBUS_DATA_PIN)))
               ? 1u
               : 0u;
#endif
}

/* Defined in hubus_slave.c; called from every vector. */
void hubus_slave_isr(void);

/* Clear the flags first, then read the pins: a change in between raises the interrupt again. */
#if HUBUS_CLK_PORT_ID == HUBUS_DATA_PORT_ID
#define HUBUS_HAL_ISRS()                                                                \
    ISR(HUBUS_CAT3(PORT, HUBUS_CLK_PORT, _PORT_vect))                                   \
    {                                                                                   \
        HUBUS_CLK_VPORT.INTFLAGS = (uint8_t)(_BV(HUBUS_CLK_PIN) | _BV(HUBUS_DATA_PIN)); \
        hubus_slave_isr();                                                              \
    }
#else
#define HUBUS_HAL_ISRS()                                          \
    ISR(HUBUS_CAT3(PORT, HUBUS_CLK_PORT, _PORT_vect))             \
    {                                                             \
        HUBUS_CLK_VPORT.INTFLAGS = (uint8_t)_BV(HUBUS_CLK_PIN);   \
        hubus_slave_isr();                                        \
    }                                                             \
    ISR(HUBUS_CAT3(PORT, HUBUS_DATA_PORT, _PORT_vect))            \
    {                                                             \
        HUBUS_DATA_VPORT.INTFLAGS = (uint8_t)_BV(HUBUS_DATA_PIN); \
        hubus_slave_isr();                                        \
    }
#endif

#endif /* HUBUS_HAL_MODERN_H */

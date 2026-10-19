/**
 * HuBus Slave Library - GPIO Hardware Abstraction Layer Implementation
 */

#include "hubus_gpio_hal.h"
#include <avr/io.h>

void hubus_gpio_init(void)
{
    // Set CLK and DATA pins as inputs (DDR = 0)
    // This puts them in high-impedance state (external pullups take over)
    HUBUS_CLK_DDR &= ~(1 << HUBUS_CLK_BIT);
    HUBUS_DATA_DDR &= ~(1 << HUBUS_DATA_BIT);
    
    // Disable pull-up resistors (internal pullups disabled)
    // On ATtiny, disable by writing 0 to PORTx
    HUBUS_CLK_PORT &= ~(1 << HUBUS_CLK_BIT);
    HUBUS_DATA_PORT &= ~(1 << HUBUS_DATA_BIT);
}

uint8_t hubus_clk_is_high(void)
{
    return (HUBUS_CLK_PIN >> HUBUS_CLK_BIT) & 1;
}

uint8_t hubus_clk_is_low(void)
{
    return !hubus_clk_is_high();
}

uint8_t hubus_data_is_high(void)
{
    return (HUBUS_DATA_PIN >> HUBUS_DATA_BIT) & 1;
}

uint8_t hubus_data_is_low(void)
{
    return !hubus_data_is_high();
}

void hubus_data_pull_low(void)
{
    // Set DDR bit = 1 (output mode)
    // Since PORT is 0 (set in init), this drives line to ground
    HUBUS_DATA_DDR |= (1 << HUBUS_DATA_BIT);
}

void hubus_data_release(void)
{
    // Clear DDR bit = 0 (input mode, high-impedance)
    // External pullup resistor pulls line high
    HUBUS_DATA_DDR &= ~(1 << HUBUS_DATA_BIT);
}

void hubus_gpio_sample_delay(void)
{
    // Simple busy-wait loop for signal stabilization
    // Adjust count based on CPU frequency and propagation delay
    volatile uint8_t delay_count = HUBUS_SAMPLE_DELAY_CYCLES;
    while (delay_count--) {
        __asm__ __volatile__("nop");
    }
}

/* ====================================================================
   PCINT Setup (Interrupt-Driven Mode)
   ==================================================================== */

#if HUBUS_USE_PCINT

// Volatile flag set by ISR, cleared by main loop
static volatile uint8_t g_clk_edge_flag = 0;

void hubus_gpio_pcint_init(void)
{
    // Enable Pin Change Interrupt on the relevant port
    GIMSK |= (1 << PCIE);
    
    // Enable PCINT on CLK pin
    PCMSK |= (1 << HUBUS_CLK_BIT);
    
    // Global interrupt enable
    sei();
}

void hubus_gpio_pcint_disable(void)
{
    // Disable PCINT globally
    GIMSK &= ~(1 << PCIE);
    cli();
}

uint8_t hubus_gpio_clk_edge_pending(void)
{
    return g_clk_edge_flag;
}

void hubus_gpio_clk_edge_clear(void)
{
    g_clk_edge_flag = 0;
}

// ISR: Pin Change Interrupt on PORTB (CLK line)
// This is called on every CLK edge (rising, falling, or both depending on config)
ISR(PCINT0_vect)
{
    // Set flag for main loop to process
    // The main loop will call handle_clk_edge() to sample DATA
    g_clk_edge_flag = 1;
}

#endif

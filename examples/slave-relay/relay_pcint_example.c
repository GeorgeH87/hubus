/**
 * HuBus Slave Example: LED Control with PCINT (Event-Driven)
 * 
 * Demonstrates the new PCINT-based (interrupt-driven) architecture:
 * - CLK ISR triggers on bus activity
 * - Main loop handles timeout checks and application logic
 * - Clean separation: interrupt for timing, main loop for processing
 * 
 * Target: ATtiny85 with:
 * - PB2 = CLK (input, PCINT trigger)
 * - PB3 = DATA (open-drain)
 * - PB0 = LED output (active high)
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include "../../lib/hubus-slave/hubus_slave.h"

// LED output pin
#define LED_DDR     DDRB
#define LED_PORT    PORTB
#define LED_PIN     PB0

// LED state
static uint8_t led_on = 0;

// Callback: handles incoming commands
static void on_hubus_event(hubus_event_t event, const uint8_t *payload, uint8_t len)
{
    switch (event) {
        case HUBUS_EVENT_COMMAND:
            // Master sent SEND_COMMAND
            if (len > 0) {
                uint8_t opcode = payload[0];
                
                if (opcode == 0x01) {
                    // Turn ON
                    LED_PORT |= (1 << LED_PIN);
                    led_on = 1;
                } else if (opcode == 0x00) {
                    // Turn OFF
                    LED_PORT &= ~(1 << LED_PIN);
                    led_on = 0;
                }
            }
            break;
            
        case HUBUS_EVENT_REQUEST_STATUS:
            // Master requests status
            hubus_slave_set_status(&led_on, 1);
            break;
            
        default:
            break;
    }
}

int main(void)
{
    // Configure LED pin as output
    LED_DDR |= (1 << LED_PIN);
    LED_PORT &= ~(1 << LED_PIN);  // Start with LED off
    
    // Initialize HuBus slave with PCINT support
    // MCID = 0x01 (slave ID 1)
    // Callback enabled for command handling
    hubus_slave_init(0x01, on_hubus_event);
    
    // PCINT is already initialized in hubus_slave_init()
    // ISR will be triggered automatically on CLK edges
    
    // Main loop: handle timeouts and application logic
    while (1) {
        // Check if CLK edge was detected by ISR
        if (hubus_gpio_clk_edge_pending()) {
            // Process the CLK edge (bit reception)
            hubus_slave_process_clk_edge();
            hubus_gpio_clk_edge_clear();
        }
        
        // Periodically check for timeouts (every ~1ms)
        // This keeps the state machine responsive even if no CLK
        static uint16_t timeout_counter = 0;
        timeout_counter++;
        if (timeout_counter > 100) {  // ~1ms at 1 MHz loop
            hubus_slave_process();  // Timeout checks
            timeout_counter = 0;
        }
        
        // Application logic can run here too
        // No blocking delays needed!
    }
    
    return 0;
}

/**
 * Note on PCINT:
 * - The ISR (PCINT0_vect) is automatically called when CLK changes
 * - ISR just sets a flag; actual processing happens in main loop
 * - This is safe (short ISR) and deterministic (event-driven)
 */

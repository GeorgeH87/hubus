/**
 * HuBus Slave Example: LED Control (ISR-Only)
 * 
 * Completely interrupt-driven architecture:
 * - CLK edge ISR automatically triggered via PCINT
 * - Main loop is empty (or can do other app tasks)
 * - All HuBus communication handled in ISR and callbacks
 * 
 * This is the cleanest design: HuBus runs on interrupts, application on main loop.
 * 
 * Target: ATtiny85 with:
 * - PB2 = CLK (input, PCINT trigger)
 * - PB3 = DATA (open-drain)
 * - PB0 = LED output (active high)
 */

#include <avr/io.h>
#include <avr/interrupt.h>
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
            // Master requests status (POLL_PENDING or REQUEST_INFO)
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
    
    // Initialize HuBus slave with ISR support
    // MCID = 0x01 (slave ID 1)
    // Callback handles all HuBus events
    // PCINT ISR is automatically enabled in hubus_slave_init()
    hubus_slave_init(0x01, on_hubus_event);
    
    // Main loop: completely empty!
    // All HuBus communication happens in CLK ISR + callbacks
    while (1) {
        // Application logic can run here (non-blocking):
        // - blink patterns
        // - sensor reads
        // - button debouncing
        // - etc.
        
        // Or just sleep to save power:
        // _delay_ms(100);
    }
    
    return 0;
}

/**
 * Architecture Notes:
 * 
 * 1. CLK ISR (PCINT0_vect) automatically triggered by GPIO interrupt
 *    - Calls hubus_state_process_clk_edge() internally
 *    - Processes one bit of incoming frame
 *    - When frame complete, calls callback
 *    - Entire ISR runs in < 1 µs
 * 
 * 2. Callback handles all event logic
 *    - Reads payload
 *    - Updates LED state
 *    - Prepares response data
 *    - Signals pending if needed
 * 
 * 3. Main loop can be completely empty
 *    - No polling
 *    - No timer checks
 *    - No blocking delays needed
 *    - Can sleep or run other tasks
 * 
 * Benefits:
 * - Very fast CLK response (ISR priority)
 * - CPU efficient (only active on bus activity)
 * - Deterministic timing (no polling jitter)
 * - Simple integration (one init call)
 * - Easy to add application logic (just modify main loop)
 */

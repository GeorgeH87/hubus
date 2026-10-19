/**
 * HuBus Slave Example: Push Button / Switch
 * 
 * A switch slave that:
 * - Detects button press (GPIO input)
 * - Signals master when state changes
 * - Responds with current button state on master request
 * - Uses hubus_slave_signal_pending() to announce events
 * 
 * Target: ATtiny85 with:
 * - PB2 = CLK (input)
 * - PB3 = DATA (open-drain)
 * - PB1 = Button input (active low, pull-up)
 * 
 * Demonstrates event-driven operation without callback.
 */

#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>
#include "../../lib/hubus-slave/hubus_slave.h"

// Button pin definition
#define BUTTON_DDR      DDRB
#define BUTTON_PORT     PORTB
#define BUTTON_PIN      PB1

// Button state tracking
static uint8_t button_pressed = 0;
static uint8_t button_debounce = 0;
static uint8_t last_reported_state = 0xFF;  // Force initial update

// Simple debounce: count stable reads
#define DEBOUNCE_COUNT 10

int main(void)
{
    // Configure button pin as input with pull-up
    BUTTON_DDR &= ~(1 << BUTTON_PIN);     // Input
    BUTTON_PORT |= (1 << BUTTON_PIN);     // Pull-up on
    
    // Initialize HuBus slave
    // MCID = 0x03 (slave ID 3)
    // No callback; we'll manage data manually
    hubus_slave_init(0x03, NULL);
    
    // Prepare initial status
    hubus_slave_set_status(&button_pressed, 1);
    
    while (1) {
        // Process HuBus communication
        hubus_slave_process();
        
        // Read button input (inverted: 0 = pressed, 1 = released)
        uint8_t btn_raw = (BUTTON_PIN >> BUTTON_PIN) & 1;
        uint8_t btn_state = btn_raw ^ 1;  // Invert to 1 = pressed
        
        // Simple debouncing
        if (btn_state == button_pressed) {
            button_debounce++;
            if (button_debounce >= DEBOUNCE_COUNT) {
                // State is stable
                if (btn_state != last_reported_state) {
                    // State changed, signal master
                    button_pressed = btn_state;
                    last_reported_state = btn_state;
                    
                    // Update buffered status
                    hubus_slave_set_status(&button_pressed, 1);
                    
                    // Signal master that we have data
                    hubus_slave_signal_pending();
                    
                    button_debounce = 0;
                }
            }
        } else {
            button_debounce = 0;
        }
        
        _delay_us(100);  // ~1ms loop (adjust for debounce timing)
    }
    
    return 0;
}

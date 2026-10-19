/**
 * HuBus Slave Example: LED/Relay Control (ISR-Only)
 * 
 * Demonstrates interrupt-driven HuBus slave:
 * - Receives ON/OFF commands via callback
 * - Sends status when requested
 * - Main loop is completely empty (ISR handles everything)
 * 
 * Target: ATtiny85 with:
 * - PB2 = CLK (input, PCINT trigger)
 * - PB3 = DATA (open-drain)
 * - PB0 = LED/Relay output (active high)
 */

#include <avr/io.h>
#include "../../lib/hubus-slave/hubus_slave.h"

// LED/Relay output pin
#define LED_DDR     DDRB
#define LED_PORT    PORTB
#define LED_PIN     PB0

// LED state
static uint8_t led_on = 0;

// Callback function: handles incoming commands
static void on_hubus_event(hubus_event_t event, const uint8_t *payload, uint8_t len)
{
    switch (event) {
        case HUBUS_EVENT_COMMAND:
            // Master sent SEND_COMMAND
            // Command format: byte 0 = opcode
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
            // Master requests status (either POLL_PENDING or REQUEST_INFO)
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
    
    // Initialize HuBus slave (automatically enables PCINT ISR)
    // MCID = 0x01 (slave ID 1)
    // Callback handles all HuBus events
    hubus_slave_init(0x01, on_hubus_event);
    
    // Main loop: ISR handles everything automatically!
    // This can be empty, or run your application logic:
    while (1) {
        // ISR processes CLK edges and calls callback
        // No polling, no timing loops needed
    }
    
    return 0;
}

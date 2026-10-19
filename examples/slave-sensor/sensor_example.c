/**
 * HuBus Slave Example: Temperature Sensor
 * 
 * A temperature sensor slave that:
 * - Periodically reads sensor value (ADC)
 * - Buffers data for master requests
 * - Responds to REQUEST_DATA with current temperature
 * - Uses callback to handle master requests efficiently
 * 
 * Target: ATtiny85 with:
 * - PB2 = CLK (input)
 * - PB3 = DATA (open-drain)
 * - PB4 = ADC input (temperature sensor analog)
 * 
 * Simple ADC reading for temperature (0..255 maps to 0..100°C)
 */

#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>
#include "../../lib/hubus-slave/hubus_slave.h"

// ADC channel for temperature sensor
#define TEMP_ADC_CHANNEL 3  // ADC3 on PB3 (or use different pin)

// Global temperature value
static uint8_t current_temp = 0;

// Callback function: handles master requests
static void on_hubus_event(hubus_event_t event, const uint8_t *payload, uint8_t len)
{
    switch (event) {
        case HUBUS_EVENT_REQUEST_DATA:
            // Master requests data (POLL_PENDING, REQUEST_INFO, or REQUEST_DATA)
            // Send current temperature reading
            hubus_slave_set_buffered_data(&current_temp, 1);
            break;
            
        default:
            break;
    }
}

// Simple ADC read
static uint8_t read_adc(uint8_t channel)
{
    // Configure ADC for single-ended input
    ADMUX = (1 << REFS0) | (channel & 0x0F);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS0);  // Enable, prescaler
    
    // Start conversion
    ADCSRA |= (1 << ADSC);
    
    // Wait for completion
    while (ADCSRA & (1 << ADSC));
    
    return ADCH;  // Return high byte (8-bit result)
}

int main(void)
{
    // Initialize HuBus slave
    // MCID = 0x02 (slave ID 2)
    hubus_slave_init(0x02, on_hubus_event);
    
    uint16_t sample_counter = 0;
    
    // Main loop
    while (1) {
        // Process HuBus communication
        hubus_slave_process();
        
        // Periodically read temperature (e.g., every 100ms)
        sample_counter++;
        if (sample_counter > 10000) {
            current_temp = read_adc(TEMP_ADC_CHANNEL);
            sample_counter = 0;
        }
        
        _delay_us(10);
    }
    
    return 0;
}

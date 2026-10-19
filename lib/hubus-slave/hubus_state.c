/**
 * HuBus Slave Library - State Machine Implementation (ISR-Only)
 * 
 * Completely interrupt-driven architecture:
 * - CLK ISR (PCINT0_vect) calls hubus_state_process_clk_edge() for each bit
 * - Frame parser converts bits to bytes and validates
 * - Callbacks triggered on events
 * - NO polling, NO timeouts, NO main-loop involvement
 */

#include "hubus_state.h"
#include "hubus_gpio_hal.h"
#include "hubus_crc.h"
#include "hubus_buffer.h"
#include "hubus_frame.h"
#include "hubus_slave_config.h"
#include <string.h>

// Singleton state machine instance
static hubus_state_machine_t g_state_machine;
static uint8_t g_initialized = 0;

/* ====================================================================
   Initialization
   ==================================================================== */

hubus_state_machine_t *hubus_state_init(uint8_t mcid, hubus_callback_t callback)
{
    g_state_machine.state = HUBUS_STATE_IDLE;
    g_state_machine.mcid = mcid & 0x3F;
    g_state_machine.callback = callback;
    g_state_machine.clk_tick_count = 0;
    g_state_machine.arb_bit_index = 0;
    g_state_machine.arb_won = 0;
    g_state_machine.tx_byte_index = 0;
    g_state_machine.tx_bit_index = 0;
    g_state_machine.pending_event = 0;
    
    hubus_bitbuffer_init(&g_state_machine.rx_bitbuffer);
    
    #if HUBUS_USE_PCINT
    hubus_gpio_pcint_init();
    #endif
    
    g_initialized = 1;
    return &g_state_machine;
}

hubus_state_machine_t *hubus_state_get(void)
{
    return &g_state_machine;
}

void hubus_state_set_pending(void)
{
    g_state_machine.pending_event = 1;
}

void hubus_state_clear_pending(void)
{
    g_state_machine.pending_event = 0;
}

const char *hubus_state_name(hubus_state_t state)
{
    switch (state) {
        case HUBUS_STATE_IDLE:          return "IDLE";
        case HUBUS_STATE_MASTER_INTENT: return "MASTER_INTENT";
        case HUBUS_STATE_SLAVE_INTENT:  return "SLAVE_INTENT";
        case HUBUS_STATE_ARBITRATION:   return "ARBITRATION";
        case HUBUS_STATE_TRANSFER:      return "TRANSFER";
        case HUBUS_STATE_RECOVERY:      return "RECOVERY";
        default:                        return "UNKNOWN";
    }
}

/* ====================================================================
   State Handlers (Forward Declarations)
   ==================================================================== */

static uint8_t handle_idle_clk(void);
static uint8_t handle_master_intent_clk(void);
static uint8_t handle_slave_intent_clk(void);
static uint8_t handle_arbitration_clk(void);
static uint8_t handle_transfer_clk(void);
static uint8_t handle_recovery_clk(void);

static uint8_t parse_and_dispatch_rx_frame(void);

/* ====================================================================
   Main Processing Function (ISR-Driven Only)
   ==================================================================== */

uint8_t hubus_state_process_clk_edge(void)
{
    if (!g_initialized) {
        return 0;
    }
    
    hubus_state_t old_state = g_state_machine.state;
    
    // Handle CLK edge based on current state
    switch (g_state_machine.state) {
        case HUBUS_STATE_IDLE:
            handle_idle_clk();
            break;
        case HUBUS_STATE_MASTER_INTENT:
            handle_master_intent_clk();
            break;
        case HUBUS_STATE_SLAVE_INTENT:
            handle_slave_intent_clk();
            break;
        case HUBUS_STATE_ARBITRATION:
            handle_arbitration_clk();
            break;
        case HUBUS_STATE_TRANSFER:
            handle_transfer_clk();
            break;
        case HUBUS_STATE_RECOVERY:
            handle_recovery_clk();
            break;
    }
    
    return (g_state_machine.state != old_state) ? 1 : 0;
}

/* ====================================================================
   IDLE State Handler
   ==================================================================== */

static uint8_t handle_idle_clk(void)
{
    // In IDLE, a CLK edge indicates master is starting communication
    // If we're pending, master is confirming with CLK ticks
    
    if (g_state_machine.pending_event) {
        // We're signaling pending data; count CLK ticks for confirmation
        g_state_machine.clk_tick_count++;
        
        if (g_state_machine.clk_tick_count >= 2) {
            // Master confirmed our pending signal, now release DATA
            hubus_data_release();
            // Wait for master to send command
            g_state_machine.state = HUBUS_STATE_MASTER_INTENT;
            hubus_bitbuffer_reset(&g_state_machine.rx_bitbuffer);
            g_state_machine.clk_tick_count = 0;
        }
    }
    // If not pending, ignore CLK in IDLE (master may be talking to someone else)
    
    return 0;
}

/* ====================================================================
   SLAVE_INTENT State Handler
   ==================================================================== */

static uint8_t handle_slave_intent_clk(void)
{
    // We've pulled DATA low to announce pending data
    // Master will send CLK ticks to confirm
    
    g_state_machine.clk_tick_count++;
    
    if (g_state_machine.clk_tick_count >= 2) {
        // Confirmed; master will now send command
        hubus_data_release();
        g_state_machine.state = HUBUS_STATE_MASTER_INTENT;
        hubus_bitbuffer_reset(&g_state_machine.rx_bitbuffer);
        g_state_machine.clk_tick_count = 0;
    }
    
    return 0;
}

/* ====================================================================
   MASTER_INTENT State Handler (Bit-Level Parsing)
   ==================================================================== */

static uint8_t handle_master_intent_clk(void)
{
    // Master is sending a frame bit-by-bit, synchronized with CLK
    // Sample DATA line on CLK edge
    
    // Small delay to allow signal settling
    hubus_gpio_sample_delay();
    
    // Read DATA bit
    uint8_t data_bit = hubus_data_is_high() ? 1 : 0;
    
    // Add bit to buffer
    uint8_t frame_complete = hubus_bitbuffer_add_bit(&g_state_machine.rx_bitbuffer, data_bit);
    
    if (frame_complete) {
        // Frame received; parse it
        parse_and_dispatch_rx_frame();
        
        // Transition to TRANSFER
        g_state_machine.state = HUBUS_STATE_TRANSFER;
        g_state_machine.tx_byte_index = 0;
        g_state_machine.tx_bit_index = 0;
    }
    
    return 0;
}

/* ====================================================================
   TRANSFER State Handler
   ==================================================================== */

static uint8_t handle_transfer_clk(void)
{
    // Send response frame bit-by-bit
    // For now, simplified: mark transfer complete and return to IDLE
    // TODO: Implement bit-level transmission synchronized with CLK
    
    g_state_machine.state = HUBUS_STATE_IDLE;
    hubus_buffer_clear_data();
    
    if (g_state_machine.callback) {
        g_state_machine.callback(HUBUS_EVENT_TRANSFER_COMPLETE, NULL, 0);
    }
    
    return 1;
}

/* ====================================================================
   ARBITRATION State Handler
   ==================================================================== */

static uint8_t handle_arbitration_clk(void)
{
    // Multiple slaves sending MCID bit-by-bit
    // For now, assume single slave (simplified)
    
    g_state_machine.arb_won = 1;
    g_state_machine.state = HUBUS_STATE_TRANSFER;
    g_state_machine.tx_byte_index = 0;
    g_state_machine.tx_bit_index = 0;
    
    return 1;
}

/* ====================================================================
   RECOVERY State Handler
   ==================================================================== */

static uint8_t handle_recovery_clk(void)
{
    // In recovery; return to IDLE on next CLK edge
    hubus_data_release();
    g_state_machine.state = HUBUS_STATE_IDLE;
    g_state_machine.clk_tick_count = 0;
    
    return 1;
}

/* ====================================================================
   Frame Parsing
   ==================================================================== */

static uint8_t parse_and_dispatch_rx_frame(void)
{
    hubus_bitbuffer_t *buf = &g_state_machine.rx_bitbuffer;
    
    if (!hubus_bitbuffer_is_complete(buf)) {
        return 0;  // Frame not complete
    }
    
    // Extract frame fields from bit buffer
    uint8_t byte0 = hubus_bitbuffer_get_byte(buf, 0);
    uint8_t sof = (byte0 >> 4) & 0x0F;
    
    // Validate SOF
    if (sof != 0x0F) {
        return 0;  // Invalid frame
    }
    
    // Extract TYPE (3 bits)
    uint8_t type = (byte0 >> 1) & 0x07;
    
    // Extract TARGET_LEN (3 bits, spans byte 0-1)
    uint8_t byte1 = hubus_bitbuffer_get_byte(buf, 1);
    uint8_t target_len = ((byte0 & 0x01) << 2) | ((byte1 >> 6) & 0x03);
    
    // Extract MCID (6 bits, in byte 1)
    uint8_t mcid = (byte1 >> 0) & 0x3F;
    
    // Extract LEN (5 bits, in byte 2)
    uint8_t byte2 = hubus_bitbuffer_get_byte(buf, 2);
    uint8_t len = (byte2 >> 3) & 0x1F;
    
    // Check if addressed
    uint8_t addressed = hubus_frame_is_addressed(
        g_state_machine.mcid,
        mcid,
        target_len
    );
    
    if (!addressed) {
        return 0;  // Not addressed
    }
    
    // Store parsed frame
    g_state_machine.rx_frame.type = type;
    g_state_machine.rx_frame.target_len = target_len;
    g_state_machine.rx_frame.mcid = mcid;
    g_state_machine.rx_frame.len = len;
    
    // Extract payload
    for (uint8_t i = 0; i < len && i < HUBUS_MAX_PAYLOAD; i++) {
        g_state_machine.rx_frame.payload[i] = hubus_bitbuffer_get_byte(buf, 3 + i);
    }
    
    // Extract CRC8
    uint8_t crc_byte_idx = 3 + len;
    g_state_machine.rx_frame.crc = hubus_bitbuffer_get_byte(buf, crc_byte_idx);
    
    // Validate CRC
    uint8_t crc_valid = hubus_crc8_validate(
        (uint8_t *)&g_state_machine.rx_frame.type,
        2 + len  // TYPE+MCID+LEN+PAYLOAD
    );
    
    if (!crc_valid) {
        return 0;  // CRC error
    }
    
    // Dispatch callback based on type
    if (g_state_machine.callback) {
        switch (type) {
            case HUBUS_TYPE_POLL_PENDING:
            case HUBUS_TYPE_REQUEST_INFO:
            case HUBUS_TYPE_REQUEST_DATA:
                // All data request types → single event
                g_state_machine.callback(HUBUS_EVENT_REQUEST_DATA,
                    g_state_machine.rx_frame.payload, len);
                break;
            case HUBUS_TYPE_SEND_COMMAND:
                g_state_machine.callback(HUBUS_EVENT_COMMAND,
                    g_state_machine.rx_frame.payload, len);
                break;
        }
    }
    
    g_state_machine.rx_frame.is_valid = 1;
    return 1;
}

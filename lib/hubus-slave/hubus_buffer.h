/**
 * HuBus Slave Library - Buffer Management
 * 
 * Manages buffered data and status for slave responses.
 * Static allocation; single buffer per data type (no queuing).
 * Overwrite when new data arrives.
 */

#ifndef HUBUS_BUFFER_H
#define HUBUS_BUFFER_H

#include <stdint.h>
#include "hubus_slave_config.h"

/* ====================================================================
   Buffer Structures
   ==================================================================== */

typedef struct {
    uint8_t data[HUBUS_BUFFER_SIZE];
    uint8_t len;
    #if HUBUS_ENABLE_GENERATION_COUNTER
    uint8_t generation;  // Increments each time buffer is overwritten
    #endif
} hubus_data_buffer_t;

typedef struct {
    uint8_t data[HUBUS_STATUS_SIZE];
    uint8_t len;
    #if HUBUS_ENABLE_GENERATION_COUNTER
    uint8_t generation;  // Increments each time buffer is overwritten
    #endif
} hubus_status_buffer_t;

/* ====================================================================
   Buffer Initialization
   ==================================================================== */

/**
 * Initialize all buffers to empty state
 */
void hubus_buffer_init(void);

/* ====================================================================
   Data Buffer (for SLAVE_DATA responses)
   ==================================================================== */

/**
 * Set buffered data (overwrites previous data if any)
 * 
 * @param data  pointer to data to buffer
 * @param len   number of bytes to copy
 * @return      1 on success, 0 if len exceeds HUBUS_BUFFER_SIZE
 */
uint8_t hubus_buffer_set_data(const uint8_t *data, uint8_t len);

/**
 * Get pointer to current buffered data
 * 
 * @param len   output parameter: will contain data length
 * @return      pointer to data buffer, or NULL if empty
 */
const uint8_t *hubus_buffer_get_data(uint8_t *len);

/**
 * Clear buffered data after successful transmission
 */
void hubus_buffer_clear_data(void);

/**
 * Check if data buffer has pending data
 * 
 * @return  1 if data waiting to be sent, 0 if empty
 */
uint8_t hubus_buffer_has_data(void);

#if HUBUS_ENABLE_GENERATION_COUNTER
/**
 * Get generation counter for data buffer
 * Increments each time buffer is overwritten
 */
uint8_t hubus_buffer_get_data_generation(void);
#endif

/* ====================================================================
   Status Buffer (for SLAVE_STATUS responses)
   ==================================================================== */

/**
 * Set status buffer (overwrites previous status if any)
 * 
 * @param status  pointer to status data
 * @param len     number of bytes to copy
 * @return        1 on success, 0 if len exceeds HUBUS_STATUS_SIZE
 */
uint8_t hubus_buffer_set_status(const uint8_t *status, uint8_t len);

/**
 * Get pointer to current status buffer
 * 
 * @param len   output parameter: will contain status length
 * @return      pointer to status buffer, or NULL if empty
 */
const uint8_t *hubus_buffer_get_status(uint8_t *len);

/**
 * Clear status buffer after successful transmission
 */
void hubus_buffer_clear_status(void);

/**
 * Check if status buffer has pending data
 * 
 * @return  1 if status waiting to be sent, 0 if empty
 */
uint8_t hubus_buffer_has_status(void);

#if HUBUS_ENABLE_GENERATION_COUNTER
/**
 * Get generation counter for status buffer
 * Increments each time buffer is overwritten
 */
uint8_t hubus_buffer_get_status_generation(void);
#endif

#endif // HUBUS_BUFFER_H

/**
 * HuBus Slave Library - Buffer Implementation
 */

#include "hubus_buffer.h"
#include <string.h>

// Static buffer instances
static hubus_data_buffer_t g_data_buffer;
static hubus_status_buffer_t g_status_buffer;

void hubus_buffer_init(void)
{
    g_data_buffer.len = 0;
    g_status_buffer.len = 0;
    
    #if HUBUS_ENABLE_GENERATION_COUNTER
    g_data_buffer.generation = 0;
    g_status_buffer.generation = 0;
    #endif
}

/* ====================================================================
   Data Buffer
   ==================================================================== */

uint8_t hubus_buffer_set_data(const uint8_t *data, uint8_t len)
{
    if (len > HUBUS_BUFFER_SIZE) {
        return 0;  // Data too large
    }
    
    #if HUBUS_ENABLE_GENERATION_COUNTER
    // Increment generation on overwrite
    if (g_data_buffer.len > 0 && len > 0) {
        g_data_buffer.generation++;
    }
    #endif
    
    if (len > 0 && data != NULL) {
        memcpy(g_data_buffer.data, data, len);
    }
    
    g_data_buffer.len = len;
    return 1;
}

const uint8_t *hubus_buffer_get_data(uint8_t *len)
{
    if (len != NULL) {
        *len = g_data_buffer.len;
    }
    
    if (g_data_buffer.len == 0) {
        return NULL;
    }
    
    return g_data_buffer.data;
}

void hubus_buffer_clear_data(void)
{
    g_data_buffer.len = 0;
}

uint8_t hubus_buffer_has_data(void)
{
    return (g_data_buffer.len > 0) ? 1 : 0;
}

#if HUBUS_ENABLE_GENERATION_COUNTER
uint8_t hubus_buffer_get_data_generation(void)
{
    return g_data_buffer.generation;
}
#endif

/* ====================================================================
   Status Buffer
   ==================================================================== */

uint8_t hubus_buffer_set_status(const uint8_t *status, uint8_t len)
{
    if (len > HUBUS_STATUS_SIZE) {
        return 0;  // Status too large
    }
    
    #if HUBUS_ENABLE_GENERATION_COUNTER
    // Increment generation on overwrite
    if (g_status_buffer.len > 0 && len > 0) {
        g_status_buffer.generation++;
    }
    #endif
    
    if (len > 0 && status != NULL) {
        memcpy(g_status_buffer.data, status, len);
    }
    
    g_status_buffer.len = len;
    return 1;
}

const uint8_t *hubus_buffer_get_status(uint8_t *len)
{
    if (len != NULL) {
        *len = g_status_buffer.len;
    }
    
    if (g_status_buffer.len == 0) {
        return NULL;
    }
    
    return g_status_buffer.data;
}

void hubus_buffer_clear_status(void)
{
    g_status_buffer.len = 0;
}

uint8_t hubus_buffer_has_status(void)
{
    return (g_status_buffer.len > 0) ? 1 : 0;
}

#if HUBUS_ENABLE_GENERATION_COUNTER
uint8_t hubus_buffer_get_status_generation(void)
{
    return g_status_buffer.generation;
}
#endif

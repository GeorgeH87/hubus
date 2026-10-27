/* Application core with firmware update for test_update (see update_app_core.c). One instance. */
#ifndef UPDATE_APP_CORE_H
#define UPDATE_APP_CORE_H

#include <stdint.h>

void *app_core(void);                                      /* the core, for the simulation */
uint8_t app_start(uint8_t mcid, const uint8_t *info, uint8_t info_len, uint8_t clk, uint8_t data);
void app_feed(uint8_t clk, uint8_t data);                  /* pin levels, like the interrupt handler */
uint8_t app_take_update_request(void);                     /* 1 once after a valid UPDATE_ENTER */
uint8_t app_mcid(void);

#endif /* UPDATE_APP_CORE_H */

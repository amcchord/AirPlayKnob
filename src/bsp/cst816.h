#ifndef CST816_H
#define CST816_H

#include <stdint.h>
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

void Touch_Init(void);
uint8_t getTouch(uint16_t *x, uint16_t *y);
void haptic_play_effect(uint8_t effect_id);

#ifdef __cplusplus
}
#endif

#endif

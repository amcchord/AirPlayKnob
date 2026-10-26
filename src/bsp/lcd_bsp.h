#ifndef LCD_BSP_H
#define LCD_BSP_H

#include "lvgl.h"
#include <stdbool.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_SWIPE_NONE  = 0,
    BSP_SWIPE_LEFT  = 1,
    BSP_SWIPE_RIGHT = 2,
    BSP_SWIPE_UP    = 3,
    BSP_SWIPE_DOWN  = 4,
} bsp_swipe_dir_t;

void lcd_lvgl_Init(void);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
const uint16_t *bsp_get_screen_fb(void);
bsp_swipe_dir_t bsp_pop_swipe_gesture(void);
void bsp_push_swipe_gesture(bsp_swipe_dir_t dir);
bool bsp_was_touch_dragged(void);
uint32_t bsp_get_fps(void);
uint32_t bsp_get_frame_us(void);

#ifdef __cplusplus
}
#endif

#endif

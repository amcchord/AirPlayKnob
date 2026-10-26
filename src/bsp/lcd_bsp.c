#include "lcd_bsp.h"
#include "esp_lcd_sh8601.h"
#include "lcd_config.h"
#include "cst816.h"
#include "esp_heap_caps.h"

static SemaphoreHandle_t lvgl_mux = NULL;
#define LCD_HOST SPI2_HOST

static esp_lcd_panel_io_handle_t amoled_panel_io_handle = NULL;
static uint16_t *s_screen_fb = NULL; // 360x360 live screen mirror in PSRAM (right-side up RGB565_SWAPPED)

static volatile bsp_swipe_dir_t s_pending_swipe = BSP_SWIPE_NONE;
static bool s_tp_pressed = false;
static bool s_tp_dragged = false;
static int16_t s_tp_start_x = 180;
static int16_t s_tp_start_y = 180;
static int16_t s_tp_last_x = 180;
static int16_t s_tp_last_y = 180;
static int64_t s_tp_start_us = 0;

static uint32_t s_fps_frames = 0;
static int64_t s_fps_window_start_us = 0;
static volatile uint32_t s_last_fps = 60;
static volatile uint32_t s_last_frame_us = 0;
static int64_t s_flush_start_us = 0;
static volatile bool s_capture_mirror = false;

static void example_increase_lvgl_tick(void *arg);
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area);
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void example_lvgl_port_task(void *arg);

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
};

void lcd_lvgl_Init(void) {
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_PCLK,
        EXAMPLE_PIN_NUM_LCD_DATA0,
        EXAMPLE_PIN_NUM_LCD_DATA1,
        EXAMPLE_PIN_NUM_LCD_DATA2,
        EXAMPLE_PIN_NUM_LCD_DATA3,
        EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io_handle = NULL;

    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        EXAMPLE_PIN_NUM_LCD_CS,
        example_notify_lvgl_flush_ready,
        &disp_drv);

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    amoled_panel_io_handle = io_handle;
    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));

    lv_init();

    // Allocate 360x360 screen mirror in PSRAM for zero-overhead screenshots
    s_screen_fb = (uint16_t *)heap_caps_calloc(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Double-buffered 30-row internal DMA draw buffers for parallel CPU render + QSPI DMA flush at 60 FPS
    const uint32_t buf_pixels = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT;
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixels);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    disp_drv.sw_rotate = 0;
    disp_drv.rotated = LV_DISP_ROT_NONE;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL, 1);
}

bool bsp_lvgl_lock(int timeout_ms) {
    if (!lvgl_mux) return false;
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void bsp_lvgl_unlock(void) {
    if (!lvgl_mux) return;
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg) {
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (bsp_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            bsp_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static void example_increase_lvgl_tick(void *arg) {
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

const uint16_t *bsp_get_screen_fb(void) {
    if (!s_screen_fb) return NULL;
    s_capture_mirror = true;
    if (bsp_lvgl_lock(50)) {
        lv_obj_invalidate(lv_scr_act());
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(35));
    s_capture_mirror = false;
    return s_screen_fb;
}

bsp_swipe_dir_t bsp_pop_swipe_gesture(void) {
    bsp_swipe_dir_t dir = s_pending_swipe;
    s_pending_swipe = BSP_SWIPE_NONE;
    return dir;
}

void bsp_push_swipe_gesture(bsp_swipe_dir_t dir) {
    s_pending_swipe = dir;
}

bool bsp_was_touch_dragged(void) {
    return s_tp_dragged;
}

uint32_t bsp_get_fps(void) {
    return s_last_fps;
}

uint32_t bsp_get_frame_us(void) {
    return s_last_frame_us;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    if (s_flush_start_us == 0) {
        s_flush_start_us = esp_timer_get_time();
    }
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    const uint32_t total = (uint32_t)w * (uint32_t)h;

    // Only copy to PSRAM screen mirror when a screenshot is actively requested
    if (s_capture_mirror && s_screen_fb) {
        if (area->x1 == 0 && w == EXAMPLE_LCD_H_RES) {
            memcpy(&s_screen_fb[area->y1 * EXAMPLE_LCD_H_RES], color_map, total * sizeof(uint16_t));
        } else {
            const uint16_t *src = (const uint16_t *)color_map;
            for (int y = area->y1; y <= area->y2; y++) {
                memcpy(&s_screen_fb[y * EXAMPLE_LCD_H_RES + area->x1], src, w * sizeof(uint16_t));
                src += w;
            }
        }
    }

    // Fast 32-bit 2-pixel in-place 180-degree reversal (USB-C port at top orientation)
    uint32_t *p32 = (uint32_t *)color_map;
    uint32_t pairs = total >> 1;
    for (uint32_t i = 0, j = pairs - 1; i < j; i++, j--) {
        uint32_t a = p32[i];
        uint32_t b = p32[j];
        p32[i] = (b >> 16) | (b << 16);
        p32[j] = (a >> 16) | (a << 16);
    }
    if (pairs & 1U) {
        uint32_t mid = pairs >> 1;
        uint32_t m = p32[mid];
        p32[mid] = (m >> 16) | (m << 16);
    }

    if (lv_disp_flush_is_last(drv)) {
        int64_t now_us = esp_timer_get_time();
        s_last_frame_us = (uint32_t)(now_us - s_flush_start_us);
        s_flush_start_us = 0;
        s_fps_frames++;
        if (s_fps_window_start_us == 0) {
            s_fps_window_start_us = now_us;
        } else if (now_us - s_fps_window_start_us >= 1000000LL) {
            s_last_fps = (uint32_t)((s_fps_frames * 1000000LL) / (now_us - s_fps_window_start_us));
            s_fps_frames = 0;
            s_fps_window_start_us = now_us;
        }
    }

    const int rx1 = (EXAMPLE_LCD_H_RES - 1) - area->x2;
    const int ry1 = (EXAMPLE_LCD_V_RES - 1) - area->y2;
    const int rx2 = (EXAMPLE_LCD_H_RES - 1) - area->x1;
    const int ry2 = (EXAMPLE_LCD_V_RES - 1) - area->y1;

    esp_lcd_panel_draw_bitmap(panel_handle, rx1, ry1, rx2 + 1, ry2 + 1, color_map);
}

static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area) {
    (void)disp_drv;
    // Always round to full-width even-row horizontal bands so sub_area chunks
    // are guaranteed to have even x1/y1 and odd x2/y2 for SH8601 CASET/RASET.
    area->x1 = 0;
    area->x2 = EXAMPLE_LCD_H_RES - 1;
    area->y1 = area->y1 & ~1;
    area->y2 = area->y2 | 1;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    uint16_t tp_x, tp_y;
    if (getTouch(&tp_x, &tp_y)) {
        if (tp_x >= EXAMPLE_LCD_H_RES) tp_x = EXAMPLE_LCD_H_RES - 1;
        if (tp_y >= EXAMPLE_LCD_V_RES) tp_y = EXAMPLE_LCD_V_RES - 1;
        // 180-degree rotated touch coordinates (USB-C port at top)
        int16_t rx = (int16_t)((EXAMPLE_LCD_H_RES - 1) - tp_x);
        int16_t ry = (int16_t)((EXAMPLE_LCD_V_RES - 1) - tp_y);
        data->point.x = rx;
        data->point.y = ry;
        data->state = LV_INDEV_STATE_PRESSED;

        if (!s_tp_pressed) {
            s_tp_pressed = true;
            s_tp_dragged = false;
            s_tp_start_x = rx;
            s_tp_start_y = ry;
            s_tp_last_x = rx;
            s_tp_last_y = ry;
            s_tp_start_us = esp_timer_get_time();
        } else {
            s_tp_last_x = rx;
            s_tp_last_y = ry;
            int dx = (int)rx - (int)s_tp_start_x;
            int dy = (int)ry - (int)s_tp_start_y;
            if (dx * dx + dy * dy >= 18 * 18) {
                s_tp_dragged = true;
            }
        }
    } else {
        data->point.x = s_tp_last_x;
        data->point.y = s_tp_last_y;
        data->state = LV_INDEV_STATE_RELEASED;

        if (s_tp_pressed) {
            s_tp_pressed = false;
            int dx = (int)s_tp_last_x - (int)s_tp_start_x;
            int dy = (int)s_tp_last_y - (int)s_tp_start_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int64_t dt_ms = (esp_timer_get_time() - s_tp_start_us) / 1000LL;
            if ((adx >= 38 || ady >= 38) && dt_ms <= 900) {
                if (adx >= ady) {
                    s_pending_swipe = (dx < 0) ? BSP_SWIPE_LEFT : BSP_SWIPE_RIGHT;
                } else {
                    s_pending_swipe = (dy < 0) ? BSP_SWIPE_UP : BSP_SWIPE_DOWN;
                }
            }
        }
    }
}

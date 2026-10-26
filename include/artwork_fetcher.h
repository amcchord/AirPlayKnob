#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#define ARTWORK_SIZE 360

void artwork_fetcher_start(void);
const lv_img_dsc_t *artwork_get_dsc(void);
uint32_t artwork_get_seq(void);

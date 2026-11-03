#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ARTWORK_SIZE 360

void artwork_fetcher_start(void);
const lv_img_dsc_t *artwork_get_dsc(void);
uint32_t artwork_get_seq(void);
uint32_t artwork_get_jpeg_seq(void);
size_t artwork_copy_jpeg(uint8_t *dst, size_t max_len, uint32_t *seq_out);

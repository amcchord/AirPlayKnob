#pragma once
#include <stdint.h>
#include <stdbool.h>

void knob_ui_init(void);
void knob_ui_on_rotate(int delta);
void knob_ui_tick(void);

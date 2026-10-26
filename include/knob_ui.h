#pragma once
#include <stdint.h>
#include <stdbool.h>

void knob_ui_init(void);
void knob_ui_on_rotate(int delta);
void knob_ui_tick(void);

// Debug / external UI navigation helpers
void knob_ui_set_tab(int tab);
int  knob_ui_get_tab(void);
void knob_ui_set_whitecap(bool visible);
void knob_ui_on_swipe(int dir);

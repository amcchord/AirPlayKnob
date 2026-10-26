#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Custom Space Grotesk 4-bpp anti-aliased LVGL font family with strong autohinting
LV_FONT_DECLARE(ui_font_sm); // 13px Space Grotesk Bold   (badges, headers, sub-metadata)
LV_FONT_DECLARE(ui_font_md); // 15px Space Grotesk Medium (wheel rows, artist, buttons)
LV_FONT_DECLARE(ui_font_lg); // 18px Space Grotesk Bold   (active wheel slot, HUD value)
LV_FONT_DECLARE(ui_font_xl); // 22px Space Grotesk Bold   (hero track title)

#ifdef __cplusplus
}
#endif

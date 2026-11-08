#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t ssd1306_init(void);
void      ssd1306_clear(void);
void      ssd1306_set_contrast(uint8_t contrast);

// Render one PCD8544 frame (page-major, `banks` pages of `width` bytes) into the
// centred 84x48 window of the 128x64 panel. `mode` follows Mad2::lcd_mode:
// 0 = blank, 1 = all on, 2 = normal, 3 = inverse.
void ssd1306_show_lcd(const uint8_t* fb, int width, int height, int banks, int mode);

#pragma once
#include <stdint.h>
#include "pixel_font.h"

// 页式帧缓冲；每行最多21个ASCII字符，越界字符不写入相邻页。
static inline void oled_text_line(uint8_t *frame, unsigned page, const char *text) {
    if (page >= 8 || !text) return;
    for (unsigned x = 0; *text && x + PIXEL_FONT_WIDTH <= 128; x += 6, ++text) {
        unsigned c = (unsigned char)*text;
        if (c < PIXEL_FONT_FIRST || c > PIXEL_FONT_LAST) c = '?';
        for (unsigned col = 0; col < PIXEL_FONT_WIDTH; ++col)
            frame[page * 128 + x + col] = pixel_font[c - PIXEL_FONT_FIRST][col];
    }
}

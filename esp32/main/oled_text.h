#pragma once
#include <stdint.h>
#include "pixel_font.h"

// 页式帧缓冲；在指定 (page, start_x) 处绘制单行 ASCII 字符，越界字符不写入相邻页。
static inline void oled_text_at(uint8_t *frame, unsigned page, unsigned start_x, const char *text) {
    if (page >= 8 || !text || start_x >= 128) return;
    for (unsigned x = start_x; *text && x + PIXEL_FONT_WIDTH <= 128; x += 6, ++text) {
        unsigned c = (unsigned char)*text;
        if (c < PIXEL_FONT_FIRST || c > PIXEL_FONT_LAST) c = '?';
        for (unsigned col = 0; col < PIXEL_FONT_WIDTH; ++col) {
            uint8_t pixels = pixel_font[c - PIXEL_FONT_FIRST][col];
            // 原字体的波浪号靠顶；OLED估算符号下移2像素，与数字中线对齐。
            frame[page * 128 + x + col] = c == '~' ? (uint8_t)(pixels << 2) : pixels;
        }
    }
}

static inline void oled_text_line(uint8_t *frame, unsigned page, const char *text) {
    oled_text_at(frame, page, 0, text);
}

// 在指定 (page, x) 起始处绘制 32x32 图标（占用连续 4 个 page，每个 page 绘制 32 列）。
static inline void oled_draw_icon32(uint8_t *frame, unsigned page, unsigned x, const uint8_t *bitmap) {
    if (page > 4 || x + 32 > 128 || !frame || !bitmap) return;
    for (unsigned p = 0; p < 4; ++p) {
        for (unsigned col = 0; col < 32; ++col) {
            frame[(page + p) * 128 + x + col] |= bitmap[p * 32 + col];
        }
    }
}

// 在指定 (page, x) 起始处绘制 16x16 图标（占用上下连续 2 个 page，每个 page 绘制 16 列）。
static inline void oled_draw_icon16(uint8_t *frame, unsigned page, unsigned x, const uint8_t *bitmap) {
    if (page >= 7 || x + 16 > 128 || !frame || !bitmap) return;
    for (unsigned col = 0; col < 16; ++col) {
        frame[page * 128 + x + col] |= bitmap[col];
        frame[(page + 1) * 128 + x + col] |= bitmap[16 + col];
    }
}

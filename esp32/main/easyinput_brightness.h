#pragma once

// 二次曲线拉开低亮区与高亮区，100%不改变各外设既有输出上限。
static inline unsigned easyinput_brightness_scale(unsigned maximum, int percent) {
    unsigned p = percent < 0 ? 0 : percent > 100 ? 100 : (unsigned)percent;
    return (maximum * p * p + 5000) / 10000;
}

static inline unsigned easyinput_brightness_channel(unsigned channel, int percent) {
    if (!channel) return 0;
    unsigned capped = channel > 50 ? 50 : channel;
    unsigned maximum = 1 + (capped - 1) / 4;
    // 不强制每个非零颜色至少一级，否则暗色通道无法调暗。
    return easyinput_brightness_scale(maximum, percent);
}

// SSD1315试验：Phase1保持1，Phase2为1～15；100%恢复原预充电0xF1。
static inline unsigned easyinput_oled_precharge(int percent) {
    return ((1 + easyinput_brightness_scale(14, percent)) << 4) | 0x01;
}

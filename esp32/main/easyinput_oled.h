#pragma once

// J4: 3V3 / GPIO44(SCL) / GPIO43(SDA) / GND，仅用于128x64 SSD1315。
void easyinput_oled_init(void);

// 桌面灵宠互动：摸摸头/按键交互，触发开心害羞表情并唤醒睡眠
void easyinput_oled_headpat(void);

"""实际帧缓冲绘制的越界、非法字符与页隔离检查；不访问硬件。"""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OledTextTest(unittest.TestCase):
    def test_right_column_labels_fit(self):
        source = (ROOT / 'main/easyinput_oled.c').read_text(encoding='utf-8')
        labels = source[source.index('static const char *network_short_label'):source.index('// 32x32')]
        render = source[source.index('static void render('):source.index('static esp_err_t start_panel(')]
        text = re.findall(r'return "([^"]*)"', labels)
        for line in render.splitlines():
            if re.search(r'\b(status|hint) = ', line):
                text.extend(re.findall(r'"([^"]*)"', line))
        self.assertTrue(text)
        # 右侧从 x=36 开始；5 列字形、6 列步距，只能完整容纳 15 字符。
        for label in text:
            with self.subTest(label=label):
                self.assertTrue(label.isascii())
                self.assertLessEqual(len(label), 15)

    def test_render_priorities_and_sleep(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('缺少主机C编译器，实际状态渲染函数未执行')
        source = (ROOT / 'main/easyinput_oled.c').read_text(encoding='utf-8')
        labels = source[source.index('static const char *network_short_label'):source.index('// 32x32')]
        render = source[source.index('static void render('):source.index('static esp_err_t start_panel(')]
        battery = source[source.index('static int battery_percent_estimate('):source.index('static void render(')]
        headpat = source[source.index('void easyinput_oled_headpat('):source.index('static esp_err_t commands(')]
        avatars = sorted(set(re.findall(r'\bs_robot_\w+', render)))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
typedef enum { NET_SETUP, NET_WIFI, NET_SERVICE, NET_PAIR, NET_READY } network_state_t;
typedef enum { VOICE_DIAL_VOLUME, VOICE_DIAL_BRIGHTNESS, VOICE_DIAL_RADIO } voice_dial_mode_t;
typedef enum { LED_VOICE_IDLE, LED_VOICE_LISTENING, LED_VOICE_END_CANDIDATE,
    LED_VOICE_TRANSCRIBING, LED_VOICE_THINKING, LED_VOICE_BUFFERING,
    LED_VOICE_SPEAKING, LED_VOICE_ERROR, LED_VOICE_REPLAY_WAIT } led_voice_t;
static int net = NET_READY, mv = 3900;
static bool muted, paused, external, charging, halted;
static int64_t now = 100000000LL, s_last_active_time;
static atomic_llong s_headpat_until;
static bool s_is_sleeping;
static char rows[8][24];
static voice_dial_mode_t dial;
static voice_dial_mode_t voice_dial_mode(void) { return dial; }
static void led_status_oled_failure(const char **status, const char **hint) {
    *status = "NET: DNS ERROR"; *hint = "CHECK NETWORK";
}
static int network_feedback_state(void) { return net; }
static int64_t esp_timer_get_time(void) { return now; }
static bool easyinput_board_muted(void) { return muted; }
static bool voice_playback_paused(void) { return paused; }
static bool voice_halted(void) { return halted; }
static int voice_volume(void) { return 100; }
static int easyinput_board_brightness(void) { return 100; }
static void easyinput_board_power(int *v, bool *e, bool *c) {
    *v = mv; *e = external; *c = charging;
}
static void oled_text_at(uint8_t *f, unsigned p, unsigned x, const char *s) {
    (void)f;
    assert(p < 8 && strlen(s) <= (128-x+1)/6);
    snprintf(rows[p], sizeof(rows[p]), "%s", s);
}
static void oled_text_line(uint8_t *f, unsigned p, const char *s) { oled_text_at(f,p,0,s); }
static void oled_draw_icon32(uint8_t *f, unsigned p, unsigned x, const uint8_t *b) {
    (void)f; (void)p; (void)x; (void)b;
}
'''
        harness += '\n'.join('static const uint8_t ' + avatar + '[128] = {0};' for avatar in avatars)
        harness += labels + headpat + battery + render + r'''
int main(void) {
    uint8_t frame[1024];
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[0], "TinyPal"));
    assert(!strcmp(rows[5], ">VOL 100%  BRI 100%"));
    dial = VOICE_DIAL_BRIGHTNESS;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[5], " VOL 100% >BRI 100%"));
    assert(!strcmp(rows[7], "BRIGHTNESS: 100%"));
    muted = true;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[2], "MIC: MUTED"));
    muted = false; dial = VOICE_DIAL_RADIO;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[7], "RADIO: TURN TO PICK"));
    dial = VOICE_DIAL_VOLUME;
    assert(battery_percent_estimate(0) == -1);
    assert(battery_percent_estimate(-1) == -1);
    assert(battery_percent_estimate(3200) == 0);
    assert(battery_percent_estimate(4215) == 100);
    assert(battery_percent_estimate(4216) == 100);
    int previous = 0;
    for (int v = 3300; v <= 4300; ++v) {
        int value = battery_percent_estimate(v);
        assert(value >= previous && value >= 0 && value <= 100);
        previous = value;
    }
    external = true; mv = 4215;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[6], "BAT ~100% 4.21V USB"));
    charging = true;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[6], "BAT ~100% 4.21V CHG"));
    mv = 0;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[6], "BAT --% --V CHG"));
    mv = 3900; external = charging = false;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[2], "AI: READY"));
    now += 46000000LL;
    render(frame, LED_VOICE_IDLE); assert(s_is_sleeping);
    easyinput_oled_headpat();
    render(frame, LED_VOICE_IDLE); assert(!s_is_sleeping);
    charging = muted = true;
    render(frame, LED_VOICE_SPEAKING);
    assert(!strcmp(rows[2], "MIC: MUTED") && !s_is_sleeping);
    muted = false; paused = true;
    render(frame, LED_VOICE_BUFFERING);
    assert(!strcmp(rows[2], "AUDIO: PAUSED"));
    render(frame, LED_VOICE_REPLAY_WAIT);
    assert(!strcmp(rows[2], "REPLAY: WAIT") && !strcmp(rows[3], "AUTO RESUME"));
    render(frame, LED_VOICE_ERROR); assert(!strcmp(rows[2], "NET: DNS ERROR"));
    halted = true;
    render(frame, LED_VOICE_IDLE);
    assert(!strcmp(rows[2], "VOICE: STOPPED") && !s_is_sleeping);
    halted = false;
    paused = charging = false;
    for (int voice = LED_VOICE_IDLE; voice <= LED_VOICE_REPLAY_WAIT; ++voice)
        for (net = NET_SETUP; net <= NET_READY; ++net) render(frame, voice);
    net = NET_WIFI; now += 46000000LL;
    render(frame, LED_VOICE_IDLE);
    assert(!s_is_sleeping && !strcmp(rows[2], "AI: NOT READY"));
    net = NET_READY; mv = 3400;
    render(frame, LED_VOICE_IDLE); assert(!s_is_sleeping);
    assert(!strcmp(rows[2], "BAT: LOW"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp/easyinput', prefix='oled-state-') as directory:
            src, exe = Path(directory) / 'test.c', Path(directory) / 'test.exe'
            src.write_text(harness, encoding='utf-8')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Werror', str(src), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

    def test_frame_boundaries_and_invalid_character(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('缺少主机C编译器，实际绘制函数未执行')
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp/easyinput', prefix='oled-test-') as directory:
            src = Path(directory) / 'test.c'
            exe = Path(directory) / 'test.exe'
            src.write_text(r'''
#include <assert.h>
#include <string.h>
#include "oled_text.h"
int main(void) {
    unsigned char guarded[1026], before[1026];
    memset(guarded, 0, sizeof(guarded));
    guarded[0] = guarded[1025] = 0xa5;
    unsigned char *frame = guarded + 1;
    oled_text_line(frame, 7, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    assert(guarded[0] == 0xa5 && guarded[1025] == 0xa5);
    for (int i = 0; i < 7*128; ++i) assert(frame[i] == 0);
    assert(frame[7*128+125] == 0 && frame[7*128+127] == 0);
    memcpy(before, guarded, sizeof(before));
    oled_text_line(frame, 8, "BAD");
    oled_text_line(frame, 0, NULL);
    assert(!memcmp(before, guarded, sizeof(before)));
    oled_text_line(frame, 0, "\xff");
    assert(!memcmp(frame, pixel_font['?'-PIXEL_FONT_FIRST], 5));
    oled_text_line(frame, 1, "A");
    assert(!memcmp(frame+128, pixel_font['A'-PIXEL_FONT_FIRST], 5));
    // 检查指定起始列绘制。
    oled_text_at(frame, 2, 36, "OK");
    assert(!memcmp(frame + 2 * 128 + 36, pixel_font['O'-PIXEL_FONT_FIRST], 5));
    // 波浪号居中，后面的数字仍使用原始字形，不能写入相邻页。
    oled_text_line(frame, 4, "~100%");
    for (int col = 0; col < 5; ++col)
        assert(frame[4*128+col] == (pixel_font['~'-PIXEL_FONT_FIRST][col] << 2));
    assert(!memcmp(frame + 4*128 + 6, pixel_font['1'-PIXEL_FONT_FIRST], 5));
    for (int col = 0; col < 128; ++col) assert(frame[5*128+col] == 0);
    // 检查图标叠加绘制，不覆盖已有文字像素。
    unsigned char icon[128];
    memset(icon, 0x55, sizeof(icon));
    oled_draw_icon32(frame, 0, 2, icon);
    assert(frame[0 * 128 + 2] == (pixel_font['?'-PIXEL_FONT_FIRST][2] | 0x55));
    assert(frame[3 * 128 + 2 + 31] == 0x55);
    // 越界页不能修改帧缓冲。
    oled_draw_icon32(frame, 5, 0, icon);
    return 0;
}
''', encoding='utf-8')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                           str(src), str(ROOT / 'main/pixel_font.c'), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

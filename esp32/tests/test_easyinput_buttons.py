# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""使用模拟GPIO与时钟，执行生产按键任务，验证恢复键隔离和消抖。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EasyInputButtons(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("主机C编译器不可用")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        headers = {
            "driver/gpio.h": r'''
#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
int gpio_get_level(int pin);
esp_err_t gpio_config(const gpio_config_t *cfg);
''',
            "freertos/FreeRTOS.h": r'''
#pragma once
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
''',
            "freertos/task.h": r'''
#pragma once
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t fn, const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(unsigned);
''',
            "esp_log.h": r'''
#pragma once
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
''',
            "esp_timer.h": r'''
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
''',
        }
        for relative, text in headers.items():
            path = out / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include "button.h"
#include "driver/gpio.h"
#include "freertos/task.h"
static int scenario, short_count, long_count, cancel_count, mute_count, replay_count, edges;
static unsigned now_ms, end_ms;
static TaskFunction_t task;
static jmp_buf finish;
static bool low(int pin, unsigned start, unsigned end, int wanted) {
    return pin == wanted && now_ms >= start && now_ms < end;
}
int gpio_get_level(int pin) {
    bool down = false;
    if (scenario == 0) down = low(pin, 100, 6200, 2);
    if (scenario == 1) {
        down = low(pin, 100, 180, 47) || low(pin, 300, 380, 41) ||
               low(pin, 500, 580, 48) || low(pin, 700, 780, 38) ||
               low(pin, 900, 980, 1) || low(pin, 1100, 1180, 6) ||
               low(pin, 1300, 1380, 7) || low(pin, 1500, 1580, 18);
    }
    if (scenario == 2) down = low(pin, 100, 5300, 48);
    if (scenario == 3) down = low(pin, 100, 120, 2);
    return !down;
}
esp_err_t gpio_config(const gpio_config_t *cfg) {
    assert(cfg->pin_bit_mask & (1ULL << 2));
    assert(cfg->pin_bit_mask & (1ULL << 48));
    return ESP_OK;
}
int xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    task = fn;
    return 1;
}
void vTaskDelay(unsigned ms) {
    now_ms += ms;
    if (now_ms >= end_ms) longjmp(finish, 1);
}
int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
void voice_cancel(void) { ++cancel_count; }
void voice_abort(void) { ++cancel_count; }
void voice_toggle_mute(void) { ++mute_count; }
void voice_replay(void) { ++replay_count; }
static void on_short(void) { ++short_count; }
static void on_long(void) { ++long_count; }
static bool on_press(bool pressed) {
    ++edges;
    return false;  /* 模拟断网、静音或音频初始化失败时语音未认领。 */
}
int main(int argc, char **argv) {
    assert(argc == 2);
    scenario = atoi(argv[1]);
    end_ms = scenario == 0 ? 6400 : scenario == 2 ? 5600 : 1800;
    assert(button_init(on_short, NULL, on_long));
    button_set_press_cb(on_press);
    if (!setjmp(finish)) task(NULL);
    if (scenario == 0) assert(edges == 2 && !short_count && !long_count && !cancel_count);
    if (scenario == 1) assert(short_count == 1 && !long_count && mute_count == 1 && replay_count == 1 && cancel_count == 1 && !edges);
    if (scenario == 2) assert(long_count == 1 && cancel_count == 1 && !short_count);
    if (scenario == 3) assert(!edges && !short_count && !long_count);
    return 0;
}
'''
        (out / "buttons.c").write_text(harness, encoding="utf-8")
        cls.binary = out / "buttons"
        result = subprocess.run([
            *cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-DCONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2=1", "-DCONFIG_HOMEHUB_VOICE=1",
            "-DCONFIG_HOMEHUB_BUTTON_GPIO=48", "-I", str(out), "-I", str(ROOT / "main"),
            "-I", str(ROOT / "managed_components/espressif__cjson/cJSON"),
            str(ROOT / "main/button.c"), str(out / "buttons.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unclaimed_s1_never_resets(self):
        self.run_case(0)

    def test_key_routes_and_diagnostic_keys(self):
        self.run_case(1)

    def test_s8_reset_fires_once_without_short_press(self):
        self.run_case(2)

    def test_single_poll_glitch_is_ignored(self):
        self.run_case(3)

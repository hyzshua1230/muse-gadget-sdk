"""检查播放所有权边界，并用模拟时钟执行实际电台状态函数。"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    begin = source.index("{", start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class RadioOwnership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.voice = (ROOT / "main/voice.c").read_text(encoding="utf-8")
        cls.radio = (ROOT / "main/radio.c").read_text(encoding="utf-8")

    def test_failed_radio_pause_releases_claim_without_starting_ai(self):
        # 通知也会申请电台输出锁；此测试只约束原AI语音主流程。
        task = self.voice[self.voice.index("static void voice_task("):]
        failed = block(task, "if (!radio_pause(true))")
        self.assertIn("radio_pause(false);", failed)
        self.assertIn("voice_wake_stop()", failed)
        self.assertIn("continue;", failed)
        self.assertNotIn("run_turn()", failed)

    def test_player_timeout_defers_release_until_idle(self):
        pending = block(self.voice, "if (radio_release_pending)")
        waiting = block(pending, "if (!voice_player_wait(0))")
        self.assertIn("continue;", waiting)
        self.assertNotIn("radio_pause(false)", waiting)
        self.assertLess(pending.index("voice_player_wait(0)"), pending.index("radio_pause(false)"))
        self.assertIn("radio_release_pending = true;", self.voice)
        task = self.voice[self.voice.index("static void voice_task("):]
        self.assertLess(task.index("if (radio_release_pending)"), task.index("xQueueReceive(s_events"))

    def test_wakeup_checks_radio_before_claiming_recording(self):
        claim = self.voice[self.voice.index("if (!voice_wake_poll())"):]
        claim = claim[:claim.index("s_auto_record = true;")]
        self.assertLess(claim.index("radio_enabled()"), claim.index("atomic_compare_exchange_strong"))

    def test_buffered_mp3_is_not_blocked_by_network_refill(self):
        refill = block(self.radio, "if (used < 4096)")
        self.assertRegex(refill, r"xStreamBufferReceive\([^;]+,0\)")
        self.assertNotIn("pdMS_TO_TICKS", refill)

    def test_asr_buffer_covers_connection_with_headroom(self):
        header = (ROOT / "components/muse/muse_qwen_asr.h").read_text(encoding="utf-8")
        session = (ROOT / "components/muse/muse_chat_session.cpp").read_text(encoding="utf-8")
        asr = (ROOT / "components/muse/muse_qwen_asr.cpp").read_text(encoding="utf-8")
        timeout = int(re.search(r"#define MUSE_QWEN_ASR_CONNECT_TIMEOUT_MS (\d+)", header)[1])
        capacity = re.search(r"#define IN_BYTES \(MIC_RATE \* 2 \* \(MUSE_QWEN_ASR_CONNECT_TIMEOUT_MS / 1000 \+ (\d+)\)\)", session)
        self.assertIsNotNone(capacity)
        seconds = timeout // 1000 + int(capacity[1])
        # 15秒慢连接仍不丢音；截止之后还需能容纳至少一个100ms音频块。
        self.assertGreaterEqual(seconds * 16000 * 2, (timeout / 1000 + 0.1) * 16000 * 2)
        self.assertIn("MUSE_QWEN_ASR_CONNECT_TIMEOUT_MS * 1000LL", asr)

    def test_station_flash_write_is_polled_from_internal_stack(self):
        dial = block(self.voice, 'static void dial_task(')
        self.assertIn('radio_poll_storage();', dial)
        self.assertNotIn('radio_poll_storage();', block(self.radio, 'static void worker('))
        preview = block(self.radio, 'bool radio_select_steps(')
        self.assertNotIn('save_at', preview)
        self.assertNotIn('config_set_str', preview)
        init = block(self.radio, 'esp_err_t radio_init(')
        self.assertIn('load_station()', init)
        self.assertNotIn('set_enabled(true)', init)


class RadioState(unittest.TestCase):
    def test_station_prompt_assets_cover_all_stations(self):
        source = (ROOT / 'main/radio.c').read_text(encoding='utf-8')
        count = int(re.search(r'urls\[(\d+)\]\[2\]', source)[1])
        header = (ROOT / 'main/radio_assets/stations.h').read_text(encoding='utf-8')
        offsets = [int(x) for x in re.findall(r'\d+', header.split('{', 1)[1])]
        pcm = (ROOT / 'main/radio_assets/stations.pcm').read_bytes()
        rate = int(re.search(r'#define RADIO_PROMPT_RATE (\d+)', header)[1])
        self.assertEqual(rate, 8000)
        self.assertEqual(len(offsets), count + 1)
        self.assertEqual(offsets[0], 0)
        self.assertEqual(offsets[-1] * 2, len(pcm))
        for start, end in zip(offsets, offsets[1:]):
            self.assertGreater(end, start)
            self.assertLessEqual((end - start) / rate, 4.2)
            self.assertNotEqual(set(pcm[start * 2:end * 2]), {0})

    def test_preview_uses_candidate_and_cancels_expired_prompt(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest('主机C编译器不可用，台名预览执行测试未验证')
        source = (ROOT / 'main/radio.c').read_text(encoding='utf-8')
        state = source[source.index('typedef struct {'):source.index('static portMUX_TYPE')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "radio_assets/stations.h"
#include <limits.h>
#define PROMPT_REPEAT (48000 / RADIO_PROMPT_RATE)
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static uint8_t stations_start[2103360];
typedef struct {
    unsigned preview;
    const int16_t *prompt;
    size_t prompt_pos, prompt_samples;
    int32_t out[192*2];
    float duck;
} audio_t;
typedef int esp_err_t;
static int32_t observed[192*2];
static esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    for (size_t i = 0; i < count * 2; ++i) observed[i] = frames[i];
    return 0;
}
'''
        harness += state + block(source, 'static state_t snapshot(') + block(source, 'static void preview_update(') + block(source, 'static esp_err_t output(')
        harness += r'''
int main(void) {
    audio_t audio = {0};
    s.selecting = true; s.preview = 1; s.preview_at = 800000; s.select_until = 5000000;
    now = 799999; preview_update(&audio); assert(!audio.prompt);
    now = 800000;
    for (int i = 0; i < 12; ++i) {
        s.candidate = i; ++s.preview; preview_update(&audio);
        assert(audio.prompt == (const int16_t *)stations_start + radio_prompt_offsets[i]);
        assert(audio.prompt_samples == radio_prompt_offsets[i+1] - radio_prompt_offsets[i]);
        assert(audio.prompt_pos == 0);
    }
    audio.prompt_pos = 42; preview_update(&audio); assert(audio.prompt_pos == 42);
    ++s.preview; s.preview_at = now + 800000; preview_update(&audio); assert(!audio.prompt);
    now = s.select_until; preview_update(&audio); assert(!audio.prompt && !s.selecting);
    const int16_t tiny[] = {100, -100};
    audio.prompt = tiny; audio.prompt_samples = 2; audio.prompt_pos = 0;
    assert(PROMPT_REPEAT == 6);
    assert(output(&audio, 12, 1, true) == 0);
    for (int i = 0; i < 24; ++i) assert(observed[i] == (i < 12 ? 6553600 : -6553600));
    assert(audio.prompt_pos == 12);
    return 0;
}
'''
        out = Path(tempfile.mkdtemp(prefix='radio-preview-', dir=ROOT / 'tmp/easyinput'))
        cfile, exe = out / 'preview.c', out / 'preview.exe'
        cfile.write_text(harness, encoding='utf-8')
        subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                            str(cfile), '-o', str(exe)], check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)

    def test_station_persistence_and_debounce(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest('主机C编译器不可用，频道持久化执行测试未验证')
        source = (ROOT / 'main/radio.c').read_text(encoding='utf-8')
        state = source[source.index('typedef struct {'):source.index('static portMUX_TYPE')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define STATION_COUNT 12
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static char stored[16];
static int writes;
static bool missing, fail_write, change_during_write;
static bool config_get_str(const char *key, char *value, size_t cap) {
    assert(!strcmp(key, "radio_station"));
    if (missing || strlen(stored) >= cap) return false;
    strcpy(value, stored); return true;
}
void radio_next(void);
static bool config_set_str(const char *key, const char *value) {
    assert(!strcmp(key, "radio_station")); ++writes;
    if (fail_write) return false;
    strcpy(stored, value);
    if (change_during_write) { change_during_write = false; radio_next(); }
    return true;
}
'''
        harness += state + '\n'.join(block(source, name) for name in (
            'static state_t snapshot(', 'static void select_station_locked(',
            'static int load_station(', 'void radio_poll_storage(',
            'void radio_next(', 'void radio_select_mode(', 'bool radio_select_steps(',
            'bool radio_select_confirm(',
        ))
        harness += r'''
int main(void) {
    missing = true; assert(load_station() == 0); missing = false;
    const char *bad[] = {"", "-1", "12", "999", "1x", "123456789"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        strcpy(stored, bad[i]); assert(load_station() == 0);
    }
    for (int i = 0; i < 12; ++i) {
        snprintf(stored, sizeof(stored), "%d", i); assert(load_station() == i);
    }
    s = (state_t){.ready = true};
    radio_next(); now = 1999999; radio_poll_storage(); assert(writes == 0);
    now = 2000000; radio_poll_storage(); assert(writes == 1 && !strcmp(stored, "1"));
    radio_next(); now = 3000000; radio_next();
    now = 4999999; radio_poll_storage(); assert(writes == 1);
    now = 5000000; radio_poll_storage(); assert(writes == 2 && !strcmp(stored, "3"));
    radio_select_mode(); radio_select_steps(4);
    now += 5000001; assert(!radio_select_confirm());
    radio_poll_storage(); assert(writes == 2 && s.station == 3);
    radio_select_mode(); radio_select_steps(4); assert(radio_select_confirm());
    now += 2000000; radio_poll_storage(); assert(writes == 3 && !strcmp(stored, "7"));
    radio_next(); radio_select_mode(); radio_select_steps(-1); radio_select_confirm();
    now += 2000000; radio_poll_storage(); assert(writes == 3);
    fail_write = true; radio_next(); now += 2000000; radio_poll_storage();
    assert(writes == 4 && !strcmp(stored, "7") && s.saved_station == 7);
    fail_write = false; radio_next(); now += 2000000; radio_poll_storage();
    assert(writes == 5 && !strcmp(stored, "9"));
    change_during_write = true; radio_next(); now += 2000000; radio_poll_storage();
    assert(!strcmp(stored, "10") && s.station == 11 && s.save_at > now);
    now += 2000000; radio_poll_storage(); assert(!strcmp(stored, "11"));
    assert(!s.enabled);
    return 0;
}
'''
        out = Path(tempfile.mkdtemp(prefix='radio-storage-', dir=ROOT / 'tmp/easyinput'))
        cfile, exe = out / 'storage.c', out / 'storage.exe'
        cfile.write_text(harness, encoding='utf-8')
        subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', str(cfile), '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)

    def test_actual_state_functions_with_fake_clock(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("主机C编译器不可用，状态执行测试未验证")
        source = (ROOT / "main/radio.c").read_text(encoding="utf-8")
        state = source[source.index("typedef struct {"):source.index("static portMUX_TYPE")]
        functions = "\n".join(block(source, name) for name in (
            "static state_t snapshot(", "bool radio_enabled(", "static void select_station_locked(", "static void set_enabled(",
            "void radio_toggle(", "void radio_next(", "void radio_select_mode(",
            "bool radio_select_steps(", "bool radio_select_confirm(", "bool radio_pause(",
            "static bool current(unsigned epoch) {",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define STATION_COUNT 12
static int64_t now;
static bool release_output;
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(unsigned ms);
'''
        harness += state + functions
        harness += r'''
static void vTaskDelay(unsigned ms) {
    now += ms * 1000LL;
    if (release_output) s.output = false;
}
int main(void) {
    s.ready = true;
    radio_toggle();
    assert(radio_enabled() && current(s.epoch));
    s.deadline = now + 3600000000LL;
    int64_t deadline = s.deadline;
    radio_next();
    assert(s.station == 1 && s.deadline == deadline);
    unsigned old_epoch = s.epoch;
    s.output = true;
    release_output = true;
    assert(radio_pause(true));
    assert(s.blocked && !current(old_epoch));
    assert(radio_pause(false) && s.deadline == deadline);
    assert(current(s.epoch));
    s.output = true;
    release_output = false;
    assert(!radio_pause(true));
    assert(s.blocked);
    assert(radio_pause(false));
    assert(!s.blocked && s.deadline == deadline);
    s.output = false;
    radio_select_mode();
    assert(radio_select_steps(11) && s.candidate == 0);
    assert(radio_select_confirm() && s.station == 0);
    assert(s.deadline == deadline);
    radio_select_mode();
    radio_select_steps(1);
    now += 5000001;
    assert(!radio_select_confirm() && s.station == 0);
    now = deadline;
    assert(!current(s.epoch));
    radio_toggle();
    assert(!radio_enabled());
    radio_toggle();
    assert(s.deadline == 0 && current(s.epoch));
    return 0;
}
'''
        # 产物保留在工作区，避免使用用户目录或自动清理其他路径。
        out = Path(tempfile.mkdtemp(prefix="radio-lifecycle-", dir=ROOT / "tmp/easyinput"))
        cfile, exe = out / "state.c", out / "state.exe"
        cfile.write_text(harness, encoding="utf-8")
        subprocess.run(cc + ["-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                             str(cfile), "-o", str(exe)], check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()

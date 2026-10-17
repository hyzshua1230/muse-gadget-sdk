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
        failed = block(self.voice, "if (!radio_pause(true))")
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


class RadioState(unittest.TestCase):
    def test_actual_state_functions_with_fake_clock(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("主机C编译器不可用，状态执行测试未验证")
        source = (ROOT / "main/radio.c").read_text(encoding="utf-8")
        state = source[source.index("typedef struct {"):source.index("static portMUX_TYPE")]
        functions = "\n".join(block(source, name) for name in (
            "static state_t snapshot(", "bool radio_enabled(", "static void set_enabled(",
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
    assert(radio_select_steps(1) && s.candidate == 0);
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

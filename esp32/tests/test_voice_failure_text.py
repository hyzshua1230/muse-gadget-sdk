"""错误显示只输出固定短语，覆盖原因优先级与未知输入；不联网。"""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VoiceFailureTextTest(unittest.TestCase):
    def test_all_labels_fit_oled_right_column(self):
        source = (ROOT / 'main/voice_failure_text.h').read_text(encoding='utf-8')
        pairs = re.findall(r'\{"([^"]*)", "([^"]*)"\}', source)
        self.assertGreater(len(pairs), 10)
        for pair in pairs:
            for label in pair:
                self.assertTrue(label.isascii())
                self.assertLessEqual(len(label), 15, label)

    def test_real_classifier(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('缺少主机C编译器，错误分类函数未执行')
        source = r'''
#include <assert.h>
#include "voice_failure_text.h"
int main(void) {
    const char *inputs[] = {
        "QWEN TTS FAILED (dns)", "QWEN ASR CONNECTION FAILED (timeout)",
        "QWEN ASR FAILED (auth)", "QWEN TTS FAILED (quota)",
        "QWEN TTS FAILED (rate_limit)", "QWEN TTS FAILED (forbidden)",
        "QWEN TTS FAILED (transport)", "QWEN TTS FAILED (buffer_limit)",
        "QWEN TTS FAILED (format)", "SPEAKER STOP TIMEOUT",
        "WAKE INIT FAILED", "MICROPHONE STOP FAILED", "DATE/SOURCES UNAVAILABLE",
        "CAN'T REACH MUSE", "unknown error with https://private.invalid/", 0
    };
    const char *labels[] = {
        "NET: DNS ERROR", "NET: TIMEOUT", "API: CHECK KEY", "API: NO QUOTA",
        "API: RATE LIMIT", "API: FORBIDDEN", "NET: LINK ERROR", "AUDIO: MEMORY",
        "AUDIO: FORMAT", "SPK: FAILED", "WAKE: FAILED", "VOICE: STOPPED",
        "BRIEF: INVALID", "MUSE: OFFLINE", "AI: ERROR", "AI: ERROR"
    };
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        voice_failure_text_t result = voice_failure_text(inputs[i]);
        assert(!strcmp(result.status, labels[i]));
        assert(strlen(result.status) <= 15 && strlen(result.hint) <= 15);
        assert(!strstr(result.status, "private.invalid"));
        assert(!strstr(result.hint, "private.invalid"));
    }
    assert(strcmp(voice_failure_text("QWEN TTS FAILED (forbidden)").status,
                  voice_failure_text("QWEN TTS FAILED (quota)").status));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp/easyinput', prefix='voice-failure-') as directory:
            src, exe = Path(directory) / 'test.c', Path(directory) / 'test.exe'
            src.write_text(source, encoding='utf-8')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                                      str(src), '-o', str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

"""实际帧缓冲绘制的越界、非法字符与页隔离检查；不访问硬件。"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OledTextTest(unittest.TestCase):
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
    return 0;
}
''', encoding='utf-8')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                           str(src), str(ROOT / 'main/pixel_font.c'), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

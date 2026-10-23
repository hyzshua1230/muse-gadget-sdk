"""亮度曲线须单调且不突破原柔和上限，不访问硬件。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EasyInputBrightnessTest(unittest.TestCase):
    def test_curve_bounds_and_channel_ceiling(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest('缺少主机C编译器，亮度映射函数未执行')
        source = r'''
#include <assert.h>
#include "easyinput_brightness.h"
int main(void) {
    for (unsigned channel = 0; channel <= 255; ++channel) {
        unsigned cap = channel > 50 ? 50 : channel;
        unsigned old_max = cap ? 1 + (cap-1)*100/400 : 0;
        unsigned previous = 0;
        for (int p = 0; p <= 100; ++p) {
            unsigned value = easyinput_brightness_channel(channel, p);
            assert(value >= previous && value <= old_max);
            previous = value;
        }
        assert(easyinput_brightness_channel(channel, 100) == old_max);
    }
    assert(easyinput_brightness_scale(256, -1) == 0);
    assert(easyinput_brightness_scale(256, 101) == 256);
    assert(easyinput_brightness_scale(256, 25) == 16);
    assert(easyinput_brightness_scale(256, 50) == 64);
    assert(easyinput_brightness_scale(256, 75) == 144);
    assert(easyinput_brightness_scale(63, 100) + 1 == 64);
    assert(easyinput_brightness_channel(50, 25) == 1);
    assert(easyinput_brightness_channel(50, 50) == 3);
    assert(easyinput_brightness_channel(50, 75) == 7);
    assert(easyinput_brightness_channel(50, 100) == 13);
    // 暗色不再被最低一级锁住；颜色为零时始终为零。
    assert(easyinput_brightness_channel(6, 25) == 0);
    assert(easyinput_brightness_channel(6, 100) == 2);
    assert(easyinput_brightness_channel(0, 100) == 0);
    assert(easyinput_oled_precharge(-1) == 0x11);
    assert(easyinput_oled_precharge(1) == 0x11);
    assert(easyinput_oled_precharge(25) == 0x21);
    assert(easyinput_oled_precharge(50) == 0x51);
    assert(easyinput_oled_precharge(75) == 0x91);
    assert(easyinput_oled_precharge(100) == 0xf1);
    assert(easyinput_oled_precharge(101) == 0xf1);
    unsigned previous_period = 0;
    for (int p = 0; p <= 100; ++p) {
        unsigned period = easyinput_oled_precharge(p);
        assert((period & 0x0f) == 1);
        assert((period >> 4) >= 1 && (period >> 4) <= 15);
        assert(period >= previous_period);
        previous_period = period;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp/easyinput', prefix='brightness-') as directory:
            src, exe = Path(directory) / 'test.c', Path(directory) / 'test.exe'
            src.write_text(source, encoding='utf-8')
            subprocess.run(compiler + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                                      str(src), '-o', str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

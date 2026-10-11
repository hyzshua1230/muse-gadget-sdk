# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""验证降采样滤波器的信号指标，不依赖 ESP-IDF 或硬件。"""
import cmath
from pathlib import Path
import re
import unittest


class EasyInputAudioFilterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (Path(__file__).resolve().parents[1] / "main" /
                  "voice_board_easyinput.c").read_text(encoding="utf-8")
        match = re.search(r"s_fir\[31\]\s*=\s*\{([^}]+)\}", source)
        cls.taps = [int(x) for x in re.findall(r"-?\d+", match[1])]

    def gain(self, frequency):
        return abs(sum(c / 32768 * cmath.exp(-2j * cmath.pi * frequency * i / 48000)
                       for i, c in enumerate(self.taps)))

    def test_dc_unity_and_linear_phase(self):
        self.assertEqual(len(self.taps), 31)
        self.assertEqual(sum(self.taps), 32768)
        self.assertEqual(self.taps, self.taps[::-1])

    def test_speech_passband(self):
        for frequency in (300, 1000, 3000, 5000):
            self.assertLess(abs(self.gain(frequency) - 1), 0.05)

    def test_high_frequency_alias_rejection(self):
        for frequency in (12000, 16000, 20000, 24000):
            self.assertLess(self.gain(frequency), 10 ** (-35 / 20))


if __name__ == "__main__":
    unittest.main()

"""核验实际嵌入的中文提示音，避免采样格式错误或WAV头作为PCM播放。"""
from pathlib import Path
import struct
import unittest
import wave

ASSETS = Path(__file__).resolve().parents[1] / "main" / "radio_assets"


class RadioAssets(unittest.TestCase):
    def test_embedded_pcm_matches_wave(self):
        for name in ("classical", "ambient"):
            with self.subTest(name=name), wave.open(str(ASSETS / f"{name}.wav"), "rb") as source:
                self.assertEqual((source.getframerate(), source.getsampwidth(), source.getnchannels()), (16000, 2, 1))
                samples = source.readframes(source.getnframes())
                self.assertEqual((ASSETS / f"{name}.pcm").read_bytes(), samples)
                self.assertGreater(len(samples), 16000)
                self.assertLess(len(samples), 16000 * 2 * 5)
                peak = max(abs(x[0]) for x in struct.iter_unpack("<h", samples))
                self.assertGreater(peak, 100)
                self.assertLess(peak, 32767)

    def test_station_prompts_are_different(self):
        self.assertNotEqual((ASSETS / "classical.pcm").read_bytes(), (ASSETS / "ambient.pcm").read_bytes())

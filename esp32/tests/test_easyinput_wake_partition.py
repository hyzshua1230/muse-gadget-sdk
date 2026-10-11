"""核验离线唤醒分区边界及真实模型产物，不访问设备或凭据。"""
import csv
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


def number(value):
    value = value.strip()
    if value[-1:] in ("K", "M"):
        return int(value[:-1], 0) * (1024 if value[-1] == "K" else 1024 * 1024)
    return int(value, 0)


def partitions(path):
    rows = csv.reader(line for line in path.read_text(encoding="utf-8").splitlines()
                      if line.strip() and not line.lstrip().startswith("#"))
    return {row[0].strip(): (row[1].strip(), row[2].strip(), number(row[3]), number(row[4]))
            for row in rows}


class WakePartitionTest(unittest.TestCase):
    def test_existing_regions_preserved(self):
        stable = partitions(ROOT / "partitions_muse.csv")
        wake = partitions(ROOT / "partitions_easyinput_v2_wake.csv")
        self.assertEqual(set(wake) - set(stable), {"model"})
        for name, region in stable.items():
            with self.subTest(name=name):
                self.assertEqual(wake[name], region)

    def test_alignment_overlap_and_flash_boundary(self):
        wake = partitions(ROOT / "partitions_easyinput_v2_wake.csv")
        previous_end = 0x11000
        for name, (kind, _, offset, size) in sorted(wake.items(), key=lambda item: item[1][2]):
            with self.subTest(name=name):
                self.assertGreaterEqual(offset, previous_end)
                self.assertEqual(offset % (0x10000 if kind == "app" else 0x1000), 0)
                self.assertEqual(size % 0x1000, 0)
                self.assertGreater(size, 0)
                self.assertLessEqual(offset + size, 16 * 1024 * 1024)
                previous_end = offset + size

    def test_built_model_contents_and_capacity(self):
        path = ROOT / "build-easyinput-v2-wake" / "srmodels" / "srmodels.bin"
        if not path.exists():
            self.skipTest("离线唤醒模型尚未构建")
        data = path.read_bytes()
        capacity = partitions(ROOT / "partitions_easyinput_v2_wake.csv")["model"][3]
        self.assertLessEqual(len(data), capacity)
        count, = struct.unpack_from("<I", data)
        self.assertEqual(count, 2)
        offset = 4
        names = set()
        spans = []
        for _ in range(count):
            names.add(data[offset:offset + 32].split(b"\0", 1)[0].decode("ascii"))
            files, = struct.unpack_from("<I", data, offset + 32)
            offset += 36
            for _ in range(files):
                start, size = struct.unpack_from("<II", data, offset + 32)
                spans.append((start, size))
                offset += 40
        self.assertEqual(names, {"wn9_nihaoxiaoyi_tts2", "vadnet1_medium"})
        end = offset
        for start, size in sorted(spans):
            self.assertEqual(start, end)
            self.assertGreater(size, 0)
            end = start + size
        self.assertEqual(end, len(data))


if __name__ == "__main__":
    unittest.main()

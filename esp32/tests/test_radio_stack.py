"""根据实际解码器机器码核验电台任务栈预算，不执行固件。"""
import pathlib
import re
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class RadioStackTest(unittest.TestCase):
    def test_compiled_decoder_fits_task_with_headroom(self):
        tool = shutil.which("xtensa-esp32s3-elf-objdump")
        obj = ROOT / "build-easyinput-v2-wake/esp-idf/minimp3/CMakeFiles/__idf_minimp3.dir/src/minimp3.c.obj"
        if not tool or not obj.exists():
            self.skipTest("需要现有ESP32-S3工具链和已编译解码器产物")
        disasm = subprocess.check_output([tool, "-d", str(obj)], text=True)
        name = ""
        stack = {}
        for line in disasm.splitlines():
            label = re.search(r"<([^>]+)>:$", line)
            if label:
                name = label.group(1)
            frame = re.search(r"\bentry\s+a1,\s*(0x[0-9a-f]+|[0-9]+)", line)
            if frame:
                stack[name] = int(frame.group(1), 0)
        self.assertIn("mp3dec_decode_frame", stack)
        source = (ROOT / "main/radio.c").read_text(encoding="utf-8")
        allocation = re.search(r"#define RADIO_STACK_BYTES \((\d+) \* 1024\)", source)
        self.assertIsNotNone(allocation)
        budget = int(allocation.group(1)) * 1024
        # 所有解码辅助函数栈帧相加（保守预算），另留8KiB给调用方及系统。
        required = sum(stack.values()) + 8192
        self.assertGreaterEqual(budget, required, f"任务栈{budget}字节，保守需求{required}字节")
        print(f"电台栈={budget}，解码入口={stack['mp3dec_decode_frame']}，保守需求={required}")


if __name__ == "__main__":
    unittest.main()

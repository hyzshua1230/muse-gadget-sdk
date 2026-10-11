"""编译实际缓存游标函数，比较续播与首次重采样的音频尾部。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def definition(source, marker):
    start = source.index(marker)
    begin = source.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ReplaySeek(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get("CXX", "c++"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("主机C++编译器不可用")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / "components/muse/muse_chat_session.cpp").read_text(encoding="utf-8")
        declarations = source[source.index("static SemaphoreHandle_t s_replay_lock;"):
                              source.index("static void replay_clear_locked(void)")]
        functions = "\n".join(definition(source, name) for name in (
            "static void resampler_init(", "static size_t resample(",
            "static void replay_clear_locked(", "static void replay_keep(",
            'extern "C" muse_replay_status_t muse_hatch_replay_status(',
            'extern "C" void muse_hatch_replay_failed(',
            'extern "C" void muse_hatch_replay_make_room(',
            'extern "C" void muse_hatch_replay_commit(',
            'extern "C" bool muse_hatch_replay_begin(',
            'extern "C" size_t muse_hatch_replay_read(',
        ))
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#define MAX_MSGS 8
#define MIC_RATE 16000
#define portMAX_DELAY 0
#define MALLOC_CAP_SPIRAM 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static size_t free_bytes = 8 * 1024 * 1024, largest_bytes = free_bytes;
static size_t heap_caps_get_free_size(int) { return free_bytes; }
static size_t heap_caps_get_largest_free_block(int) { return largest_bytes; }
struct { uint32_t load() { return 7; } } s_gen;
static void *heap_caps_realloc(void *p, size_t bytes, int) { return realloc(p, bytes); }
using SemaphoreHandle_t = void *;
static void xSemaphoreTake(void *, int) {}
static void xSemaphoreGive(void *) {}
'''
        harness += definition(source, "struct resampler_t {") + ";\n"
        header = (ROOT / "components/muse/muse_chat.h").read_text(encoding="utf-8")
        start = header.index("typedef enum {\n    MUSE_REPLAY_EMPTY")
        harness += header[start:header.index("} muse_replay_status_t;", start) + len("} muse_replay_status_t;")] + "\n"
        harness += declarations + functions
        harness += r'''
int main() {
    std::vector<std::vector<int16_t>> messages;
    for (size_t count : {1u, 2u, 3u, 255u, 256u, 257u, 513u, 2049u}) {
        messages.emplace_back(count);
        for (size_t i = 0; i < count; ++i)
            messages.back()[i] = int16_t((i * 7919 + messages.size() * 127) % 60001 - 30000);
    }
    s_replay_lock = reinterpret_cast<void *>(1);
    s_replay_generation = 7;
    std::vector<int16_t> expected;
    for (size_t i = 0; i < messages.size(); ++i) {
        resampler_t r;
        resampler_init(&r, 24000, 16000);
        std::vector<int16_t> whole(messages[i].size() + 2);
        size_t n = resample(&r, messages[i].data(), messages[i].size(), whole.data());
        expected.insert(expected.end(), whole.begin(), whole.begin() + n);
        auto *owned = static_cast<int16_t *>(malloc(messages[i].size() * sizeof(int16_t)));
        memcpy(owned, messages[i].data(), messages[i].size() * sizeof(int16_t));
        replay_keep(owned, messages[i].size(), 7);
        assert(s_replay_frames[i] == n);
        assert(!memcmp(s_replay_pcm[i], whole.data(), n * sizeof(int16_t)));
    }
    s_replay_valid = true;
    // 检查每一个暂停点，包括消息边界、奇偶相位和已播放完后的从头重播。
    for (size_t capacity : {1u, 3u, 17u, 320u}) {
        for (size_t position = 0; position <= expected.size() + 1; ++position) {
            size_t offset = position;
            assert(muse_hatch_replay_begin(&offset));
            assert(offset == (position >= expected.size() ? 0 : position));
            std::vector<int16_t> actual, chunk(capacity);
            while (size_t n = muse_hatch_replay_read(chunk.data(), chunk.size())) {
                assert(n <= chunk.size());
                actual.insert(actual.end(), chunk.begin(), chunk.begin() + n);
            }
            assert(actual == std::vector<int16_t>(expected.begin() + offset, expected.end()));
        }
    }
    size_t offset = 123;
    s_replay_valid = false;
    assert(!muse_hatch_replay_begin(&offset));
    int16_t chunk[320];
    assert(muse_hatch_replay_read(chunk, 320) == 0);
    replay_clear_locked();
    assert(muse_hatch_replay_status() == MUSE_REPLAY_EMPTY);
    muse_hatch_replay_failed();
    assert(muse_hatch_replay_status() == MUSE_REPLAY_FAILED);
    replay_clear_locked();
    s_replay_generation = 7;
    replay_keep(static_cast<int16_t *>(calloc(256, 2)), 256, 7);
    free_bytes = largest_bytes = 0;
    muse_hatch_replay_make_room(1024);
    assert(muse_hatch_replay_status() == MUSE_REPLAY_NO_MEMORY);
    muse_hatch_replay_commit();
    assert(muse_hatch_replay_status() == MUSE_REPLAY_NO_MEMORY);
    replay_clear_locked();
    s_replay_generation = 7;
    s_replay_bytes = REPLAY_LIMIT;
    replay_keep(static_cast<int16_t *>(calloc(256, 2)), 256, 7);
    assert(muse_hatch_replay_status() == MUSE_REPLAY_TOO_LARGE);
    // 超限后后续片段不能变成一份貌似完整的缓存。
    replay_keep(static_cast<int16_t *>(calloc(256, 2)), 256, 7);
    muse_hatch_replay_commit();
    assert(muse_hatch_replay_status() == MUSE_REPLAY_TOO_LARGE);
    assert(!muse_hatch_replay_begin(&offset));
    replay_clear_locked();
    s_replay_generation = 7;
    replay_keep(static_cast<int16_t *>(calloc(256, 2)), 256, 7);
    muse_hatch_replay_commit();
    assert(muse_hatch_replay_status() == MUSE_REPLAY_READY);
    replay_clear_locked();
    return 0;
}
'''
        (out / "seek.cpp").write_text(harness, encoding="utf-8")
        cls.binary = out / "seek.exe"
        result = subprocess.run([*cc, "-std=c++17", str(out / "seek.cpp"), "-o", str(cls.binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_every_pause_position_and_message_boundary(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

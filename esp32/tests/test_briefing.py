"""简报的无效输入、按键边界、准入与音频所有权；不访问云端。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def execute(test, source, cpp=False, args=()):
    compiler = shlex.split(os.environ.get('CXX' if cpp else 'CC', 'c++' if cpp else 'cc'))
    if not compiler or not shutil.which(compiler[0]):
        test.skipTest('主机编译器不可用，实际函数执行未验证')
    directory = Path(tempfile.mkdtemp(prefix='briefing-test-', dir=ROOT / 'tmp/easyinput'))
    src = directory / ('test.cpp' if cpp else 'test.c')
    exe = directory / 'test.exe'
    src.write_text(source, encoding='utf-8')
    subprocess.run(compiler + ['-std=c++11' if cpp else '-std=c11', '-Wall', '-Werror',
                              '-I', str(ROOT / 'components/muse'), str(src), '-o', str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe), *args], check=True, timeout=10, capture_output=True)


class BriefingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.voice = (ROOT / 'main/voice.c').read_text(encoding='utf-8')
        cls.session = (ROOT / 'components/muse/muse_chat_session.cpp').read_text(encoding='utf-8')

    def test_invalid_messages_are_not_spoken_and_links_stay_out_of_audio(self):
        execute(self, r'''
#include <assert.h>
#include <string.h>
#include "muse_briefing_text.h"
static const char *sources = "【参考来源】\nhttps://one.example/a\nhttps://two.example/b\nhttps://three.example/c";
static bool check(const char *date, const char *body, const char *refs) {
    char value[2048];
    strcpy(value,"【北京时间】"); strcat(value,date);
    strcat(value,"\n【简报正文】\n"); strcat(value,body); strcat(value,"\n"); strcat(value,refs);
    return muse_briefing_speech(value, NULL);
}
int main(void) {
    assert(!muse_briefing_speech(NULL, NULL));
    char progress[] = "正在搜索今天的新闻";
    assert(!muse_briefing_speech(progress, NULL));
    assert(!check("2026-02-30","北京时间2026-02-30，正文。",sources));
    assert(!check("2026-10-17","北京时间2026-10-16，旧日期。",sources));
    assert(!check("2026-10-17","北京时间2026-10-17，正文。","【参考来源】https://one.example/a"));
    assert(!check("2026-10-17","北京时间2026-10-17，正文。",
        "【参考来源】https://one.example/a\nhttps://one.example/a\nhttps://three.example/c"));
    assert(!check("2026-10-17","北京时间2026-10-17，[链接](https://one.example/a)",sources));
    assert(!check("2026-10-17","北京时间2026-10-17，http://one.example/a",sources));
    assert(!check("2026-10-17","",sources));
    assert(check("2028-02-29","北京时间2028-02-29，正文。",sources));
    assert(check(" \n2026-10-17","北京时间2026-10-17，正文。",sources));
    char final[2048] = "【北京时间】2026-10-17\n【简报正文】\n北京时间2026-10-17，完整正文。\n";
    strcat(final,sources);
    assert(muse_briefing_speech(final, NULL));
    assert(!strcmp(final,"北京时间2026-10-17，完整正文。"));
    assert(!strstr(final,"https://") && !strstr(final,"参考来源"));
    return 0;
}
''', cpp=True)

    def test_plain_sample_contains_the_three_reported_differences(self):
        # 固定用户提供的失败样本；只核验样本结构，不声称核验新闻事实。
        text = (ROOT / 'tests/fixtures/briefing-plain-20261017.txt').read_text(encoding='utf-8')
        self.assertTrue(text.startswith('2026-10-17\n北京时间2026年10月17日'))
        self.assertNotIn('【简报正文】', text)
        self.assertEqual(text.count('https://'), 2)
        self.assertEqual(text.count('http://'), 1)
        self.assertIn('.html—— IT之家', text)

    def test_actual_plain_reply_extraction_and_rejection_reasons(self):
        execute(self, r'''
#include <assert.h>
#include <fstream>
#include <iterator>
#include <string>
#include "muse_briefing_text.h"
static void reject(std::string value, const char *expected) {
    std::string before = value;
    const char *reason = NULL;
    assert(!muse_briefing_speech(&value[0], &reason));
    assert(reason && !strcmp(reason,expected));
    assert(value==before); // 拒绝时不能破坏原文。
}
int main(int argc, char **argv) {
    assert(argc==2);
    std::ifstream input(argv[1],std::ios::binary);
    assert(input.good());
    std::string original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    size_t begin=original.find('\n')+1, sources=original.find("\n1. ");
    assert(begin && sources!=std::string::npos);
    std::string speech=original.substr(begin,sources-begin), value=original;
    while (!speech.empty() && (speech.back()=='\r' || speech.back()==' ')) speech.pop_back();
    const char *reason=NULL;
    assert(muse_briefing_speech(&value[0], &reason) && !strcmp(reason,"ok"));
    assert(std::string(value.c_str())==speech);
    assert(!strstr(value.c_str(),"http") && !strstr(value.c_str(),"参考来源"));
    value=original; value.replace(0,10,"2026年10月17日");
    assert(muse_briefing_speech(&value[0],&reason));
    assert(std::string(value.c_str())==speech);
    value=original; value.replace(0,10,"2026-10-16"); reject(value,"date_mismatch");
    value=original.substr(0,original.find("\n3. ")); reject(value,"distinct_sources_less_than_three");
    value=original.substr(0,original.find("\n3. "))+"\n3. http://news.qq.com/rain/a/20261016A06NV100";
    reject(value,"distinct_sources_less_than_three"); // HTTP/HTTPS同地址不计两份来源。
    value=original.substr(0,sources)+"\n1. http://\n2. https://two.example/b\n3. https://three.example/c";
    reject(value,"source_url_invalid");
    value=original; value.insert(begin,"http://not-for-speech.example/a\n"); reject(value,"body_url");
    value=original; value.insert(begin,"**正文**\n"); reject(value,"body_markup");
    value=original.substr(0,begin)+original.substr(sources+1); reject(value,"body_empty");
    value=original.substr(0,sources); reject(value,"sources_missing");
    value="2026-10-17\n缺少正文日期。"+original.substr(sources); reject(value,"spoken_date_invalid");
    assert(muse_briefing_url_size("http://k.sina.com.cn/article.html—— IT之家")==strlen("http://k.sina.com.cn/article.html"));
    return 0;
}
''', cpp=True, args=(str(ROOT / 'tests/fixtures/briefing-plain-20261017.txt'),))

    def test_busy_muted_unpaired_offline_and_queue_failure(self):
        function = block(self.voice, 'void voice_briefing(void)')
        execute(self, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
typedef enum { EVT_BRIEFING } voice_evt_t;
typedef struct { voice_evt_t kind; unsigned cancel_epoch; } voice_event_t;
static atomic_bool s_ready, s_busy;
static atomic_uint s_cancel_epoch;
static void *s_events;
static bool muted, paired=true, network=true, connected=true, queue_ok=true;
static int queued, notices;
static voice_event_t event;
static bool easyinput_board_muted(void) { return muted; }
static bool config_setup_complete(void) { return paired; }
static bool network_feedback_ready(void) { return network; }
static bool muse_hatch_ready(void) { return connected; }
static void network_feedback_request(void) { ++notices; }
#define pdTRUE 1
static int xQueueSend(void *q,const voice_event_t *evt,int ticks) {
    (void)q; (void)ticks;
    if (!queue_ok) return 0;
    ++queued; event=*evt; return pdTRUE;
}
''' + function + r'''
int main(void) {
    voice_briefing(); assert(!queued && !s_busy);
    s_ready=true; muted=true; voice_briefing(); assert(!queued && !s_busy);
    muted=false; s_busy=true; voice_briefing(); assert(!queued && s_busy);
    s_busy=false; paired=false; voice_briefing(); assert(!queued && !s_busy && notices==1);
    paired=true; network=false; voice_briefing(); assert(!queued && !s_busy && notices==2);
    network=true; connected=false; voice_briefing(); assert(!queued && !s_busy && notices==3);
    connected=true; s_cancel_epoch=42; voice_briefing();
    assert(queued==1 && s_busy && event.kind==EVT_BRIEFING && event.cancel_epoch==42);
    voice_briefing(); assert(queued==1);
    s_busy=false; queue_ok=false; voice_briefing(); assert(!s_busy && queued==1);
    return 0;
}
''')

    def test_s5_and_radio_press_boundaries_use_production_task(self):
        button = (ROOT / 'main/button.c').read_text(encoding='utf-8')
        task = block(button, 'static void button_task(void *arg)')
        execute(self, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#define CONFIG_HOMEHUB_VOICE 1
#define CONFIG_HOMEHUB_QWEN_TTS 1
#define CONFIG_HOMEHUB_RADIO 1
#define CONFIG_MUSE_ENABLED 0
#define SHORT_PRESS_MAX_MS 1000
#define LONG_PRESS_MS 5000
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static const int s_keys[9]={2,47,38,41,1,6,7,48,18};
typedef bool (*button_press_cb)(bool);
static button_press_cb s_press_cb;
static void (*s_short_press_cb)(void), (*s_long_press_cb)(void);
static int64_t now, release_at;
static int key, briefings, notices, toggles, nexts, selects, resets;
static jmp_buf done;
static int gpio_get_level(int pin) { return !(pin==key && now>=100000 && now<release_at); }
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ms) { now+=ms*1000; if (now>release_at+100000) longjmp(done,1); }
static void voice_toggle_mute(void) {}
static void voice_replay(void) {}
static void voice_cancel(void) {}
static void voice_toggle_brightness_mode(void) {}
static void voice_abort(void) { ++resets; }
static void voice_briefing(void) { ++briefings; }
static void network_feedback_request(void) { ++notices; }
static void radio_toggle(void) { ++toggles; }
static void radio_next(void) { ++nexts; }
static void radio_select_mode(void) { ++selects; }
''' + task + r'''
static void run(int pin, int64_t duration) {
    now=0; key=pin; release_at=100000+duration;
    if (!setjmp(done)) button_task(0);
}
int main(void) {
    run(1,980000); assert(briefings==1 && !notices);
    run(1,1000000); assert(briefings==1 && notices==1);
    run(1,5200000); assert(briefings==1 && notices==2 && !resets);
    run(6,980000); assert(toggles==1);
    run(6,1000000); assert(toggles==1);
    run(7,980000); assert(nexts==1 && !selects);
    run(7,1000000); assert(nexts==1 && selects==1);
    return 0;
}
''')

    def test_no_asr_or_network_in_button_and_replay_paths(self):
        briefing = block(self.voice, 'static bool run_briefing(void)')
        self.assertIn('muse_briefing_turn(BRIEFING_PROMPT)', briefing)
        self.assertIn('return reply();', briefing)
        self.assertNotIn('record(', briefing)
        replay = block(self.voice, 'static bool replay(void)')
        self.assertNotIn('muse_briefing_turn', replay)
        self.assertNotIn('muse_qwen_tts_start', replay)
        admission = block(self.voice, 'void voice_briefing(void)')
        self.assertNotIn('muse_briefing_turn', admission)
        self.assertNotIn('voice_player_', admission)

    def test_cancelled_prompt_is_dropped_and_references_are_gated(self):
        handling = self.session[self.session.index('case CMD_PROMPT:'):self.session.index('case CMD_END:')]
        self.assertGreaterEqual(handling.count('cmd.gen == s_gen.load()'), 2)
        self.assertIn('free(cmd.text)', handling)
        final = block(self.session, 'static void message_done(')
        self.assertLess(final.index('muse_briefing_speech('), final.index('m.tts = TTS_QUEUED'))
        self.assertIn('BRIEFING DATE/SOURCES UNAVAILABLE', self.session)

    def test_paused_briefing_retains_radio_and_explicitly_releases(self):
        hold = block(self.voice, 'if (!radio_briefing_hold)')
        self.assertIn('voice_player_wait(1000)', hold)
        self.assertIn('radio_pause(false)', hold)
        self.assertIn('radio_briefing_hold = briefing_audio && atomic_load(&s_paused) && !easyinput_board_muted()', self.voice)
        self.assertIn('muse_hatch_replay_status() != MUSE_REPLAY_READY', self.voice)
        release = block(self.voice, 'if (radio_briefing_hold &&')
        self.assertIn('radio_pause(false)', release)
        self.assertIn('radio_briefing_hold = false', release)
        self.assertIn('briefing_audio = false; // S1', self.voice)


if __name__ == '__main__':
    unittest.main()

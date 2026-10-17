"""一键故事的配对优先级、无ASR入口和按键事件所有权。"""
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


class StoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.voice = (ROOT / 'main/voice.c').read_text(encoding='utf-8')
        cls.session = (ROOT / 'components/muse/muse_chat_session.cpp').read_text(encoding='utf-8')

    def test_pairing_precedes_story(self):
        app = (ROOT / 'main/app.c').read_text(encoding='utf-8')
        callback = block(app, 'static void on_button_short_press(void)')
        confirm = block(callback, 'if (link_pairing_confirmation_required())')
        unpaired = block(callback, 'if (!config_setup_complete())')
        self.assertIn('return;', confirm)
        self.assertIn('open_setup_window', unpaired)
        self.assertIn('return;', unpaired)
        self.assertLess(callback.index('if (!config_setup_complete())'), callback.index('voice_story();'))

    def test_spoken_prompt_skips_recording_and_console_mode(self):
        story = block(self.voice, 'static bool run_story(void)')
        self.assertIn('muse_hatch_prompt_turn(STORY_PROMPT)', story)
        self.assertIn('return reply();', story)
        self.assertNotIn('record()', story)
        self.assertNotIn('muse_hatch_turn_begin()', story)
        command = self.session[self.session.index('case CMD_PROMPT:'):self.session.index('case CMD_END:')]
        self.assertIn('cmd.gen == s_gen.load()', command)
        self.assertIn('turn_start(cmd.gen, false)', command)
        self.assertIn('send_chat(cmd.text,"text")', command)
        self.assertIn('free(cmd.text)', command)
        self.assertNotIn('turn_begin(', command)
        api = block(self.session, 'extern "C" bool muse_hatch_prompt_turn(')
        self.assertIn('strdup(text)', api)
        self.assertIn('free(copy)', api)

    def test_regular_voice_restores_its_lights(self):
        regular = block(self.voice, 'static bool run_turn(void)')
        self.assertIn('led_status_set_story(false)', regular)
        story = block(self.voice, 'static bool run_story(void)')
        self.assertIn('led_status_set_story(true)', story)
        prompt = (ROOT / 'main/story_prompt.h').read_text(encoding='utf-8')
        self.assertIn('6至9岁', prompt)
        self.assertIn('不固定小萤火虫', prompt)

    def test_actual_button_event_admission(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('主机C编译器不可用，实际事件准入测试未执行')
        function = block(self.voice, 'void voice_story(void)')
        source = r'''
#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>
#define ESP_LOGI(...) ((void)0)
typedef enum { EVT_PRESS, EVT_RELEASE, EVT_REPLAY, EVT_STORY } voice_evt_t;
typedef struct { voice_evt_t kind; unsigned cancel_epoch; } voice_event_t;
static atomic_bool s_ready, s_busy;
static atomic_uint s_cancel_epoch;
static void *s_events;
static bool muted, connected, queue_ok=true;
static int requests, queued;
static voice_event_t event;
static bool easyinput_board_muted(void) { return muted; }
static bool muse_hatch_ready(void) { return connected; }
static void network_feedback_request(void) { ++requests; }
#define pdTRUE 1
static int xQueueSend(void *q,const voice_event_t *e,int wait) {
    (void)q; (void)wait;
    if (!queue_ok) return 0;
    event=*e; ++queued; return 1;
}
'''
        source += function + r'''
int main(void) {
    voice_story(); assert(!queued && !s_busy);
    s_ready=true; muted=true; voice_story(); assert(!queued && !s_busy);
    muted=false; s_busy=true; voice_story(); assert(!queued && s_busy);
    s_busy=false; connected=false; voice_story(); assert(!queued && !s_busy && requests==1);
    connected=true; s_cancel_epoch=77; voice_story();
    assert(queued==1 && s_busy && event.kind==EVT_STORY && event.cancel_epoch==77);
    voice_story(); assert(queued==1);
    s_busy=false; queue_ok=false; voice_story(); assert(queued==1 && !s_busy);
    return 0;
}
'''
        directory = Path(tempfile.mkdtemp(prefix='story-admission-', dir=ROOT / 'tmp/easyinput'))
        src, exe = directory / 'story.c', directory / 'story.exe'
        src.write_text(source, encoding='utf-8')
        subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', str(src), '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)


if __name__ == '__main__':
    unittest.main()

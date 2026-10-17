"""核验离线资源、音频仲裁约束及实际提示策略（有主机C编译器时）。"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]


class NetworkFeedbackTest(unittest.TestCase):
    def test_local_prompts_match_player_format(self):
        for name in ('wifi', 'service', 'pair', 'ready'):
            with self.subTest(name=name):
                path = ROOT / 'main/network_assets' / name
                with wave.open(str(path.with_suffix('.wav')), 'rb') as f:
                    self.assertEqual((f.getframerate(), f.getnchannels(), f.getsampwidth()), (16000, 1, 2))
                    pcm = f.readframes(f.getnframes())
                    self.assertEqual(pcm, path.with_suffix('.pcm').read_bytes())
                    self.assertLess(len(pcm), 8 * 32000)
                    self.assertGreater(len(pcm), 16000)

    def test_notices_use_existing_player_and_guard_audio(self):
        voice = (ROOT / 'main/voice.c').read_text(encoding='utf-8')
        notice = voice[voice.index('static bool play_network_notice('):voice.index('static void voice_task(')]
        for guard in ('easyinput_board_muted()', 'atomic_load(&s_busy)', 'radio_pause(true)',
                      'voice_wake_stop()', 'uxQueueMessagesWaiting(s_events)', 'network_feedback_notice_valid(notice)'):
            self.assertIn(guard, notice)
        self.assertNotIn('voice_board_set_volume', notice)
        self.assertNotIn('muse_hatch_turn', notice)
        self.assertLess(notice.index('radio_pause(true)'), notice.index('voice_player_begin()'))

    def test_actual_policy_with_simulated_clock(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('主机C编译器不可用，实际策略执行未验证')
        source = r'''
#include <assert.h>
#include "network_feedback_policy.h"
int main(void) {
    network_policy_t p={0};
    assert(network_policy_step(&p,NET_READY,0,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,1000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_READY,5000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,10000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,24999,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,25000,false,false)==NET_NOTICE_DELAY);
    assert(network_policy_step(&p,NET_SERVICE,30000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_SERVICE,70000,false,false)==NET_NOTICE_SERVICE);
    assert(network_policy_step(&p,NET_SERVICE,80000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_READY,81000,false,false)==NET_NOTICE_RECOVERED);
    assert(network_policy_step(&p,NET_READY,82000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_READY,83000,false,true)==NET_NOTICE_READY);
    assert(network_policy_step(&p,NET_WIFI,90000,true,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,150000,true,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,151000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_WIFI,152000,false,true)==NET_NOTICE_WIFI);
    assert(network_policy_step(&p,NET_READY,153000,false,false)==NET_NOTICE_NONE);
    assert(network_policy_step(&p,NET_SETUP,154000,false,true)==NET_NOTICE_PAIR);
    assert(!p.tracking);
    assert(network_policy_step(&p,NET_READY,155000,true,true)==NET_NOTICE_NONE);
    return 0;
}
'''
        directory = Path(tempfile.mkdtemp(prefix='network-policy-', dir=ROOT / 'tmp/easyinput'))
        src, exe = directory / 'policy.c', directory / 'policy.exe'
        src.write_text(source, encoding='utf-8')
        subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'main'),
                             str(src), '-o', str(exe)], check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)


if __name__ == '__main__':
    unittest.main()

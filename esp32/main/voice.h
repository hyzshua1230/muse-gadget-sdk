/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Push-to-talk voice chat with the agent. Hold the button and speak; on
// release the recording goes to the agent's chat as a voice note, which the
// VM transcribes, over the voice session borrowed from Muse (muse_chat.h).
// The reply is text and shows up in the Muse app; a TTS API of your own can
// speak it (start_tts in muse_chat_session.cpp).

#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include "cJSON.h"

// Start the voice task. It brings up the audio hardware and then takes over
// the button whenever a turn can run.
void voice_init(void);
#if CONFIG_HOMEHUB_EASYINPUT_OLED
typedef enum { VOICE_DIAL_VOLUME, VOICE_DIAL_BRIGHTNESS, VOICE_DIAL_RADIO } voice_dial_mode_t;
voice_dial_mode_t voice_dial_mode(void);
int voice_radio_station(void);
void voice_radio_telemetry(int *bitrate_kbps, int *remain_min, int *vu_level);
int voice_volume(void);
bool voice_playback_paused(void);
bool voice_halted(void);
#endif

/* EasyInput：按键任务仅提交请求，音频任务执行取消；不会转为再次录音。 */
void voice_cancel(void);
/* 恢复设置等操作始终放弃当前任务，不解释为暂停。 */
void voice_abort(void);
void voice_toggle_mute(void);
void voice_replay(void);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 && CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_VOICE && !CONFIG_MUSE_ENABLED
// S8短按提交故事事件，按键任务不执行网络或音频操作。
void voice_story(void);
// S5短按：文字简报，无录音/ASR；与故事共用语音任务。
void voice_briefing(void);
#endif
// EasyInput旋钮短按：请求切换音量/亮度模式。
void voice_toggle_brightness_mode(void);

// voice.configure: sets the speaker volume (0-100), kept in NVS. The dial on
// top sets it too.
cJSON *voice_configure_command(cJSON *params);

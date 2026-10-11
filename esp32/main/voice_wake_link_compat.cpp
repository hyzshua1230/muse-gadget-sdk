// SPDX-License-Identifier: Apache-2.0
#include "dl_fbank.hpp"

// 初始化入口引用此空函数，让链接器取出包含虚函数表的静态库对象。
extern "C" void voice_wake_link_compat(void) {}

namespace {
// ESP-SR预编译库引用基类虚函数表，但当前ESP-DL对象未生成它。
// 让同一编译器从官方头文件生成该表；此函数不调用，链接时会被移除。
__attribute__((used)) dl::audio::Fbank *emit_speech_feature_vtable(dl::audio::SpeechFeatureConfig config)
{
    return new dl::audio::Fbank(config);
}
}

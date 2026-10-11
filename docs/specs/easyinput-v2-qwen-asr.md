# EasyInput V2：Qwen-ASR 接入

## 范围

按住 S1 录音，松开后使用北京地域 `qwen3-asr-flash` 转写，指定 `language=zh`。
识别文字提交到 Muse `/chat/stream`，回复继续由已有 Qwen-TTS 朗读。
麦克风驱动、采样增益、按键和分区布局不变。本次不承诺识别准确率已经提高。

## 实现

- 本地缓存最多 20 秒的 16 kHz、单声道 PCM16；现有语音任务仍限制录音最多 15 秒。
- 生成实际长度的 WAV，再以 Base64 Data URL 提交 HTTPS 请求。
- 接口：`https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`。
- 复用本机配置中的北京地域 Qwen Key，不在文档或日志中记录 Key。
- 新增 `CONFIG_HOMEHUB_QWEN_ASR`；EasyInput 配置启用。
- HTTPS 保留证书验证，禁止自动重定向；响应最多 16 KiB。
- 后台任务执行识别，连接读写超时 15 秒，会话等待最多 60 秒。
- 取消后旧结果作废，不自动退回 Muse 语音识别；失败显示 `QWEN ASR FAILED`。
- 空文本或非正常结束的响应不提交 Muse，HTTP 日志只记录状态码。
- 录音、请求和响应使用 PSRAM，后台任务栈使用内部内存。

## 验收

1. 编译启用 ASR 与已有 TTS 的 ESP32-S3 固件。
2. 实机录音后，日志出现 Qwen ASR HTTP 200 和 heard 文本；Muse 返回并朗读。
3. S4 取消、S2 静音及新录音打断时，旧识别结果不进入下一轮。
4. 空录音、无权限、断网、超时有明确错误，无崩溃。
5. 同一短句重复测试，对比替换前后的错误；原始音频质量仍需单独验证。

ASR 模型权限、额度和费用尚未核验；本次不进行云端试音。
烧录及实机验收尚未完成。

## 执行记录（2026-10-10）

- 已完成源代码接入，并让取消后的新请求等待旧后台任务退出。
- 使用现有 D 盘 ESP-IDF 6.0.1 编译；最终构建退出码 0，无编译警告或错误。
- 编译命令：在 `esp32` 中激活已有 `tmp/easyinput/env.ps1`，加载已有 Qwen 本地配置，
  执行 `idf.py -B build-easyinput-v2 -DIDF_TARGET=esp32s3 build`。
- 已核验 ASR/TTS 均启用、目标 ESP32-S3、Key 已配置；未输出密钥。
- 编译日志：`esp32/tmp/easyinput/qwen-asr-build-final.log`；`git diff --check` 通过。
- 修改：新增 `muse_qwen_asr.cpp/.h`，更新会话处理、组件 CMake、Kconfig、EasyInput 配置及本文档。
- 未安装新依赖或修改系统配置；访问的文档域名仅 `help.aliyun.com`。
- 未调用 ASR 云服务、未烧录；未验证费用额度、实机准确率、取消行为或内存峰值。
- 本机完整会话单元测试仍受此前缺少主机 C/C++ 编译器的限制，未执行。
- 本轮构建产物和日志保留于现有构建及临时目录，不进行删除清理。

## 烧录与串口验证（2026-10-10）

- 用户授权“刷机验证”；烧录前后核验 COM6、ESP32-S3 rev v0.2、MAC `c4:9e:7e:07:9f:d8` 一致。
- 使用 `esptool --chip esp32s3 --port COM6 --baud 460800 --before default-reset --after hard-reset write-flash @flash_args`。
- 写入启动程序、同布局分区表、OTA 初始状态、射频初始化及应用；退出码 0，数据 Hash 校验通过；未擦除 NVS。
- 应用大小 1,445,888 字节，启动日志 ELF SHA256 前缀 `b945638ca` 与本机构建一致，ESP-IDF v6.0.1。
- 已恢复原有 Wi-Fi、配对配置；初始 Muse 服务 TLS 握手失败后重试恢复，最终 `setup=done wifi=up ws=up raw=up`。
- 两次录音分别 3.58 秒、2.84 秒，Qwen-ASR 均返回 HTTP 200，现有北京 Key 的本次 ASR 调用成功。
- 转写分别为“介绍一下你自己。”、“你能帮我做什么呢？”；未取得用户实际口述文本，不能据此计算准确率。
- 第一轮 Muse 回复成功，取得 Qwen-TTS 24 kHz 单声道 PCM16；播放任务记录 `reply: 14.1s of speech, 30.8s total`。
- 第二轮同样正常结束，播放任务记录 `reply: 35.0s of speech, 62.1s total`。
- 采集窗口无崩溃或看门狗日志；声音是否正常、识别是否准确等待用户观察确认。
- 本次实际调用会消耗 ASR/TTS 额度；未查询余额或账单。
- 日志：`esp32/tmp/easyinput/qwen-asr-flash.log`、`esp32/tmp/easyinput/qwen-asr-runtime.log`，敏感串口日志已过滤。
- 本机串口采集器、核验记录保存在 `esp32/tmp/easyinput/`；未改系统配置或安装依赖。
- 尚未验证静音、取消、打断、错误路径及准确率对照；历史“未烧录”记录由本节更新。

### 无声反馈与验证边界

用户随后报告“最后一次没有播放声音”。对应哪一轮尚待确认，不能将播放任务结束标为实际出声验收通过。
`reply: ...s of speech` 按送入播放队列的采样数统计；`voice.c` 未检查
`voice_player_write` 返回值，`voice_player.c` 未处理 `voice_board_speaker_write` 返回值，
并且缺少音频幅值与播放时实际增益统计，因此该日志不能证明非静音音频成功送到扬声器。
第二轮 TTS 开始附近存在约 21 秒采集空档，不能用缺失日志排除静音、音量或音频输出问题。
补抓日志仅有健康心跳，没有新的语音交互；当前无法确定无声根因。

用户补充无声请求为“帮我总结一下今日的AA要闻”，并非此前两条短句。
一次复测捕获 ASR HTTP 200，转写为“帮我总结今日的，a，要闻。”，Muse 回复结束事件为 728 字节
（日志写作 chars，但源码使用 `strlen`，因此不是字符数）。该次回复未超过 1023 字节缓存，
不能归因于文本截断；TTS 期间发生采集切换空档，仍缺少无声根因证据。
随后连续采集 180 秒，仅捕获健康心跳，未捕获新的 S1 交互。
现场日志：`esp32/tmp/easyinput/qwen-asr-news-no-sound.log`、`qwen-asr-news-continuous.log`。

### 根因确认：Qwen-TTS 合成超时

2026-10-10 用户报告同一句无声、5 灯熄灭。补充合成/下载阶段、PCM 幅值、扬声器实际增益及
I2S 写入诊断；修正播放代码忽略队列及 I2S 写入失败的问题。现有 ESP-IDF 6.0.1 编译退出码 0，
无警告或错误；沿用本次刷机验证授权烧入同一 COM6/MAC 设备，写入退出码 0、Hash 校验通过。
新应用 ELF SHA256 前缀 `f91e1656a`，NVS 保留。

连续日志 `esp32/tmp/easyinput/qwen-audio-diagnostic-runtime.log` 复现：

- 70.986 秒：Qwen-ASR HTTP 200，转写“帮我总结一下今日的A I要闻。”。
- 78.773 秒：Muse 回复完成，731 字节；没有超过当前文本缓存。
- 78.779 秒：Qwen-TTS synthesis begin。
- 89.494 秒：`Connection timed out before data was ready!`，HTTP status -1。
- 89.498 秒：`synthesis failed (0 bytes)`。
- 89.500 秒：`turn failed: QWEN TTS FAILED`。
- Wi-Fi、Muse WebSocket 仍在线，无重启证据。

本次失败发生于 Qwen-TTS 合成等待阶段，尚未取得音频，不是 ASR 或扬声器写入失败。
代码为合成和音频下载统一设置 10 秒超时；请求约 10.7 秒后失败。
5 灯在错误提示结束后恢复空闲而熄灭，不能据此判断断电或掉线。
本轮错误退出时 speaker end 的样本数是启动提示音留下的统计，不能作为本轮回复输出证据。

建议下一步：将合成请求等待上限增加到 30 秒，下载仍维持 10 秒，保留明确失败提示；
同一句连续复测，验证 HTTP 200、非零 PCM 幅值、I2S 写入及实际出声。
合成超时调整尚未实施，当前只完成诊断与播放错误检查修正。
构建日志 `qwen-audio-diagnostic-build.log`、烧录日志 `qwen-audio-diagnostic-flash.log` 均位于
`esp32/tmp/easyinput/`；未安装依赖或改系统配置。实际测试调用了北京 ASR/TTS 服务。

同一采集窗口随后捕获“今天星期几？”：TTS 合成和下载均 HTTP 200，PCM peak=17023，
音量 55、muted=0、amp=1；I2S 写入 42240 个 48 kHz 帧，输出 peak=9362，播放任务正常结束。
这证明该短句的软件音频输出成功，支持将要闻请求的无声定位到合成超时，物理出声仍以用户观察为准。

### 修复与验证（2026-10-10）

用户授权“帮我修复、验证”后，将 `muse_qwen_tts.cpp` 的合成请求超时由 10 秒改为 30 秒，
音频下载继续维持 10 秒，不增加自动重试或备用服务。

- 现有 ESP-IDF 6.0.1 编译退出码 0，无警告或错误；`git diff --check` 通过。
- 固件 1,445,888 字节，4 MiB 应用槽剩余 66%。
- 烧录前后核验 COM6、ESP32-S3、MAC `c4:9e:7e:07:9f:d8` 一致；烧录退出码 0、Hash 校验通过。
- 运行应用恢复原有 Wi-Fi/Muse 连接。ELF SHA256 为
  `216315c84e6188fd6f5c3f19352d90d1a7b5be23eb3d16ea7534ae4432c63615`。
- 首段串口采集在约 78 秒后因 USB 断开报错；重新扫描 USB 序列号仍匹配原设备，再次开启只读采集。
- 本轮没有安装依赖、访问密钥内容或修改系统配置，没有擦除 NVS。
- 日志：`esp32/tmp/easyinput/qwen-tts-timeout-build.log`、`qwen-tts-timeout-flash.log`、`qwen-tts-timeout-runtime.log`。
- 实际要闻请求的合成耗时、输出和用户听音确认仍待复测，不能仅凭编译/烧录通过宣告无声问题解决。

本轮后续捕获一次 3.80 秒录音，ASR 转写“今日哀要闻。”，与预期要闻请求不完全一致。
Muse 返回 524 字节回复，合成耗时约 9.52 秒，HTTP 200；下载 HTTP 200，
取得 950400 个 24 kHz 帧（39.6 秒），PCM peak=16319。
扬声器音量 55、muted=0、amp=1；I2S 成功写入 1900800 个 48 kHz 帧，output_peak=8905。
播放任务正常结束，未报 QWEN TTS FAILED 或输出错误，联网保持正常。
这次验证通过了软件合成、下载与音频输出，实际是否听见声音待用户确认。
此次合成未超过旧 10 秒上限，因此尚未实测“超过10秒、30秒内成功”的边界，
也未完成原请求语义准确的复测；不能将单轮成功视为所有超时已消除。

用户随后明确反馈“听到了”，本轮扬声器实际朗读人工验收通过。
本次无声问题已完成超时修复、编译、烧录及一轮实际出声验证；ASR 对 AI 要闻的转写准确性仍待解决，
超过旧10秒上限的合成请求成功边界也尚未单独验证。

## 参考资料

[阿里云 Qwen-ASR API](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)

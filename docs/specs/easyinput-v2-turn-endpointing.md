# Spec：EasyInput V2 语音轮次结束判断

日期：2026-10-11。用户已授权写入规格并开始实现。用户随后授权按自动60秒、S1按住120秒与流式ASR方向修改。实现、编译及现场验证分别记录，未经用户确认不烧录。

## 1. 场景与问题

定位为中文桌面语音助手：用户可能在问题中换气、思考、补充条件。目标是在正常停顿时继续接收整条问题，同时给出清楚的监听反馈。当前自动录音连续800ms被VAD判为非语音即提交，可能将中途停顿当作结束；15秒录音上限也可能截断长问题。VAD判断人声活动，不能判断语义是否完整。

S1按住说话、松开提交继续保留，供用户显式决定结束。S1样本上限改为120秒，保留250ms松开尾音，不加入语音结束口令或新按键映射。自动录音总时间/样本上限为60秒，包含唤醒后等问题及句尾等待。

## 2. 首阶段：候选结束与延迟提交

| 状态/条件 | 行为 |
| --- | --- |
| 唤醒后尚无问题 | 沿用5秒无问题取消，不提交整轮识别或调用Muse；已流式发送的音频可能计费 |
| 已开始说话 | 持续采音，蓝色音量反馈 |
| 连续非语音达到800ms | 进入结束候选，蓝色柔和呼吸；仍采音并向ASR流式发送，不提交识别或调用Muse |
| 候选期重新检测到语音 | 清零静音累计，回到音量反馈；保留停顿前后全部内容 |
| 连续非语音达到2200ms | 排空音频队列后提交一次整轮识别，等待最终文字 |
| S4取消/S2静音/S1接管 | 沿用既有取消与接管规则，旧自动录音不提交 |
| 自动录音达到60秒时间/样本上限，或S1达到120秒样本上限 | 拒绝提交半条录音，1灯红色录音错误提示并记录RECORDING TIME LIMIT；随后恢复可用状态 |

2200ms包含800ms候选阶段，均依据16kHz音频样本累计，不是800+2200ms；VAD内部判定与处理调度会影响实际从人耳所感句尾到提交的时延。参数是首轮试验值，不声明为普适最佳值。该方案仍属声学结束判断，超过门限的思考停顿仍可能提前提交。

新灯效遵守统一亮度设置，静音/错误优先，旋钮音量和亮度提示不得覆盖候选录音反馈。仅自动录音触发该候选状态，不改变唤醒词、采音增益或VAD模型；云端ASR变更见第3节。

## 3. 流式ASR与录音容量

旧实现前端15秒、Muse缓存640000字节、Qwen请求入口640000字节，这两处约20秒限制不能通过单独改前端解决。60/120秒PCM分别约1.92/3.84MB，旧PCM+WAV+Base64同时存在时的构造峰值约6.4/12.8MB，不适合8MiB PSRAM及本地唤醒/TLS并存。

新链路使用既有esp_websocket_client组件、qwen3-asr-flash-realtime模型、北京域名 `wss://dashscope.aliyuncs.com/api-ws/v1/realtime`。原域名仍受官方支持，不增加Workspace ID配置。复用本机已有Qwen key，不读取或打印真实值；实时模型可用性与免费额度独立于旧非流式模型，烧录后测试时核验。

- 用户触发一轮录音才打开会话；空闲离线监听不连接ASR。
- 等待session.created，发送session.update：16kHz/PCM、中文、turn_detection=null；收到session.updated才发音频，拒绝明确返回非手动模式的配置。
- 保留既有256000字节（8秒）积压缓冲，发送每块最多3200字节PCM，Base64发送缓冲4608字节、接收JSON缓冲16384字节均在PSRAM，取消640000字节全录音缓存及完整WAV/请求体。WebSocket使用8192字节内部栈，取代旧ASR任务同规模内部栈；这只是设计预算，TLS并存峰值需测量。
- 应用结束前不发commit，不使用草稿/分段结果触发Muse。收到结束命令并排空音频队列，发送一次input_audio_buffer.commit和session.finish；等待最终transcript与session.finished后关闭ASR连接，再提交一次完整Muse请求。
- 接收同时处理消息分片及组件缓冲区分块，超过16383字节的事件明确失败，不截断文字；取消以代次隔离并关闭连接，旧结果不得交给新录音。
- 连接等待15秒、网络操作10秒、单次发送等待100ms、结束后识别等待60秒，沿用8秒积压约束。网络过慢、缓冲满或断网明确失败，不自动重连、不重试、不保留半条录音降级；录音任务及时读取上游错误以停止采音。

120秒上限不是每次等待120秒：S1松开即可结束，自动录音仍由2.2秒非语音确认结束。5分钟长篇口述作为未来独立模式，不在本次实现。S1在上限仍被按住时也不提交；需要用户缩短后重新录入，首版没有分段拼接或保留被拒绝长录音的功能。

## 4. 后续：语义轮次判断

旧Qwen接入是录完上传的qwen3-asr-flash；本轮改为实时手动提交，但尚未接入语义轮次模型。后续可评估流式ASR加经过中文验证的语义轮次检测；云端识别片段完成不等于用户整轮完成，服务端停顿切片也不能直接触发Muse执行。

必须验证中文犹豫词、未完成句、补充条件、数字/专名、弱声句尾；不能仅凭句号或固定词尾决定结束。统一由应用确认整轮提交，检测到用户继续说话时取消候选判断，避免重复提交。服务、模型、成本、网络中断行为及内存预算另行评审，本次不接入语义模型。

参考：[Google人声事件与超时](https://docs.cloud.google.com/speech-to-text/docs/voice-activity-events)、[OpenAI语义与声学轮次检测](https://platform.openai.com/docs/api-reference/realtime-client-events/session)、[Qwen实时ASR的server_vad与手动提交](https://www.alibabacloud.com/help/en/model-studio/qwen-asr-realtime-client-events)、[识别片段完成与停顿结束的区别](https://developers.deepgram.com/docs/understand-endpointing-interim-results)。这些公开能力用于设计参考，不推断所有消费产品采用同一算法。

## 5. 验收与日志

- 安静约0.5–1米，同一句问题在中间分别停顿约0.5、1、1.5秒再继续，各5次：目标均不提前提交，最终转写包含前后内容且仅发起一轮Muse请求。
- 说完后保持安静：候选阶段继续监听，累计VAD静音约2.2秒后提交；记录句尾到提交的实际时延，不把ASR或回复网络时延混入结束等待。
- 背景人声、弱声句尾各5次，记录提前结束及迟迟不结束的次数；未达标先定位VAD判定或结束门限，不同时调整多项参数。
- 自动录音持续超过60秒、S1持续超过120秒：明确时间限制日志、录音错误提示、无ASR/Muse提交，随后能再次唤醒。唤醒后无问题5秒取消、S4取消、S2静音、S1接管及手动松开各验证，旧录音不得重复提交。
- 日志区分silence_end、no_question、time_limit、sample_limit、button_release、cancelled、manual_takeover、microphone_error、backlog_full、upstream_error；记录候选进入与说话恢复。若采音停止失败，沿用既有MICROPHONE STOP FAILED错误分类。日志不输出凭据。

## 6. 执行记录

- 修改voice.c录音边界和结束原因日志；led_status.c/.h新增候选蓝色呼吸并保持优先级。未增加模型、依赖、配置项、网络请求或新任务，现有亮度调节实现保留。
- 复用已审查及已授权的本地构建流程，激活既有SDK6.0.1，沿用本地配置overlay及组件缓存，实际命令 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake -D SDKCONFIG=tmp/easyinput/sdkconfig.wake-memory-4k build`，日志 `esp32/tmp/easyinput/turn-endpointing-build.log`，退出码0。应用SHA256 `aef915ce82224f9c719df6507d73348c7ed42c03f9d544193f780458a6fa980b`。
- 执行 `python -m unittest tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter`，6项现有回归通过；`git diff --check`通过。这些测试不验证端点时序或实际灯效，第5节现场验收尚未执行。本次未烧录，30秒录音和语义检测未实施。
- 未新增依赖、外部域名访问、全局配置修改或清理操作。修改仅包括录音判断和候选灯效代码以及本规格、离线唤醒规格的范围说明；保留当前亮度功能待验收记录，不清除无关修改。

### 6.1 长录音与流式接入（2026-10-11）

- 用户授权按60/120秒建议修改后，替换muse_qwen_asr.cpp/.h的文件式HTTP识别为WebSocket手动提交；muse_chat_session.cpp改为小块发送、排空后结束，不再保存完整PCM；muse/CMakeLists.txt声明已有WebSocket组件的编译依赖。voice.c分别限制自动60秒、S1 120秒，并即时消费上游错误；不删除原Muse语音便笺20秒逻辑，它不属于Qwen链路。
- 另行验证：S1连续口述20秒、40秒、90秒后松开，完整转写且只有一次Muse请求；自动录音说话超过原15秒仍继续；60/120秒上限触发后无Muse提交、可再次录音；短句、中途停顿、候选灯效、静音、取消、S1接管、断网、配网及重播回归；记录ASR连接/识别耗时、内部RAM/PSRAM和最大空闲块。
- 资料访问域名alibabacloud.com；没有新增依赖或实际ASR调用。官方当前价格表显示北京实时ASR按时长计费，没有该模型免费额度；现场测试前须取得费用授权。[官方价格表](https://www.alibabacloud.com/help/en/model-studio/model-pricing)
- 首次流式编译退出码2，日志esp32/tmp/easyinput/qwen-streaming-asr-build.log：Muse未正确声明WebSocket编译依赖，找不到esp_websocket_client.h。用户确认后，将已有组件以实际名称espressif__esp_websocket_client加入固定依赖列表，避免早期依赖扫描时CONFIG尚未解析而漏掉依赖；未下载或安装新包。
- 使用原构建命令重新编译，日志esp32/tmp/easyinput/qwen-streaming-asr-link-build.log，退出码0，未检出warning/error。应用2101248字节（0x201000），4MiB应用分区剩余约50%；SHA256为4cefd3d244b078fb9378b6ad638298447a44536fa0cd7f97ad5c0ebc4130c0b3。
- 在esp32目录执行 `python -m unittest tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter`，6项现有回归通过；`git diff --check`通过。首次误从仓库根目录运行测试导致模块路径找不到，随后在正确目录运行通过，没有改动测试或环境。这些回归不验证实时ASR协议或60/120秒录音现场表现。本次未烧录、未产生云端调用，运行资格待验证。

### 6.2 烧录与现场验证（2026-10-12）

- 用户确认烧录，并接受北京实时ASR计费，授权一次短句及一次约20秒录音测试。鲜读COM6芯片与MAC确认同一目标；只读取0x10000处3072字节分区表，与构建产物SHA256一致。
- 实际烧录命令 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin`，退出码0，写入校验通过；仅写应用，未写NVS、模型或分区表。日志esp32/tmp/easyinput/qwen-streaming-asr-flash.log。
- 自动复位后以115200波特率采集脱敏日志，未再次调用esptool扰动启动。应用heartbeat确认setup=done、wifi=up、ws=up、raw=up，无重启/崩溃证据；日志esp32/tmp/easyinput/qwen-streaming-asr-verify.log。短句和约20秒S1测试尚待用户操作与日志核验，不标为功能验收通过。
- 无新增安装、下载或全局配置修改；临时分区表副本及日志均保留在Workspace的esp32/tmp/easyinput，未清理文件。实际ASR测试会访问dashscope.aliyuncs.com，费用与服务可用性仍以现场结果为准。

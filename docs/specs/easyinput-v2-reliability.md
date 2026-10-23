# EasyInput V2：错误可诊断性与稳定性验收

日期：2026-10-23。用户同意继续完善，另明确选择“不改变恢复策略，仅完善提示”。本轮不烧录、不调用云端、不安装依赖、不修改DNS或升级工具链。

## 本轮行为

Qwen ASR/TTS将已有脱敏错误类别沿现有错误事件传递到显示层，不保存服务端错误正文、凭据或请求URL。分类只增加诊断信息，不改变超时、重试、录音结束或播放策略。新请求重置类别，取消保持原生命周期，不跨任务关闭HTTP句柄。403未提供额度耗尽错误码时显示禁止访问，不能武断显示额度不足；扬声器停止超时不能显示成网络超时。

OLED右侧最多15个ASCII字符，显示类别及对应操作：DNS/连接→检查网络，鉴权→检查Key，额度→检查额度，限流/超时→稍后尝试，403→检查访问权限，内存/缓冲→缩短回复，简报日期/来源校验失败→查看App文字。屏幕只返回固定短语，未知错误保持通用提示。沿用现有红灯错误显示周期，静音仍最高优先级。

等待完整重播缓存的LED_VOICE_REPLAY_WAIT高于普通暂停提示，显示`REPLAY: WAIT / AUTO RESUME`，底部提示S4取消等待。不让已按S3的用户继续重复按S3，不更改实际缓存及自动续播逻辑。

语音任务因唤醒初始化或麦克风停止失败而挂起时，保持原来的挂起行为；补全停止阶段诊断，OLED持续显示`VOICE: STOPPED / CHECK FIRMWARE`，不被充电、爱心或45秒显示休眠覆盖。没有启用“模型坏时保留S1”、自动重新加载模型或自动重启。模型区异常原因仍待追溯，重启不能保证修复损坏镜像。

## 本地回归

- 原始HTTP/服务错误类别与传递保留脱敏固定字面量；未知输入不能变成屏幕文字。
- 固定错误短语与操作提示均适配15字符；错误分类覆盖DNS、超时、鉴权、额度、限流、禁止访问、传输、缓冲、格式和本地音频错误。
- 静音、充电、暂停、等待续播、语音停止交叉时，显示优先级符合本节。
- 保留电量估算、电压、波浪号居中、音量/亮度显示与电台熄屏。
- 主机C/C++执行用例依赖现有编译器；缺少时明确跳过，不因跳过而宣布逻辑执行通过。现有GitHub host-tests作业会发现新增test_*.py，无需为本轮安装组件。

## 后续硬件验收清单（尚未执行）

先用户确认烧录；真实云端问答/故事/简报测试需确认调用次数与费用范围。不要故意写坏模型区或删除NVS来模拟故障；模型/硬件停止提示优先用主机故障注入测试，物理写入另行评审。

| 项目 | 成功标准 | 状态 |
|---|---|---|
| 正常显示回归 | 联网、静音、音量/亮度、电量估算保持正常 | 新版烧录后用户确认正常 |
| 等待续播 | 首次朗读暂停后S3显示等待，就绪后自动续播；S4撤销 | 等待及自动续播已通过；本版S4撤销等待尚未专项复测 |
| 错误提示 | 所有固定类别通过本地故障注入；未知错误不泄露原文，403不冒充额度耗尽 | 待C执行测试环境 |
| 30轮交互 | 逐轮完成问答/暂停/续播并回到空闲，无卡死、重启或遗留忙碌状态 | 待云调用范围确认 |
| 内存趋势 | 每轮结束稳定后记录内部空闲、最大块、PSRAM、语音phase/error；排查持续下降，不能只看总空闲 | 待采集 |
| 网络恢复 | 开机网络缺失、使用中断网、恢复网络均自动恢复；主连接与ASR/TTS分别验收 | 待场景测试 |
| 电台长播 | 两台分别核对音质/缺粮计数，60分钟自动停、末5分钟淡出，切台/关闭及时响应 | 待长时测试 |
| 发布复现 | 应用与模型哈希、分区、板型、工具链及依赖锁可重现；EasyInput完整组合CI构建 | 后续独立任务 |

当前目标是把故障定位和验收闭环补齐；不会把上述待测表或编译通过写成长期稳定性已经通过。

## 本轮执行记录

源码涉及Qwen ASR/TTS头文件及实现、muse_chat_session错误事件、voice阶段诊断、led_status固定提示发布、OLED渲染与对应测试，新增`voice_failure_text.h`和`test_voice_failure_text.py`。原有UI、电量估算及符号居中修改保留，未提交、推送或改动恢复策略。

在esp32目录沿用已审查的`tmp/easyinput/env.ps1`、现有EasyInput配置和metadata-python限制，执行`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`。ESP-IDF 6.0.1构建退出0，无编译器warning/error，日志`esp32/tmp/easyinput/oled-failure-replay-build.log`。应用2756608字节，4MiB分区余34%；SHA256为`6be3ff7395c0bf7d28144a7bc44a65d4ac04e79dccc91b0496439a97643d2876`。模型哈希仍为`b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007`。

从仓库根目录分别执行`python -m unittest discover -s esp32/tests -p <文件名>`，覆盖test_voice_failure_text.py、test_oled_text.py、test_qwen_tts_transport.py、test_briefing.py、test_story.py、test_easyinput_wake_partition.py，共22项：13项通过、9项因缺少主机C/C++编译器跳过。`git diff --check`通过。没有安装依赖、修改全局配置或DNS、访问云端生成服务、清理目录、刷机。构建仅沿用components.espressif.com和components-file.espressif.com只读元数据访问限制，不允许组件包下载。板上仍是用户已验收的波浪号居中版，新错误提示尚未烧录验收。

## 用户确认后的烧录记录

用户另行要求“刷机”。重新扫描COM6并通过esptool实读ESP32-S3、MAC `c4:9e:7e:07:9f:d8`，校验本节应用SHA256。读取0x10000起4096字节，前3072字节与本地分区表完全一致、余下1024字节均0xff。再核验USB身份后，执行`python -m esptool --chip esp32s3 -p COM6 -b 460800 --before no-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，退出0并报告写入数据哈希校验成功。仅更新应用，保留模型、分区表及NVS，日志`esp32/tmp/easyinput/oled-failure-replay-flash.log`。

硬复位后复扫同一USB身份，使用现有脱敏采集器采集40秒，日志`esp32/tmp/easyinput/oled-failure-replay-boot.log`。Muse控制连接已恢复、网络state=4、OTA应用验证成功、语音phase=7/error=0/ready=1，电池4208mV、external=1、charging=0；采集范围未见panic或重启循环。未主动调用云端、断网或模拟模型损坏。已请求用户核对OLED正常状态、电量/波浪号及S2静音进出；反馈尚待回复。新错误类别、等待续播、任务停止提示及30轮稳定性仍不能标为实机验收通过。

后续验收：用户回复“正常”，确认上述OLED正常状态、网络、音量/亮度、电量百分比/波浪号及S2静音进出通过。用户要求继续后，计划验证等待续播；重刷机后无旧音频缓存，已询问一次S1生成约200～250字回复及ASR/Muse/TTS调用许可，许可尚待回复，不沿用历史单次额度。复扫同一USB身份后开启180秒脱敏日志采集`esp32/tmp/easyinput/oled-replay-wait-validation.log`，此时仅采集，没有由程序发起云请求、改代码、刷机或修改网络设置。等待续播与其他专项仍保持待验收。

用户随后明确允许一次等待续播验收。设备phase=7/error=0/ready=1且空闲，关闭旧180秒采集以释放串口，复核原USB身份后开启600秒采集`esp32/tmp/easyinput/oled-replay-wait-acceptance.log`。已通知用户只触发一次S1，生成约250字回复，朗读约5秒后S4、S3，观察等待提示及自动续播；若缓存已就绪直接续播亦正常，不能据此认定等待分支已覆盖。不重复生成、不自动发起云调用；操作及日志结果仍待反馈。

等待续播验收结果：用户反馈“正常”。日志确认收到一次S1提问，S4在22.32秒（357120样本）处暂停；564314ms的S3记录`resume requested; waiting for complete audio cache`，575769ms缓存就绪（2188800字节、2条消息），约11.46秒后从同一357120样本位置自动续播；622005ms正常`replay done`，随后回到phase=7/error=0/ready=1/busy=0/paused=0。结合用户观察确认等待提示及自动续播通过，不是缓存就绪后的直接重播。采集已结束释放串口，本次仅使用获准的一次问答，没有追加云端测试。S4撤销等待、新错误类别及任务停止提示、30轮稳定性仍为后续专项。

# Spec：S5 AI简报与S6/S7电台交互

日期：2026-10-17。状态：当前板上为手动DNS修正版3b1d5e（192.168.1.1），已获用户确认烧录，启动及Muse主连接通过。S5完整简报试播尚未完成；换网需恢复自动DNS或重新确认地址。本文保留规格与验收标准，实施证据见末尾。

## 目标与范围

在保留语音问答、故事机、电台、静音、续播及旋钮功能的前提下，让S5一键生成并播放新闻简报。首期采用手动触发，默认主题为“今日科技与AI要闻”，3条、目标朗读约2分钟。

S6/S7继续服务电台；S8继续服务故事机。每天定时播放、主题配置界面、多份历史简报、推送订阅及新增新闻服务不在首期范围，不创建电脑端定时任务。

本规格定义目标映射：本地源码S5短按现已改为简报，长按为网络查询；用户确认烧录后板上已更新为本映射，完整简报试播待确认测试范围。早期文档提到的“S5长按新闻”不再作为此次开发目标，以本规格为准。

## 按键映射

| 输入 | 目标行为 |
| --- | --- |
| S5短按 | 正常就绪且空闲、未静音时，生成并播放一轮新简报 |
| S5长按约1秒后松开 | 本地网络状态查询与手动重试，沿用现有网络反馈能力，不请求新闻或云端TTS |
| S6短按 | 电台开／关，沿用现有行为 |
| S6长按 | 首期不增加动作，长按释放不误触短按 |
| S7短按 | 下一预置台；电台播放时实时切台，关闭时只改变选台，不自动开启 |
| S7长按约1秒后松开 | 进入原选台模式：旋钮选择候选台、停转800ms播台名、旋钮短按确认、5秒无操作取消 |
| S1 | 保持按住说话，可打断简报进入问答 |
| S2 | 保持软件静音 |
| S3 | 暂停后续播，播完后从头重播当前回复缓存 |
| S4 | 等待生成／尚未出声时取消；已经播放时暂停 |
| S8 | 保持故事入口、配对确认优先及长按恢复设置 |
| 旋钮 | 保持音量、亮度模式及选台模式的既有优先级 |

S5复用已有消抖，在释放时按持续时间判定：小于1000ms为短按，达到1000ms为长按。一次按住最多触发一个动作，长按不提前提交简报，释放后也不补触短按；S5任何时长均不成为恢复设置键。

忙碌时S5短按不叠加、不排队；静音时不启动简报；未配对或离线时沿用本地连接状态反馈，不伪装成正在生成。S5长按状态提示继续遵守静音及音频避让规则，不能抢占问答、故事或音乐。

## 简报内容与新闻真实性

请求Muse联网检索后生成简报，要求：

1. 使用北京时间说明简报日期；每条说明必要的新闻发生时间，不把旧事件重新包装成今日新闻。
2. 选择3条科技／AI要闻，每条交代发生了什么、为何值得关注；去除重复报道和宣传性填充，不用创作内容补足条数。
3. 文字回复附可核对的来源链接；朗读以新闻正文和必要来源名称为主，不念长URL、Markdown标记或检索过程。
4. 目标总朗读约2分钟，通过请求篇幅和实际测试校准，不用硬切音频实现时长，也不截断未完成的句子。
5. 检索失败、没有足够可核实消息时明确说明，不自动换新闻服务、不把缓存简报说成今日新消息。

联网检索、日期和引用均不能仅靠提示词宣称成功。开发准入时必须核实当前Muse服务能执行检索并给出可核对结果；如果当前服务不支持，本项能力需先评审，不直接实现一个仅凭模型记忆生成的“今日新闻”。

设备uptime不能代替日期。当前日期、时区与检索结果的可信来源及传递方式需要在开发评审时明确；该项未定前不承诺新闻时效性。

## 请求、播放和电台共存

S5采用文字请求Muse，再复用Qwen-TTS朗读；不录音、不调用ASR，不在按键任务中执行网络或音频操作。与故事／问答共用单一语音任务和扬声器仲裁，不另建可同时播放的新闻播放器。

简报流程为：空闲 → 提交请求 → 等待检索／生成 → 分段合成与播放 → 完成；错误或取消进入原清理流程，释放忙碌状态并允许下一次用户操作。

- 电台正在播放时，先暂停并断流，再运行简报；完整结束或取消后，按最新电台开关和选台状态回到直播点。原60分钟电台计时不因简报重置。
- 简报暂停期间电台保持暂停，避免音乐盖过后续续播；S3从暂停位置继续。该暂停所有权必须明确释放，不能遗留“电台永久暂停”。
- S1打断后沿用正常问答与缓存替换规则，电台等待新一轮问答结束后再恢复，不承诺同时保留被打断简报。
- 简报期间S6/S7不能抢占扬声器；如允许其更新电台开关／选台意图，须在简报结束后按最新意图恢复。
- 灯光复用等待、缓冲、播放、暂停和错误状态，保持用户保存的亮度。首期不新增一套需要记忆的简报灯语；静音与错误优先级不降低。

暂停时电台持续停播属于本规格要求，不能以“网络合成完成”等同用户整轮收听结束而提前恢复音乐。实现时需与现有故事／问答的音频所有权一起评审。

## 缓存和费用边界

一次S5短按代表一轮新的简报生成，并可能包含多段TTS请求，不等于仅一个HTTP请求。重复按键不重复提交，不增加未经用户触发的付费重试。

S3优先使用现有音频缓存，不重新检索、生成或调用TTS。继续共用当前一条回复的4MiB缓存：新问答／故事／简报可能替换旧缓存，超长或内存不足时沿用已有容量提示，不增加多份简报归档，不承诺失败后一定保留上一条缓存。

离线状态查询和已缓存重播不依赖新的云端生成；无缓存时明确反馈，不静默重新生成。开发测试中的真实检索、生成和TTS调用需要事先确定范围，不能把一次试播授权用作无限次调用。

## 开发准入与已知前置项

- 先修复或明确处理review发现的TTS取消阻塞：取消后旧HTTP任务仍可能占用至90秒，不能让下一轮简报长期无法启动。
- 补充脱敏错误分类，区分DNS、鉴权、额度／欠费、限流和超时；不打印Key、签名URL或完整响应正文。
- 核实Muse检索和可信日期能力，并用真实结果验证来源与发生时间。
- 当前设备已恢复DHCP自动DNS；过去手动DNS版短句通过，不能作为当前网络、完整简报或自动DNS语音链路通过的证据。不把固定路由器DNS加入通用出厂配置。

## 验收标准

1. S5短按只提交一轮简报；忙碌重复按不排队；静音、未配对、断网均不触发新闻生成。
2. S5长按只触发一次原本地状态查询／重试，无新闻或ASR请求，松开不补发短按；S8配对与长按重置不受影响。
3. 检索确实执行，文字日期、事件时间和来源可核对；旧闻、来源缺失、检索失败不能被标成已验证今日新闻。
4. 新闻文字与朗读内容对应，正常情况下完整播放约2分钟；无URL朗读、无截句，并记录实际字数与播放时长。
5. 等待阶段S4取消后不再提交过期请求；播放中S4暂停、S3续播，播完S3从头重播，重播无新云调用。
6. S1打断进入问答，后续S5/S8正常可用；取消、错误和静音均不留下忙碌状态或音频占用。
7. 电台播放时触发简报，音乐暂停；简报暂停不擅自恢复音乐，完成／取消后正确恢复直播点；S6关闭意图、S7选台与60分钟计时保持。
8. 缓存不足明确提示，超长回复不崩溃；记录内部RAM、PSRAM、最大空闲块及连续多轮后的资源回收。
9. 使用可控的错误输入／模拟失败验证DNS、鉴权、额度、超时分类，避免为覆盖失败路径反复消耗真实服务。

## 2026-10-17实施记录

### 检索与日期准入样本

用户批准一次仅文字能力测试，并在Muse App提交真实检索请求、提供文字回复。回复给出北京时间2026-10-17 18:07、3条事件时间和来源。本轮没有读取本机真实凭据，也未自动调用ASR/TTS。

网页工具无法打开链接后，使用本机只读HTTPS请求核对同样3个公开原文，均HTTP 200，标题、正文主题与发布日期对应：

- [东方日报原文](https://www.orientaldaily.com.my/news/international/2026/10/17/855901)：页面标注2026-10-17发布，内容涉及腾讯AI代理与高德地图。
- [腾讯新闻转载原文](https://news.qq.com/rain/a/20261017A03SSS00)：页面标注2026-10-17 09:19，主题是AI蠕虫与自主复制。
- [新浪财经转载原文](https://finance.sina.com.cn/roll/2026-10-17/doc-inivnvku1399539.shtml)：页面标注2026-10-17 08:41，主题是科技股及半导体股下跌。

这支持当前Muse能给出可核对的实时新闻样本；不是对模型每轮内容正确性的保证。当前API未提供客户端可独立核验的工具调用证明，也未在本轮验证板端简报请求。运行时的北京时间及事件时间由Muse现有实时检索能力提供，随最终文本传递；设备不把uptime当日期、不增加NTP或新闻服务、不修改系统时钟。固件只校验日期格式、正文日期一致和来源段存在，不能把格式校验当作事实核查。该限制需在实机简报内容验收中继续关注。

### 已实现行为

- `esp32/main/button.c`：S5在释放时判定，<1000ms提交简报，>=1000ms仅原网络查询；S6短按、S7切台/选台保持，S8配对与重置分支未改。
- `esp32/main/voice.c`：新增独立简报事件，空闲/未静音/已配对/联网就绪才入队，不录音、不调用ASR。忙碌重复请求忽略；执行前再核对连接及取消状态。
- `esp32/main/briefing_prompt.h`：请求3条科技与AI新闻、350～400汉字、完整句子，目标约2分钟。具体时长仍待实测校准，不用音频截断凑时长。
- `esp32/components/muse/muse_chat_session.cpp`与`muse_briefing_text.h`：单轮文字请求复用原会话。最终消息以北京时间、朗读正文和参考来源分段；仅合格正文进入TTS。缺日期、日期不一致、少于3个不同HTTPS链接、正文包含URL或Markdown时不朗读，不自动重试；Muse App保留原始文字及来源。进度消息不朗读；整轮没有合格正文则沿用错误反馈。
- 复用S3/S4和4MiB缓存。简报暂停期间即使合成完成也不恢复音乐；续播完成、取消、静音、重置或缓存失效时按现有播放器退出条件交还电台。S1打断后转为普通问答；S6/S7仍只更新电台意图，睡眠定时逻辑未改。
- `muse_qwen_tts.cpp`：HTTPS异步连接与短等待读写，在连接/写入/等头/读音频之间核对generation；取消不跨任务销毁HTTP句柄，不调用会重新连接的`esp_http_client_cancel_request`，不新增付费重试。连接后单次I/O等待250ms，合成总等待预算90秒、下载无进展预算10秒；DNS解析及TLS内部调用仍受平台行为影响，不宣称取消全阶段严格250ms。
- `muse_qwen_error.h`与`muse_qwen_asr.cpp`：固定类别日志区分dns/auth/quota/rate_limit/timeout/forbidden/transport等；403只有明确额度/欠费错误码才归quota。不输出Key、服务端错误原文或音频签名URL。
- 不更改分区、GPIO、电台音源、模型、主机DNS及现有自动DNS配置；依赖锁版本/哈希及7个第三方组件源码哈希保持基线。

### 本地验证与设备验收边界

- 定向检查：30项，21通过、9跳过。新增了实际生产函数的按键边界/准入、简报格式与可控HTTP取消/超时/错误输入测试；因当前环境没有主机C/C++编译器，4项新增的函数执行测试和原有5项执行检查未运行。静态检查通过不等于这些执行场景已验证。
- 全仓检查：`python -B -m unittest discover -s tests -p 'test_*.py'`，153项，2失败、25错误、34跳过。25错误和配对握手失败均涉及主机编译器缺失；另一个失败为基线已有的`radio`日志标签未按`link.`命名。本轮不安装工具或修改无关电台日志代码，不声称全仓测试通过。
- 最终构建退出0，无编译器warning/error。应用2756608字节，4MiB分区余34%；SHA256=`a0d21b34c7860084d7e02dfcab466a9289b8f89025619ae39446055379e29ffb`。产物为`esp32/build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，日志`esp32/tmp/easyinput/ai-briefing-final-build.log`；精确核对构建宏DNS为空，仍为自动DNS。依赖锁8项及7个第三方目录哈希通过。
- 尚未烧录。完整简报、实际朗读时长/字数、S4/S3与电台共存、收费调用次数、内存回收与连续多轮稳定性，均仍需用户确认烧录及测试范围后在设备上验收。自动DNS下完整云端链路仍未由本轮测试证明。

### 执行与产物

构建实际命令（在`esp32`中，先加载现有`tmp/easyinput/env.ps1`，使用原本地defaults、`IDF_COMPONENT_MANAGER=1`和受限`metadata-python`）：

```powershell
python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build
python -B -m unittest tests.test_briefing tests.test_qwen_tts_transport tests.test_story tests.test_network_feedback tests.test_radio_lifecycle tests.test_radio_assets tests.test_radio_stack tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter tests.test_easyinput_buttons tests.test_easyinput_replay_seek
```

本轮新增联网仅核对上述3个公开原文；构建沿用只允许乐鑫依赖元数据、禁止组件下载和升级的限制。未安装依赖、未改全局环境、未清理或删除目录、未提交推送。`esp32/tmp/easyinput/ai-briefing-*.log`和`briefing-source-check.json`为本地核验产物，源码及本地凭据应继续按现有忽略规则处理，不将带凭据的配置或完整构建目录推送仓库。

### 2026-10-17烧录与启动核验

用户明确要求“烧录验证”。重新扫描COM6/USB序列、读取ESP32-S3 MAC确认c4:9e:7e:07:9f:d8；只读设备0x10000起0xc00分区表与构建逐字节一致，重新核验应用SHA256为`a0d21b34c7860084d7e02dfcab466a9289b8f89025619ae39446055379e29ffb`，仅执行：

```powershell
python -m esptool --chip esp32s3 --port COM6 --baud 460800 --before no-reset --after hard-reset write-flash 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin
```

写入2756608字节，哈希校验成功，未写NVS/分区/模型。烧录后重新扫描USB身份，使用原脱敏采集器采集55秒，`ai-briefing-startup.log`记录16秒控制连接建立、17秒OTA应用验证、网络状态ready及后续wifi/ws/raw均up、离线本地监听恢复；样本无panic、看门狗或重启循环。日志早期两条AFE空缓冲警告后正常恢复监听，不宣称没有任何警告。采集末内部空闲约31KiB/最大块21KiB、PSRAM约4417KiB；这些是空闲样本，不代替简报多轮内存验收。音量日志为100，工具未调整设置。

`ai-briefing-identify.log`、`ai-briefing-partition-read.log`、`ai-briefing-flash.log`与启动日志均在`esp32/tmp/easyinput/`。串口采集已结束并释放；尚未主动发起新ASR/TTS或简报。已询问一次完整S5简报及多段Qwen-TTS试播许可，收到确认后先开启阶段采集再指导短按S5，不复用此前仅文字能力测试许可。

### 首次S5试播失败与只读DNS诊断

用户批准一轮完整简报，首次未接串口未触发；连接后COM6/USB序列确认，开启420秒采集，用户短按一次S5并报告闪红灯。`ai-briefing-first-play.log`记录S5按下/松开约121ms、事件进入briefing、随即`hatch.metaaivm.com`的getaddrinfo返回202，最终`CAN'T REACH MUSE`。本轮未见chat/stream提交成功或Qwen-TTS开始，不能据此判断ASR/TTS额度，也没有验证到正文格式、音频链路或电台共存。不自动重复试播。

用户明确允许只读核验热点/DNS，对Muse及Qwen域名执行DNS/TCP443测试：

- 热点地址192.168.137.1及本机127.0.0.1，对`hatch.metaaivm.com`、`dashscope.aliyuncs.com`的A/AAAA查询均rcode=2（SERVFAIL）、0答案。
- 路由器192.168.1.1对同样查询均rcode=0，返回地址；此检查未验证地址真实性或完整TLS/服务调用，不能据此承诺固定DNS可修复所有环节。
- 电脑系统解析与两个域名的TCP443连接成功。这只能证明电脑当前路径可连，不代表开发板经热点DNS可用。
- Wi-Fi接口5的DNS仍为[127.0.0.1,192.168.1.1]，热点接口33的DNS为127.0.0.1。当前存在singbox_tun等网络适配器；不能仅凭存在断言其导致故障。
- UDP127.0.0.1:53由`SangforServiceClient`（PID11672）监听；热点接口与0.0.0.0:53由svchost（PID4264）监听。局部证据表明当前DNS代理/转发链失败，不足以排除其它转发/策略影响或认定单一软件责任。

只读命令为`Get-NetIPConfiguration`、`Get-DnsClientServerAddress -AddressFamily IPv4`、`Get-NetUDPEndpoint -LocalPort 53`及按对应PID读取进程名；Python标准库发送固定两域名的UDP DNS查询和TCP连接，未发送HTTP/ASR/TTS请求，脱敏结果保存`esp32/tmp/easyinput/ai-briefing-dns-diagnostic.json`。未修改DNS、VPN、代理、防火墙、服务或固件；维持用户要求的板端自动DNS。采集已主动结束释放串口。

已请用户选择：保持自动DNS、换独立手机热点再确认单轮简报；或先制定有回滚步骤的Windows热点DNS修复方案。系统修改仍需明确确认，不自动恢复板端固定DNS，不重复此前退出VPN界面后无效的步骤并声称解决。

### 最新复现及已批准的解析兼容修复（待新版本烧录）

后续用户批准一次故障复现，`ai-briefing-repro-latest.log`记录80ms短按S5、POST /chat/stream收到ack，约104.1秒后返回1条文字，随后本地`BRIEFING DATE/SOURCES UNAVAILABLE`。未进入Qwen-TTS，本轮不能继续套用前次DNS失败原因，也不能视为热点DNS已永久修好。采集已结束释放串口。

用户提供本次完整App文字，保存为`esp32/tests/fixtures/briefing-plain-20261017.txt`。样本首行ISO日期，正文中文日期，末尾编号3条来源（2个HTTPS、1个HTTP，第三个URL紧接中文破折号），没有固定分段标记。旧实现首先因缺标记拒绝；只补标记后仍有日期字面比较及HTTPS来源数量两个不兼容点。原spec要求日期和来源可核对，没有要求这些显示形式。

用户明确允许修正源码、补样本用例、原环境编译，不调用云端，烧录另行确认。已修改：

- `muse_briefing_text.h`兼容原固定标记以及“首行日期＋正文＋末尾1.开头的编号来源列表”。日历校验仍在，ISO与中文日期先转换为年月日再核对正文的北京时间，不把新闻事件的昨日日期误作简报日期。
- 参考URL计数接受HTTP和HTTPS，中文破折号等分隔符不吞入URL；同地址的HTTP/HTTPS只计一份来源，至少3个不同地址。设备不访问这些链接，不更换新闻源，不验证每个链接的真实性，不把格式检查当事实核查。Qwen音频下载的HTTPS及域名限制未改。
- 只提取正文用于TTS，末尾来源留在Muse App。正文仍不能有URL或Markdown；无日期、无正文、日期不一致或来源不足仍拒绝，不自动付费重试。
- 日志输出固定原因码，如header_date_invalid、spoken_date_invalid、date_mismatch、sources_missing、distinct_sources_less_than_three、body_markup、body_url等，不输出完整消息或凭据。
- `test_briefing.py`增加实际样本提取及拒绝原因用例，覆盖缺来源、重复来源、日期错误、正文链接/Markdown、中文日期和破折号；拒绝不修改输入。样本中的新闻仅作为用户提供的解析输入，本轮未联网核验其事实。

本轮定向32项检查22通过、10因无主机C/C++编译器跳过，实际解析函数的回归用例未执行，不能记成通过。全仓155项检查仍2失败、25错误、35跳过，原因仍为主机编译器缺失及原有radio日志标签，不修改无关代码。日志ai-briefing-parser-fix-tests.log、ai-briefing-parser-fix-all-tests.log在esp32/tmp/easyinput。

最终原环境构建退出0，无编译器warning/error。应用2756608字节，4MiB分区余34%；SHA256=`b5ae83c68482e3ef51be5b96cffae7e5c3ca8cc8d2c471aff35afcc9771d645f`，产物`esp32/build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，日志`esp32/tmp/easyinput/ai-briefing-parser-fix-final-build.log`。8项依赖锁及7个第三方目录哈希保持基线；构建DNS宏仍为空（自动DNS），git diff --check通过。本轮沿用前述构建/测试命令，不访问新闻链接、不调用云端、不安装或升级依赖、不改全局环境、不执行清理。

本轮未烧录、未提交推送。实际板上仍a0d21b旧解析器；本地构建目录bin已更新，不能用目录当前bin代替设备版本。新版本需用户单独确认后只写应用；完整简报重试涉及新的Muse/TTS调用范围，不擅自重复。无需清理用户配置；本地日志和构建产物沿用忽略规则保留供验收。


### 2026-10-17解析兼容修正版烧录与启动核验

用户明确要求“烧录”。重新扫描COM6/USB序列，读取ESP32-S3 MAC确认c4:9e:7e:07:9f:d8；设备0x10000起0xc00分区表与本地构建逐字节一致，应用2756608字节、SHA256=`b5ae83c68482e3ef51be5b96cffae7e5c3ca8cc8d2c471aff35afcc9771d645f`核验一致。沿用前述esptool命令，仅写0x20000应用，退出0，Hash of data verified。未写NVS、分区表或模型，未调整Wi-Fi、配对、音量、亮度与自动DNS设置。

烧录后鲜扫串口并以原脱敏采集器采集55秒。启动早期api.muse.ai与hatch.metaaivm.com出现getaddrinfo返回202，自动重连后43秒控制连接建立、OTA image validated，约44秒network ready，46～61秒wifi/ws/raw持续up，本地监听恢复。一次AFE空缓冲警告后恢复；采样未见panic、看门狗或重启循环。不能把主连接恢复当作热点DNS已永久修好，也不能替代S5生成与TTS验收。

实际执行为read-mac、read-flash 0x10000 0xc00、Python本地分区/大小/哈希比较、write-flash 0x20000，以及capture-asr.py COM6 55。证据为esp32/tmp/easyinput/ai-briefing-parser-fix-identify.log、partition-read.log（同前缀）、flash.log（同前缀）和startup.log（同前缀）。未新增工具联网诊断；固件自身访问Muse服务完成自动重连。未安装依赖、改系统设置、执行清理、提交或推送。串口采集已结束。未主动发起新简报、ASR或TTS；下一步完整S5试播需要新的单轮调用授权。此前主机测试环境限制保持不变，不因烧录成功而记为通过。


### 修正版单次诊断：S5立即红灯、无新文字

用户另准一次诊断试播。ai-briefing-post-fix-repro.log记录S5约100ms短按后，新建Muse连接对hatch.metaaivm.com的getaddrinfo返回202，随后CAN'T REACH MUSE，尚无chat/stream确认、解析或TTS开始。同期wifi/ws/raw心跳up只代表已有连接可用。本次不能验证解析修复，也不能解释此前已有文字无声的不同轮次。未更改源码、自动DNS、系统网络或烧录；采集已主动结束释放串口，不自动重复付费测试。后续应先隔离Windows热点DNS路径，再单独授权完整云端验收。


### 临时板端DNS修正版编译通过，尚未烧录

- 用户明确批准板端DNS临时192.168.1.1并编译，烧录另确认。仅将本地tmp/easyinput/sdkconfig.soft-aes-s3的CONFIG_HOMEHUB_WIFI_DNS_IPV4单键改为192.168.1.1，沿用已有sdkconfig.dns.local；没有改业务源码、电脑DNS、VPN、代理、防火墙。指定DNS适用于当前网络，换网需恢复自动或重新确认地址。
- 原ESP-IDF6.0.1及受限metadata-python构建退出0，无编译器warning/error匹配；应用2756608字节，4MiB槽余34%。SHA256=3b1d5e7ab13bf74c5f2ad8a3dfb31ef38697a360867a149d369ee45901c46a14。生成头文件精确核验DNS=192.168.1.1，8项依赖锁版本/哈希保持基线，分区表前0xc00字节与上次设备读取一致。
- 定向17项：10通过、7因缺主机编译器跳过，日志ai-briefing-manual-dns-tests.log。前次全仓测试失败/环境缺口尚未解决，本轮未重跑全仓，不宣称全部测试通过。构建日志ai-briefing-manual-dns-build.log；脱敏大小/哈希结果ai-briefing-manual-dns-build-result.json，均在esp32/tmp/easyinput。
- 实际命令沿用idf.py -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build；测试python -B -m unittest tests.test_briefing tests.test_qwen_tts_transport tests.test_story tests.test_network_feedback。沿用官方components.espressif.com/components-file.espressif.com只读元数据限制，禁止下载包、升级及managed_components清理。未安装新依赖、执行清理或提交推送。
- 尚未烧录、未调用ASR/TTS/Muse生成。板上仍b5ae83自动DNS版；本地bin现为3b1d5e手动DNS版，两者必须区分。下一步用户明确确认后鲜核同一MAC与分区，仅写应用，先验DNS应用与主连接；完整S5试播涉及新一轮云端调用范围，需另明确。当前简报解析和完整朗读仍未验收。


### 当前简报手动DNS修正版烧录与启动核验

- 用户明确要求烧录。鲜扫COM6/USB序列C4:9E:7E:07:9F:D8，esptool读MAC c4:9e:7e:07:9f:d8，设备0x10000起0xc00分区与本地逐字节相同，核对应用2756608字节/SHA256=3b1d5e7ab13bf74c5f2ad8a3dfb31ef38697a360867a149d369ee45901c46a14。仅写0x20000，退出0、Hash of data verified；未写NVS、分区、模型，保留Wi-Fi/配对/音量/亮度。
- ai-briefing-manual-dns-startup.log采55秒：约12秒VM API HTTP200及TLS成功，14秒控制连接与OTA image validated，15秒ready、16秒chat/subscribe建立，18～63秒wifi/ws/raw持续up，样本无DNS错误/panic/WDT/重启循环；一条AFE空缓冲警告。早期启动日志未完整捕获，没有直接采到manual DNS applied，DNS配置由编译头文件192.168.1.1与烧入哈希交叉确认，不冒称采到该日志。
- 实际执行read-mac、read-flash 0x10000 0xc00、Python本地大小/哈希/分区核验、esptool write-flash 0x20000及capture-asr.py COM6 55。identify/partition-read/flash/startup日志前缀均ai-briefing-manual-dns-，在esp32/tmp/easyinput。未安装依赖、改电脑设置、清理目录或提交推送。采集已结束释放串口。
- 板上现为3b1d5e手动DNS版，之前b5ae83自动DNS已替换。192.168.1.1仅适用于当前网络，换网需恢复自动DNS或重新确认地址。本轮未触发简报/ASR/TTS；S5完整朗读、格式校验与音频下载仍需新的单轮云端验收授权。此前17项检查10通过7跳过、全仓环境缺口保持原状态，不因启动成功升级为通过。

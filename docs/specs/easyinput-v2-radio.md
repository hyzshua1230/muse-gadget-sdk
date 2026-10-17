# EasyInput V2 睡前音乐电台规格草案

状态：交互已确认；用户选择寻找其它国内MP3音乐台、保留原解码方案，默认候选采用已通过电脑短时检查的上海经典947，第二台为SomaFM Drone Zone。已接入代码并编译成功，未烧录，现场功能待验收。

## 1. 已确认范围

- S6短按开关电台，S7短按实时切预置台。保留当前S1按住说话、S2静音、S3续播/重播、S4暂停/取消、S8配对/重置，以及旋钮音量/亮度；S5目前仅记录输入，不增加回环。
- 首次实际输出音频后按单调时钟计时60分钟，最后5分钟淡出；切台、断流、AI暂停不重置截止时间。手动开关短淡入淡出。
- 播放中5颗灯灭；退出移除临时覆盖，显示当前有效状态，不恢复过期灯色。不改变保存的亮度；独立工作灯首版保持现有行为。
- AI录音、识别、等待回复、回复播放和S3重播期间，关闭电台流并让出扬声器，整轮结束后从直播点恢复。到60分钟时即使处于AI暂停也关闭电台，不打断AI。错误和取消路径同样需要释放播放所有权。
- 既有板级扬声器接口是48kHz、32bit、立体声交错帧，电台通过原接口输出，复用现有音量和静音控制。不承诺实体单喇叭具有立体声声场。
- 音频及解码工作缓冲放PSRAM；I2S DMA和网络内部内存遵守原驱动约束，不承诺全部在PSRAM。电台不保存音频、不写Flash，既有音量/亮度设置保存行为保留。
- 首次连接失败或意外断流最多重试4次，之后干净退出；主动切台、关闭、AI暂停不计为失败。连接/读取及停止等待必须有界，关闭期间不得产生过期重连。MP3链路处理重定向、ICY元数据和跨块帧。

## 2. 音源核验（2026-10-12）

### 默认台：上海经典947

频道身份：https://m.qingting.fm/channels/267/ （蜻蜓FM标明上海经典947）。

广播机构介绍：https://www.smg.cn/review/202212/0164882.html 。介绍涵盖古典音乐、音乐剧、爵士、民乐、世界音乐和轻音乐；直播不是纯助眠曲目库，不保证无人声、无广告或所有节目音量平稳，也不以2022年介绍作为2026年节目时间表保证。

主地址：http://lhttp.qingting.fm/live/267/64k.mp3

同台HTTPS地址：https://lhttp.qingting.fm/live/267/64k.mp3

当前电脑网络分别读取32768字节，两者均HTTP 200、Content-Type=audio/mpeg，连续MP3帧头均显示64kbps、44100Hz、2声道。HTTPS候选 https://lhttp-hw.qtfm.cn/live/267/64k.mp3 也通过相同短时检查，但不额外加入运行时备用域名，保持用户要求的HTTP优先、同地址HTTPS兜底。上述检查没有完整解码、实际收听、开发板播放或长期测试。

另一候选 http://lhttp.qingting.fm/live/5022107/64k.mp3 同样返回200、64kbps/44100Hz/双声道MP3；尚未充分核验频道身份，不列入预置台。

预置顺序：上海经典947 → SomaFM Drone Zone。用户允许另找国内MP3台后采用上述候选，不保留央广失效MP3地址，也不引入HLS/AAC支持。

### SomaFM Drone Zone

官方链接页面：https://somafm.com/dronezone/directstreamlinks.html

候选流：https://ice5.somafm.com/dronezone-128-mp3

当前电脑网络短时读取32768字节：HTTP 200，Content-Type=audio/mpeg，icy-name识别Drone Zone；连续MP3帧头显示128kbps、44100Hz、2声道。未进行完整解码、开发板播放或长期稳定测试，不能据此断言所有国内网络可达。官方说明直链可能变化，且限个人使用；商用分发需另行核验使用条件。频道不保证绝无人声。

### 央广经典音乐广播

官方频道介绍：https://ent.cnr.cn/ylzt/goldenradio/

可访问候选播放列表：https://ngcdn001.cnr.cn/live/dszs/index.m3u8

当前电脑网络：HTTP 200，Content-Type=application/vnd.apple.mpegurl，列表包含约10秒的TS分段；读取一个分段65536字节，PMT识别音频stream_type=0x0f（AAC），不是原MP3链路。尚未实际收听确认节目内容、验证停播时间或开发板可达性。

蜻蜓频道389的旧MP3候选检查结果：

- http/https://lhttp.qingting.fm/live/389/64k.mp3：HTTP 404。
- http/https://live.qingting.fm/live/389/64k.mp3：当前电脑DNS解析失败。
- https://lhttp.qtfm.cn/live/389/64k.mp3、https://lhttp-hw.qtfm.cn/live/389/64k.mp3：HTTP 404。
- https://ngcdn004.cnr.cn/live/dszs/index.m3u8：HTTP 403。

不将失效链接作为预置台，不把停播时段写死为已验证事实。用户已选择继续寻找其它国内MP3音乐台、保留原解码方案；央广作为调查记录保留，不列入首版预置台。

## 3. 实现边界

拟新增radio.c/h，项目构建清单注册源码，Kconfig增加功能开关；button.c接S6/S7，led_status增加临时灭灯覆盖，voice.c协调AI/重播与电台。原板级音频接口尽量复用，确需变更时只作相关的播放仲裁。Meta第三方SDK及minimp3原样保留，不重构既有修复。

用户确认：电台开启期间暂停离线唤醒，S1仍可提问；关闭后恢复既有离线唤醒条件。本地选台提示期间同样暂停唤醒，避免监听自身声音。没有回声消除资格验证，不开启播放时免按键唤醒，不自动换台。

本轮实现：S7按住至少1秒后松开进入选台模式；旋钮旋转仅选候选台，停转800ms后播报台名，音乐临时压低；旋钮短按确认并恢复调音量，5秒无操作取消。未播放时也可选台并听到本地提示，但确认不自动开启电台。两段提示经用户授权用本机已有中文语音组件生成，嵌入固件，不调用云端TTS。播放期间独立工作灯保留既有行为。

S1最终转写的明确命令由本地处理：打开/关闭收音机或电台、下一台/上一台、播放上海经典947（或九四七）、播放/切换到环境音乐台；清理常见中英文标点和空格后精确匹配，不用模糊子串。当前仅两台，上一台与下一台都切到另一台；在关闭状态切台仅改变预置选择。不匹配的普通问题仍走原Muse对话，没有新加语义猜测或自动执行模糊台名。

音频采用minimp3原样解码，跨帧线性重采样到48kHz/32bit双声道；独立HTTP任务写64KiB PSRAM下载缓冲，解码任务输出喇叭，避免同步网络读取阻塞I2S。音频工作区及两个任务栈使用PSRAM；网络客户端与原I2S驱动内部内存保持原约束。首版不声明高保真音质指标。

短淡入1秒，主动关闭/切台沿已缓冲音乐淡出约200ms；AI暂停优先让出喇叭，淡出约20ms后排空DMA；网络中断且缓冲耗尽时无法保证同样长度的淡出。首次成功提交喇叭音频后计时60分钟，最后5分钟线性淡出。网络读/连接超时1000ms，最多4次重定向，初次失败后重连4次，退避1/2/3/4秒，预算在新开机播放或切台时重置。意外断流不会自动切到另一频道。

AI先暂停电台并等待输出释放（最多4秒），失败时不启动另一播放；正常结束后等待既有播放器排空再恢复电台。超时/取消以播放代次隔离，切台和重连不重置睡眠定时。内部RAM、TLS峰值、切台时延、实际声音与中断路径尚需实机核验。

## 4. 验收计划

两台实机连接及MP3解码/重采样；S6重复开关、S7实时切台；S1对讲与S3重播独占扬声器；S2静音和旋钮音量/亮度保持；灯灭及当前状态恢复；主动关闭能中断连接/重试；断网最多4次重试后恢复可用；切台不重置计时；完整60分钟与最后5分钟淡出；持续播放的内部RAM/PSRAM、最大空闲块、无崩溃及资源回收。

## 5. 音源调查记录

仅只读检查当前代码、查询官方页面、使用本机Python urllib进行有界短读及curl保存公共频道页面。访问主要域名cnr.cn、qingting.fm、qtfm.cn、somafm.com及相关CDN；未安装依赖、未运行外部脚本、未修改全局配置、未调用收费ASR/TTS、未烧录。探测产物留在esp32/tmp/easyinput/radio-*及qingting-389-page.html，未清理。该调查阶段未修改业务代码。

## 6. 功能实现记录（2026-10-12）

- 用户明确授权实现和编译，烧录必须再确认；另行确认电台期间暂停离线唤醒，并允许核验Windows内置中文语音、仅在Workspace生成提示音。已读取安装声音名单，使用Microsoft Huihui Desktop经System.Speech生成WAV，以Python wave提取PCM；未安装系统组件、未修改系统设置、未调用云端TTS。提示资源位于esp32/main/radio_assets，WAV原件及PCM均保留。
- 新增radio.c/h及radio配置overlay；修改main构建清单、Kconfig、button.c、led_status.c/.h和voice.c进行任务与按键接入；muse_chat.h与muse_chat_session.cpp增加本地命令回调入口，只有精确匹配的电台指令提前结束本轮，普通Muse请求路径保留。未修改第三方SDK或minimp3源码，未清除先前改动。
- 新增提示资源验证test_radio_assets.py，2项通过；现有分区/音频过滤6项通过；按键和重播测试两组因主机C/C++编译器不可用而跳过，不能算通过。没有安装主机编译器；这些测试不覆盖实际网络播放、命令执行和定时状态机。
- 沿用已审查、授权的ESP-IDF 6.0.1构建流程和相同build-easyinput-v2-wake模型产物目录；本轮采用新的生成配置tmp/easyinput/sdkconfig.radio，不编辑此前生成的配置。默认overlay追加devices/sdkconfig.easyinput-v2-radio，既有本地token/Qwen配置仅由构建使用，不读取或输出凭据。实际命令 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake -D SDKCONFIG=tmp/easyinput/sdkconfig.radio build`，日志esp32/tmp/easyinput/radio-build.log，最终结果待记录。
- radio默认关闭，未执行串口、烧录或设备上的收费语音调用。无需新增组件包；未修改全局环境，未删除或清理文件。最终构建与第4节现场验收结果必须分别记录。
- 首轮radio-build.log编译退出码0。审查时补充下载/解码任务隔离、播放所有权及提示音平滑压低音乐后，沿用相同构建命令执行radio-final-build.log与最终radio-verified-build.log，均退出码0；最终日志未检出warning/error，`git diff --check`通过（既有ASR头文件只有换行格式提示）。未发生编译失败后的环境变更或方案替换。
- 最终应用2297856字节（0x231000，约2.19MiB），4MiB应用分区剩余约45%；SHA256为2a7971100f3647f39cd999846d60dc8390497f89a1e9361fd60c2c55fd5353f5。通过已安装工具链的nm核验ELF含radio_init、radio_voice_command及两段PCM嵌入符号，确认功能实际编入，未读取生成配置中的凭据。产物esp32/build-easyinput-v2-wake/muse-gadget.bin。
- 尚未验证：开发板两台可达性、实际音乐音质/连续性、台名播报观感、按键/旋钮/语音命令、静音、AI暂停恢复、断网重连、满60分钟计时与末5分钟淡出、内部RAM/TLS峰值及长期稳定性。原流式ASR的短句/20秒现场结果仍待核验，不以本轮编译通过替代任何设备测试。烧录仅在用户下一次明确确认后进行。

## 7. 电台版烧录与现场验证（2026-10-12）

用户明确请求“刷机验证”。COM6核验为ESP32-S3，MAC c4:9e:7e:07:9f:d8；读取板上0x10000长度0xc00的分区表，与本次构建产物哈希一致。仅更新0x20000应用，不写启动程序、分区表、模型区或NVS，保留Wi-Fi、配对和亮度设置。

实际执行：`python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-flash 0x10000 0xc00 tmp/easyinput/partition-radio-check.bin`；随后 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin`。烧录退出码0，日志radio-flash.log显示Hash of data verified。应用SHA256沿用第6节最终产物。

执行已审查的有界脱敏采集：`python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/radio-validation.log`。启动后心跳确认setup=done、wifi=up、ws=up、raw=up，内部空闲约31KiB、最大块13KiB，PSRAM约4851KiB。初始记录未见崩溃；这只确认启动联网，不代表电台功能通过。

已请求用户验证S6播放/关闭、S7切台、旋钮选台与台名提示、五灯熄灭和恢复，结果待反馈。尚未执行设备语音收费调用、60分钟定时、末5分钟淡出或断网重连现场验收。本次未安装依赖、未访问新增下载域名、未改全局配置，临时日志留在Workspace，未清理。

## 8. 首次电台杂音与按键失效修复（2026-10-12）

用户反馈“都是杂音”，进一步确认按键均不生效。首次采集在station=0 decoded 44100Hz 64kbps channels=2之后中断；采集进程报USB串口ClearCommError，不能单独将断串口当作重启证据。重新有界采集radio-noise.log未收到新日志。

对现有minimp3对象文件执行 `xtensa-esp32s3-elf-objdump -d build-easyinput-v2-wake/esp-idf/minimp3/CMakeFiles/__idf_minimp3.dir/src/minimp3.c.obj`，解码入口机器指令为 `entry a1, 0x40e0`，单函数栈帧16608字节，已经超过原电台任务16384字节栈。此为确定的栈容量错误，可导致内存破坏、杂音和卡死，但最终声音恢复仍须新固件现场验收。

精准修改main/radio.c：任务PSRAM栈增至32768字节；首次有效解码后输出栈最低余量。第三方解码源码、音源、重采样、板级I2S及既有AI功能未改。增加tests/test_radio_stack.py，读取真实已编译解码器机器码，将全部辅助函数栈帧相加再留8192字节余量，本机预算25200字节，小于32768；1项通过。台名资源2项检查通过，git diff --check通过（既有ASR头文件换行警告）。没有安装依赖、联网下载、修改全局配置或清理文件。

沿用第6节构建命令与原环境，编译日志tmp/easyinput/radio-stack-fix-build.log。本轮修复版尚未烧录，必须等用户确认；两台声音、按键响应、长时间播放和栈余量尚待现场核验。

修复版编译结束，退出码0，日志未检出warning/error；SHA256=3610737c43a405daac9d6ad9775bb1945fb3a288fcde9f8ddcd8fbd48d82b22b。烧录状态仍为未烧录，待用户确认后只更新应用区，保留配置。

## 9. 栈修复版烧录（2026-10-12）

用户确认烧录并验证。再次以 `python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-mac` 核验MAC c4:9e:7e:07:9f:d8；应用哈希与第8节一致。沿用已核验相同布局，仅以 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin` 更新应用，不写NVS或分区表。退出码0，radio-stack-fix-flash.log显示Hash of data verified。

采集命令 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/radio-stack-fix-validation.log`。初始心跳持续，setup=done，但wifi/ws/raw=down；本段未见panic，不能确认电台解码与声音恢复。已请用户开启原保存的2.4GHz网络，正常上电后测试S6播放、S7两台切换和S6停止。结果待反馈，不能将写入校验当作功能验收通过。

未安装依赖、未改全局配置、未删除文件。日志均保留在Workspace；设备联网后会访问既有电台CDN，尚未触发收费语音测试。

联网后用户反馈电台无反应。重新采集radio-no-response.log确认wifi/ws/raw=up、心跳持续，S6输入完整收到：140606→142266ms（1660ms），152666→155647ms（2981ms）。两次均超过button.c的SHORT_PRESS_MAX_MS=1000ms，因此不调用radio_toggle；这两次操作不能用于判断修复版播放失败。已请用户快速点按0.2～0.5秒后复测，尚未改动按键规则或烧录。电台栈余量、音乐声音恢复仍待首次实际启动后的日志验证。

## 10. 播放音质反馈与初步诊断（2026-10-12）

用户确认有声音但质量很差。radio-no-response.log确认两台均成功解码：默认44100Hz/64kbps、备选44100Hz/128kbps；解码后任务最低栈余量14796字节，设备心跳和按键响应持续，首轮栈问题已获得余量证据，但不能由此确认音质合格。

默认台播放220600→233819ms后重连1/4；271909→285157ms后重连1/4；288197→296342ms后重连2/4。备选台首连接311283ms出现esp-tls Failed to open new connection in specified timeout。现有HTTP客户端timeout_ms=1000，对连接与读取都偏紧，日志证明确有网络中断/超时现象，但缺少逐次HTTP读取错误原因和欠载统计，尚不能将持续杂音全部归因于网络。

同段记录旋钮曾将音量调至95%，后降至35%、25%及15%，高音量可能造成小喇叭失真，需与低音量和本地台名提示对照。默认64kbps压缩和单喇叭也限制音乐观感；未测实际频响，不将其当作确定根因。

已询问用户声音是间断、持续破音或单纯发闷、两台及台名对照；依据用户测试失败后配置调整需确认规则，提出仅将电台网络超时延长至10秒并补充断流/缓冲诊断后重新编译，待授权。未改源码、未烧录、未安装依赖或新增联网探测。

## 11. 状态恢复与播放诊断优化（2026-10-13）

用户请求优化当前未提交改动。本轮修改：

- AI尚未开始播放而电台暂停等待超时时，撤销暂停请求并停止自动录音所持有的麦克风。AI播放器停止超时时，保留电台暂停状态，语音任务继续等待播放器退出后交还喇叭；期间不接管新录音，避免两个输出任务重叠。
- 离线唤醒检测返回后、接管自动录音前再次检查电台状态，丢弃检测期间开启电台或进入选台造成的过期唤醒。
- 解码输入不足4096字节时，先非阻塞补充可用数据并尝试解码；只有缺少完整帧时才等待下载，避免新数据等待阻塞已有音频输出。
- 电台增加连接失败、HTTP状态、读取结果/errno、输出失败，以及结束时收到的音频字节数、压缩音频最低缓存量和空缓冲等待次数。空缓冲等待次数是缺帧时20ms等待未得到数据的次数，不等同于测量到的I2S DMA欠载次数；最低缓存量包含本地解码输入与下载缓冲。
- 实时ASR连接预算以共享常量表示，Qwen配置下录音PSRAM缓存由8秒增至20秒，覆盖15秒ASR握手及5秒调度/发送余量，多占384000字节PSRAM；其他配置保持8秒。该预算不保证额外Muse重连或持续上行拥塞时录音不积压，积压溢出仍报错且不提交缺失的半句。

新增tests/test_radio_lifecycle.py：所有权与缓冲的静态约束检查，以及模拟时钟下执行实际电台状态函数的C测试，覆盖暂停成功/超时、切台与确认不重置定时、选台超时取消、截止时间及重新开启。C测试需要主机编译器；临时产物仅保留在Workspace的tmp/easyinput。

实际执行 `python -B -m unittest tests.test_radio_lifecycle tests.test_radio_assets tests.test_radio_stack tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter tests.test_easyinput_buttons tests.test_easyinput_replay_seek`：Ran 15 tests，13项通过、2项测试及2组测试跳过。状态执行、按键与续播测试因主机编译器不可用跳过，栈测试因未激活工具链跳过；不能将静态检查当作这些运行测试通过。

本轮网络超时仍为1000ms，未以未经验证的延长读取等待替代诊断。尚未编译或烧录；沿用已有ESP-IDF环境编译需要用户确认工作区外工具链只读访问。未安装依赖、未联网、未改全局配置、未删除或清理文件。物理音质、停止恢复、慢连接与20秒缓冲的实机资格仍待验证。

### 11.1 本地编译验证

用户随后明确允许只读esp-idf-cy技能及既有工具链，并使用已有环境编译，不安装、不下载、不烧录。读取已有tmp/easyinput/env.ps1确认只修改本次进程环境；真实 `python "$env:IDF_PATH/tools/idf.py" --version` 返回ESP-IDF v6.0.1，编译器为xtensa-esp-elf-gcc 15.2.0，项目构建元数据确认esp32s3和tmp/easyinput/sdkconfig.radio。

实际编译：在esp32目录加载 `. ./tmp/easyinput/env.ps1`，设置本次进程 `$env:IDF_COMPONENT_MANAGER = '0'` 禁止组件管理器下载，执行 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake -D SDKCONFIG=tmp/easyinput/sdkconfig.radio build`，全部输出写入tmp/easyinput/radio-lifecycle-optimize-build.log，仅完成后检查结果。退出码0，日志无error/warning编译诊断。应用2297856字节（0x231000），4MiB分区剩余0x1cf000（45%）；SHA256为812a5d9f559097f313c11ea04ca4a153701810d76db262fb066f6260a5bfef06。

在已激活环境重跑上述测试命令：Ran 15 tests，14项通过、1项测试及2组测试跳过。真实解码器机器码栈检查通过，入口栈16608字节，保守需求25200字节，小于任务32768字节。状态执行测试与按键/续播两组仍缺主机C/C++编译器，未安装补齐，不能算通过。`git diff --check`通过，仅有既有ASR头文件换行格式提示。

本次仅修改工作区构建产物及记录，未读取或输出真实凭据、未访问远程域名、未修改全局配置、未安装依赖、未清理文件或操作串口。产物为build-easyinput-v2-wake/muse-gadget.bin，日志保留供后续核验；现场音质、暂停恢复、慢连接、PSRAM余量和长时间播放仍未验收。

### 11.2 优化版烧录与采样验证

用户明确请求“刷机验证”。刷新端口后只发现COM6（VID_303A/PID_1001），以 `python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-mac` 鲜读确认ESP32-S3、MAC c4:9e:7e:07:9f:d8。随后以同样连接参数 `read-flash 0x10000 0xc00 tmp/easyinput/partition-lifecycle-optimize-check.bin` 读取3072字节分区表，与build-easyinput-v2-wake/partition_table/partition-table.bin的SHA256一致。只读取分区布局，不读取NVS、凭据或模型内容。

烧录前再次通过serial.tools.list_ports确认COM6的USB serial为C4:9E:7E:07:9F:D8。实际执行 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin`，日志tmp/easyinput/radio-lifecycle-optimize-flash.log，退出码0并显示Hash of data verified。只写应用，不更新启动程序、分区表、OTA数据、模型或NVS。固件哈希与11.1一致。

本次由默认控制线完成下载及复位，用户未参与手动BOOT步骤。复位后重新核对USB serial，执行已审查的脱敏采集脚本 `python ./tmp/easyinput/capture-asr.py COM6 60 tmp/easyinput/radio-lifecycle-optimize-validation.log` 两次，各结束后检查结果，分别采集32行和18行。两次采样有间隔，开口前残留片段与新数据可能拼接，不视为完整启动日志或全时段连续监测；不再调用esptool扰动正常运行。

样本心跳确认setup=done、wifi/ws/raw=up，未见panic、下载模式提示或重启。S7及S6短按输入有记录；环境音乐台（station=1）成功解码44100Hz/128kbps/双声道，电台任务最低栈余量14828字节。播放前PSRAM约4447KiB，播放后约4384～4389KiB，后段未见持续下降；内部空闲约20KiB、最大块8KiB。该结果只代表本次短时采样，不证明长期稳定性或TLS峰值资格。

已请用户用约20%～35%音量比较两台并测试关闭；采样中实际音量曾为10%、0%、5%，有旋钮输入响应。尚未收到音质反馈，也未记录到播放期间切台、关闭和stream summary；不能宣称音质、缓冲等待次数、AI暂停恢复、慢连接或60分钟计时通过。

电脑端未安装或下载依赖、未改全局配置、未删除文件；设备正常联网使用既有服务及环境电台域名ice5.somafm.com，未主动进行收费ASR/TTS测试。分区副本、烧录日志和脱敏采样日志均保留在Workspace的tmp/easyinput，未清理。回滚仅需按相同应用布局刷入已核验旧版应用，仍需另行明确请求。

### 11.3 S1语音上传失败复现

用户反馈S1出现蓝灯后转为一颗红灯，随后按要求复现一次短句。仅核对COM6的USB serial并进行有界脱敏采集，没有复位或写入设备；日志为tmp/easyinput/voice-input-s1-error.log，先采集60秒未捕获操作，再采集180秒捕获失败，分别18行、69行。电脑端未安装、下载或修改全局配置。

日志1160898ms收到S1 down，1160954ms打开实时ASR；1162299ms报 `esp-aes: Failed to allocate memory`，继而TLS写入失败，1162304ms报 `QWEN ASR AUDIO SEND FAILED`。麦克风已采到19200样本（1.2秒）、peak=21486、clipped=0；录音因upstream_error结束，并在1162309ms恢复本地监听。同期wifi/ws/raw保持up，无panic。该次不是S1输入失效或麦克风未采音，而是上传加密路径的内存分配失败。

核对实际ESP-IDF 6.0.1源码：硬件AES的DMA路径会申请MALLOC_CAP_DMA缓冲/描述符；实际生成配置启用了CONFIG_MBEDTLS_HARDWARE_AES及AES中断，TLS主缓冲配置在PSRAM，不能据剩余PSRAM充足排除内部DMA内存不足。失败前内部空闲约30KiB、最大块13KiB；未取得失败瞬间内部内存及分配长度，不声称已量化具体缺口，也不将其归因于本轮20秒PSRAM录音缓存。

候选最小修复：只对EasyInput离线唤醒配置关闭硬件AES加速，保留TLS认证与加密，改用软件AES，避开该硬件DMA分配路径；不改第三方SDK或全局配置。CPU开销、同时联网、电台与实际S1录音必须在新固件上重新验证。依据用户测试失败后配置调整需先确认的规则，尚未修改配置或重新编译/烧录。

### 11.4 软件AES配置修复与离线构建阻塞

用户明确同意修复并编译，烧录单独确认。精准修改devices/sdkconfig.easyinput-v2-wake，增加CONFIG_MBEDTLS_HARDWARE_AES=n及中文原因注释；不修改第三方代码、TLS认证、全局配置或原版生成配置。旧固件保留在build-easyinput-v2-wake，SHA256仍为812a5d9f559097f313c11ea04ca4a153701810d76db262fb066f6260a5bfef06。

为防止新配置触发自动下载，在tmp/easyinput/offline-python/sitecustomize.py加入仅本次Python构建进程使用的外部网络阻断，通过本次进程PYTHONPATH加载。首次阻断误拦Windows asyncio的本机socketpair通信，修正为仅允许127.0.0.1、::1、localhost；本机socketpair及asyncio事件循环检查通过，外部连接继续阻断。未修改SDK文件或系统Python配置。

初次新目录build-easyinput-v2-soft-aes未显式传目标，构建系统选用了工程默认esp32c5，依赖解析被阻断，退出码2；保留失败目录。随后在build-easyinput-v2-soft-aes-s3显式使用IDF_TARGET=esp32s3及新配置tmp/easyinput/sdkconfig.soft-aes-s3，仍在配置阶段提示Direct dependencies have changed，尝试查询https://components-file.espressif.com/components/espressif/cjson.json，被外部网络阻断，退出码2。没有成功连接或下载依赖，也没有生成新应用。

已有生成配置会保留原硬件AES=y，因此通过官方kconfgen从同一组defaults生成新的配置，不直接编辑旧生成配置，不读取或输出凭据值。命令为 `python -m kconfgen --kconfig "$env:IDF_PATH/Kconfig" --config tmp/easyinput/sdkconfig.soft-aes-s3 --defaults sdkconfig.defaults --defaults devices/sdkconfig.easyinput-v2 --defaults tmp/easyinput/sdkconfig.token.local --defaults tmp/easyinput/sdkconfig.qwen.local --defaults devices/sdkconfig.easyinput-v2-wake --defaults devices/sdkconfig.easyinput-v2-radio --env-file build-easyinput-v2-wake/config.env --list-separator semicolon --output config tmp/easyinput/sdkconfig.soft-aes-s3 --output json build-easyinput-v2-soft-aes-s3/config/sdkconfig.json`。首次缺少list-separator参数导致rename路径列表被当作单一路径，修正参数后退出码0。

仅展示并核对非敏感配置项：IDF_TARGET=esp32s3、MBEDTLS_HARDWARE_AES=false、MBEDTLS_EXTERNAL_MEM_ALLOC=true、ESP_TLS_USING_MBEDTLS=true、HOMEHUB_QWEN_ASR/HOMEHUB_OFFLINE_WAKE/HOMEHUB_RADIO=true。再次执行 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，依赖核对仍要求外部仓库元数据，退出码2，未进入固件编译。软件AES配置生成通过不等于固件构建或实机语音恢复通过。

日志保留为qwen-software-aes-build.log、qwen-software-aes-build-final.log、qwen-software-aes-s3-build.log、qwen-software-aes-config.log、qwen-software-aes-config-final.log、qwen-software-aes-s3-build-final.log（均在tmp/easyinput）。本次未安装、下载依赖、删除文件、改变全局环境或烧录；工作区临时失败构建目录及配置保留，清理需要用户另行确认。继续构建需要用户确认新的依赖核对步骤；若允许联网，主要域名为components.espressif.com、components-file.espressif.com，用途仅限官方依赖元数据核对，组件包下载或版本升级需另行确认。

### 11.5 官方元数据核对与原版本依赖恢复

用户明确允许只读components.espressif.com、components-file.espressif.com的官方依赖元数据并继续编译，不允许组件包下载、依赖升级或烧录。新增tmp/easyinput/metadata-python/sitecustomize.py，仅通过本次构建进程PYTHONPATH加载：限制HTTP方法和官方元数据URL，禁止ComponentFetcher下载组件包，在写锁文件前校验版本和内容哈希；SDK和系统Python文件保持原样。

首次受限联网构建日志qwen-software-aes-metadata-build.log，退出码2。构建系统分阶段处理条件依赖：第一阶段把四个语音组件从锁文件暂时排除，同时自动清理了managed_components中的espressif__dl_fft、espressif__esp-dl、espressif__esp-sr、espressif__esp_new_jpeg。这是未预期的实际删除，不符合原计划。第二阶段重新要求这些组件，限制把它们当作新增组件并在写锁文件前停止；没有下载新的组件包。

已向用户说明，并从同一Workspace的tmp/easyinput/component-cache/service_d92d8f1e中恢复四个原版本目录：dl_fft 0.6.0、esp-dl 3.3.12、esp-sr 2.6.0、esp_new_jpeg 1.0.2。恢复前校验源、目标绝对路径都在Workspace内且目标不存在，然后用PowerShell Copy-Item复制，没有覆盖现存目录。通过官方validate_hash_eq_hashdir核验全部7个原组件的内容与首次构建前记录的哈希一致，并恢复缺失的.component_hash标记。没有从网络获取这些源文件。

限制修正为独立的dependency-baseline.json保存首次构建前的全部版本/哈希，不随分阶段锁文件变化；禁止组件管理器的自动清理。固定版本为cjson 1.7.19~2、dl_fft 0.6.0、esp-dl 3.3.12、esp-sr 2.6.0、esp_new_jpeg 1.0.2、esp_websocket_client 1.2.3、led_strip 3.0.3，IDF 6.0.1。

随后沿用同一defaults及显式esp32s3目标，在本次进程加载上述元数据限制，执行 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，输出保留于tmp/easyinput/qwen-software-aes-verified-build.log。待执行结束后记录退出码、配置、固件体积和实际AES实现；不把构建中的过程当作完成或实机恢复。

最终构建退出码0，应用2297856字节（0x231000），4MiB应用分区剩余0x1cf000（45%）。固件build-easyinput-v2-soft-aes-s3/muse-gadget.bin，SHA256为d8f1813994e36c386d01663d98c8124a0f5c06c14f20d2dc87d89fd521138dba。新旧分区表SHA256均为6de8a8dec41d229793269e5fe13016bd2f8efe0d8f95ee6d347dd66795fc50e3，没有分区变更。日志有两条配置阶段CMake警告（缺少Kconfig选项、要求重新运行配置）；构建系统随后完成配置并成功生成固件，不报告为零警告。

实际生成头文件确认esp32s3、TLS、Qwen ASR、离线唤醒和电台均开启，CONFIG_MBEDTLS_HARDWARE_AES未定义。通过nm及链接map核验mbedtls_aes_crypt_ecb来自tf-psa-crypto/drivers/builtin/libmbed-builtin.a(aes.c.obj)，为软件AES实现。ELF仍含esp_aes_process_dma，链接map显示硬件AES由ESP-SR的dl_sr_loader.cpp.obj引用，用于模型加载，不能以该符号仍存在判定TLS软件AES未生效。未修改ESP-SR模型加载逻辑。

再次使用官方validate_hash_eq_hashdir核验全部7个组件内容，并比较最终dependencies.lock与独立版本基线，版本及component_hash全部一致，没有升级或下载组件包。metadata阶段允许更新锁文件中的条件依赖元数据；首次自动清理造成的四个缺失目录已经从Workspace缓存恢复，所有受影响内容经哈希校验复原。

在已有环境运行 `python -B -m unittest tests.test_radio_lifecycle tests.test_radio_assets tests.test_radio_stack tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter tests.test_easyinput_buttons tests.test_easyinput_replay_seek`：14项通过、1项测试及2组测试因主机编译器不可用跳过；栈预算检查通过。git diff --check通过，仅有换行格式提示。新版本未烧录、未进行新固件ASR收费测试；关闭硬件TLS AES是否解决现场上传失败，仍需经用户单独确认烧录后复测。

本轮电脑访问仅限components.espressif.com、components-file.espressif.com的官方依赖元数据；未安装依赖、未下载组件包、未改变依赖版本、未修改全局或SDK配置。曾发生构建器自动清理，已按上文恢复并阻止重复发生；未再主动删除或清理文件。临时脚本、失败目录、新配置和所有日志保留在Workspace，清理需另行确认。

### 11.6 软件AES版烧录与一次短句反馈

用户明确同意刷入同一块EasyInput、只更新应用，并复测一次使用既有云端语音服务的S1短句。刷新端口为COM6，`python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-mac` 确认ESP32-S3和MAC c4:9e:7e:07:9f:d8；同参数 `read-flash 0x10000 0xc00 tmp/easyinput/partition-software-aes-check.bin` 的分区副本与新构建分区表SHA256一致。

烧录前通过serial.tools.list_ports再次确认USB serial匹配同一MAC。实际执行 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，退出码0，日志tmp/easyinput/qwen-software-aes-flash.log显示写入2297856字节、Hash of data verified。固件哈希d8f1813994e36c386d01663d98c8124a0f5c06c14f20d2dc87d89fd521138dba，未写NVS、启动程序、OTA数据、分区表或模型区，旧版应用仍保留可回退。

默认控制线复位后重新确认USB serial，执行 `python ./tmp/easyinput/capture-asr.py COM6 120 tmp/easyinput/qwen-software-aes-validation.log`。初段运行心跳107～112秒，setup=done但wifi/ws/raw=down；采集随后因USB ClearCommError中断，不能把断串口本身当作panic证据。已告知用户网络未连接，暂不重复语音操作；没有再次用esptool复位或识别。

重新核对USB serial后，执行同一采集脚本60秒。后段运行时间70～126秒，wifi/ws/raw均up，内部空闲约30KiB、最大块13KiB、PSRAM约4415KiB，18行采样心跳正常。两段运行时间重新起算，表明中间发生重新启动；未捕获其原因或用户物理操作，不报告全程没有重启。后段样本未见panic或AES分配错误，但未覆盖此前短句的完整执行过程。

用户明确反馈“测试成功”，并确认原2.4GHz网络已开启。因此S1一次短句功能恢复有用户现场确认；日志证据只确认后续联网及短时运行，未捕获该次转写、云端回复或音频播放事件，不升级为完整日志链路验收。没有要求追加第二次收费语音测试。长录音、播放电台期间提问、重连、长时间运行及CPU开销仍未验证。

本次未安装或下载依赖、未修改全局配置、未删除文件，电脑端只执行已有串口工具；设备测试按用户授权使用原有云端语音服务。分区副本、刷机日志和脱敏采样日志均在Workspace的tmp/easyinput保留；git diff --check通过，仅有换行格式提示。

### 11.7 蓝牙不可发现与Wi-Fi启动连接诊断（2026-10-14）

用户反馈蓝牙搜索不到、原Wi-Fi连不上，并确认名称和密码未变。COM6 USB serial仍匹配此前同一块开发板。只读代码确认setup_complete时不启动配对BLE广播，S8短按忽略；S8长按为清除配对及Wi-Fi配置的重置。本次没有执行配对重置、读取NVS或写入固件。

15秒和20秒脱敏采样分别写入tmp/easyinput/ble-search-diagnostic-20261014.log、wifi-connect-diagnostic-20261014.log，持续显示setup=done、wifi/ws/raw=down、ble=off。代码中首次连接失败后的s_keep_connected为false，持续自动重连只在成功取得IP后才启用；当前主循环不会持续重新搜索已保存网络。因此确认存在启动失败后的恢复缺口，但单靠心跳不能判断认证或接入点根因。

新增Workspace临时诊断脚本tmp/easyinput/capture-wifi-reboot.py：按USB serial关联同一设备，串口重新出现后继续有界采样，不读取或写入Flash；只记录经过过滤的状态、扫描统计及失败码，连接网络名称先替换成隐藏标记。先进行120秒被动采集，未捕获重启。随后通过已审查的esptool HardReset控制线原语正常复位一次，用户也报告已关机开机；60秒窗口捕获两次启动标记。90秒追加被动采样仍显示离线。为取得扫描及连接失败详情，又执行两次各一次正常复位的有界采集（90秒、75秒），未主动进入ROM会话或擦除配置。相关日志为wifi-reboot-diagnostic-20261014.log、wifi-reset-diagnostic-20261014.log、wifi-scan-diagnostic-20261014.log、wifi-join-diagnostic-20261014.log。

最终75秒样本：保存的信道11定向扫描0 AP；全信道扫描发现23 AP（去重8个网络），2874ms开始连接已保存网络；3990ms断开reason=2、rssi=-41并重试；5565ms断开reason=201、rssi=-128并重试；61065ms本轮连接失败，后续仍离线。ESP-IDF 6.0.1头文件定义2=Authentication expired、201=No AP found。不能据此宣称密码错误、硬件损坏或确定热点频段；信道缓存过时不等于全信道扫描后的连接失败原因。

已向用户说明认证超时及随后找不到目标接入点的现象，并询问当前网络是iPhone热点、安卓热点或路由器，以确定下一步。未修改业务代码、未编译/烧录、未清除配对、未安装依赖或新增电脑外部联网。所有诊断脚本及日志保留在Workspace，Wi-Fi连接尚未恢复，根因与修复仍待进一步验证。

### 11.8 Windows热点与云端连接复查（2026-10-14）

用户确认使用当前电脑的共享热点，并自行重开为2.4GHz。通过Windows Runtime的NetworkOperatorTetheringManager只读查询热点运行状态及客户端数量，通过Get-NetAdapter、Get-PnpDevice、serial.tools.list_ports检查接口，不读取热点名称、密码或配置凭据，不修改系统网络设置。初次热点On、ClientCount=0，随后用户重启期间串口消失，诊断脚本未找到设备，因此该轮并未实际执行控制线复位。

用户再次反馈无法联网时，COM6已恢复并匹配同一设备，热点On、ClientCount=1。执行capture-wifi-reboot.py 18秒被动采集到wifi=up、ws/raw=down；扩展临时脚本的严格日志允许列表，仅保留云端阶段、HTTP状态及错误码，服务地址和身份隐藏，不记录凭据。45秒被动采集未取得云端错误，继而执行75秒reset采集：一次正常控制线复位，保存信道6扫描发现1 AP，2549ms获取IP。未擦写Flash、清除设置或使用ROM识别操作。

追加50秒采集cloud-api-recurrence-20261014.log：95～120秒wifi/ws/raw均up；123319ms云端rx error并重连，130586ms DNS解析失败，TLS connect failed=-1；134348ms TLS连接成功，136073ms控制连接成功，141115ms数据通道恢复。全过程Wi-Fi保持up。后续25秒cloud-recovery-check-20261014.log中175～195秒三个连接均up，未捕获新的中断。结论：本轮已恢复联网，故障样本包含云端接收中断与一次DNS解析失败，不能据此确定热点DNS、上游网络或服务端是根因，也不能宣称长期稳定或语音功能已复测。

实际串口命令均在esp32下加载tmp/easyinput/env.ps1后执行：`python ./tmp/easyinput/capture-wifi-reboot.py <秒数> tmp/easyinput/<日志名> [reset]`。相关日志为wifi-recurrence、cloud-connection-recurrence、cloud-startup-recurrence、cloud-api-recurrence、cloud-recovery-check，均带20261014后缀。本轮仅修改Workspace临时诊断脚本及本记录，没有修改固件、安装依赖、编译、烧录或主动发起电脑外部网络请求；设备仍使用既有云端连接。保留诊断日志供后续复现，启动失败后不持续重试的问题仍未修复。

### 11.9 热点通过VPN上网的现场恢复（2026-10-17）

用户要求保留v2rayN TUN，让热点客户端通过电脑VPN上网。手机连接同一热点但不能打开网页，说明除板子连接失败外，热点上网共享也存在异常。截图确认singbox_tun共享至本地连接* 10；只读状态确认sing-box进程、TUN地址172.18.0.1/30及DNS 172.18.0.2，热点地址192.168.137.1/24。没有读取订阅或节点配置，截图中的敏感信息不写入本记录。

依据sing-box官方TUN说明，Windows严格路由会限制非TUN接口的DNS流量。经用户同意，由用户保持TUN、自动路由及共享设置不变，仅关闭严格路由并重启v2rayN服务。用户随后确认普通及需要VPN的网页均可打开，并确认板子恢复正常。该单项对照支持严格路由与本机热点共享存在兼容问题，不证明所有DNS或流量均经VPN；关闭严格路由会放宽DNS防泄漏限制。

本次没有修改电脑网络设置、板子固件或配网信息；电脑设置由用户按确认方案调整。板子首次联网失败后停止重试的固件缺口仍未修复。提交前复跑电台、分区、音频过滤、按键与重播相关测试：unittest汇总Ran 15 tests，OK (skipped=3)，跳过依赖缺失的主机编译测试。本轮没有重新编译或烧录，构建与实机验证边界沿用11.5～11.6及本节用户反馈。

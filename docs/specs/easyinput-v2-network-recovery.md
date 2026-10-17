# Spec：开机联网恢复与离线状态提示

日期：2026-10-17。用户授权实现并编译；烧录单独确认。本规格只针对当前EasyInput无屏、语音开启且非显示Muse构建，不更改其他板型的重连策略。

## 目标与边界

解决首次连接失败后长期离线；开机热点晚开和使用中掉线均能重试。联网分Wi-Fi、服务及缺失配对凭据，不从单一断开原因猜测密码错误。不新加自动重置、擦除配置、系统网络修改、云端提示音或依赖包。

## 恢复策略

应用主循环在已有操作锁下串行执行Wi-Fi恢复及未启动的服务连接。该板关闭驱动层并行掉线重连，避免与全信道扫描争用。已有网络选择及60秒连接预算保留，扫描可尝试已保存网络；每轮结束后按5、10、20、30、60秒级别退避，持续失败后60秒一次，主循环5秒节拍会带来调度延迟。开机原有首轮结束后5秒启动第一次恢复，后续失败间隔10、20、30、60秒；成功重置退避。

Wi-Fi已连接时不重启Wi-Fi；未启动服务任务时以独立退避重新获取/连接服务，已经运行的服务沿用其自身重连及授权刷新路径，不重建并发任务。S5短按查询并请求重试；已有连接尝试正在执行时仅查询，不再积累一次额外重试。正在运行的服务重连由原任务接管，S5不强制重启它。

## 灯光与声音

- 联网监测不增加任务，复用旋钮任务每100ms更新。故障持续5秒后柔和蓝色呼吸，15秒后暖黄呼吸，60秒后暖黄间歇；首次前5秒沿用原连接灯效。Muse控制和启用的数据通道均就绪后独立绿灯亮。
- 每次连续故障15秒轻柔双音一次，60秒按当前状态播报一次。状态从Wi-Fi失败转服务未就绪不重置故障计时；不重复播报。短暂掉线恢复不出声，故障超过15秒后恢复轻响一次，快速正常开机不播报成功。
- S5可主动查询“未连接无线网络”“服务暂不可用”“需要连接设备”或“连接正常”。缺失配对配置才提示连接设备，不猜测未知授权故障。
- 中文提示使用Windows已有声音生成并嵌入，断网可用。初版提示样本幅度限制为原幅度1/5，2026-10-17依据听感反馈改为1/2，再经过原用户音量/静音链路；不擅改持久音量。提示带短包络，不触碰GPIO8共享电源策略。
- 提示和AI使用同一播放器。仅空闲时取通知，先停止本地唤醒，再占用电台播放锁；S1、S2、S4、S3及新电台操作可打断提示，结束后恢复监听条件。静音、暂停缓存、对讲或电台期间不自动出声；忙碌期提示不排队，故障期间若发生这些交互，该连续故障的剩余自动声音也抑制，S5仍可在空闲未静音时主动查询。睡前电台结束后不会补播旧故障。
- 通知有效期1秒，状态改变即丢弃；讲话期间过期的提示不在回复结束后突然出现。五灯优先保留电台灭灯、静音红灯、语音录音/错误/音量及亮度交互；网络提示仅覆盖空闲网络故障。

## 验收

1. 热点关闭开机：15秒一次短音、60秒一次原因提示；其后不重复；1分钟后开启热点，无按键自动恢复Muse，配置不丢失。
2. 热点在线而服务不可达：仅服务恢复，不重启Wi-Fi；灯与S5播报服务异常，不误称密码错误。
3. 使用中掉线小于5秒不改变网络故障灯效，小于15秒恢复不出声；长时间掉线按退避恢复。
4. 静音、录音、回复、重播、电台/选台期间不抢占声音；S1打断提示仍能开始一轮正常对讲，暂停缓存不丢失；电台停止后不补播旧提醒。
5. 连续按S5不启动并行连接；连接尝试中只查询；配网流程及S8确认/重置保持原逻辑，S6/S7和旋钮不回归。
6. 记录内部RAM/PSRAM、任务栈及长期重试状态，无崩溃和持续泄漏。静态测试、构建、烧录及实体结果分别记录，未完成不标通过。

## 执行记录

已生成四段本地WAV/PCM，未调用收费服务。新增network_feedback.c/.h及纯策略头；接入app、button、voice、LED及构建清单。暂未烧录。后续记录真实编译及测试结果。

首次相关检查运行18项，旧电台所有权测试以全文查找`if (!radio_pause(true))`，误匹配新增通知分支，报1项失败；将该测试限定为原voice_task后重跑，14项通过、4项跳过。跳过为网络策略实际执行、电台状态执行及按键/续播测试（缺少主机C/C++编译器）；不安装工具补齐，也不将静态检查当作策略时序运行通过。资源检查逐字节确认四段WAV与16kHz单声道PCM一致，每段不足8秒。

在esp32加载`. ./tmp/easyinput/env.ps1`，本次进程设置完整原defaults、`IDF_COMPONENT_MANAGER=0`，执行`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，日志tmp/easyinput/network-recovery-build.log。退出码2，在配置阶段报已有espressif__esp_websocket_client无法解析；没有进入源码编译或生成新应用，不把旧目录内固件当作本轮产物。未下载、安装、删除组件或改全局环境。

已申请沿用此前审查的metadata-python/sitecustomize.py：只读components.espressif.com、components-file.espressif.com的官方元数据，禁止组件包下载、依赖版本变更与自动清理。按用户构建失败后调整配置需先确认规则，等待确认后继续编译；烧录仍需另行确认。四段提示由本机Microsoft Huihui Desktop生成，WAV/PCM共保留在main/network_assets；未读取或输出任何真实凭据。

用户随后明确允许按原限制继续编译。本次进程设置`IDF_COMPONENT_MANAGER=1`及`PYTHONPATH=tmp/easyinput/metadata-python`，保持同一目标、defaults和软件AES配置，执行相同build命令，日志改为tmp/easyinput/network-recovery-verified-build.log；不实时读取编译进度，仅完成后检查结果。

独立只读核验确认原有组件目录齐全，并使用官方`idf_component_tools.hash_tools.validate.validate_hash_eq_hashdir`对照metadata-python/dependency-baseline.json核验7个第三方组件源码，全部通过。定位该函数时两次导入路径探测失败，随后修正为实际validate模块，没有安装或修改环境。组件源码未变化，不等于本轮源码编译或功能验收已经通过；构建结果仍待最终记录。

### 最终构建与交付

受限构建network-recovery-verified-build.log退出码0；有两条配置阶段CMake警告（其中一条提示Missing kconfig option并要求重新配置）及一条本次板型切换导致的schedule_reconnect未使用警告。仅用与该板型相同的编译条件隔离旧驱动重连函数，未删除其他板型实现；不更改依赖或配置方案。随后沿用同一命令、环境和限制脚本执行增量构建，日志tmp/easyinput/network-recovery-final-build.log，退出码0，无编译器warning/error。

最终应用`esp32/build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，2756608字节，约2.63MiB；4MiB应用槽位剩余1437696字节，约34%。SHA256=`cf95ef738372fc9369bb9d1c1080b2f9cc70dae0245fe748b9778f0eb9fdce30`。生成配置仅选取非敏感项核对：目标esp32s3，TLS硬件AES=false；不读取/展示凭据。nm确认network_feedback_poll、play_network_notice及四段PCM嵌入符号；编译后的通知函数栈帧704字节、voice_task栈帧288字节，这不是完整调用链或运行栈余量验收。

最终重跑18项相关检查，14项通过、4项跳过（原因同上）。电台机器码栈预算仍通过；git diff --check通过，仅有旧测试文件换行格式提示。最终dependencies.lock的依赖名称、版本和component_hash均与8项基线（含IDF）一致，7个第三方源码内容校验已通过。

本轮新增network_feedback.c/.h、network_feedback_policy.h、network_assets四段WAV/PCM及README、test_network_feedback.py和本规格；修改app.c/.h、wifi_mgr.c、button.c、voice.c、led_status.c和main/CMakeLists.txt，旧电台测试只收紧匹配范围；更新flow进展/决策/踩坑指针。没有提交、推送、烧录、清理文件、安装依赖或修改全局配置。模型及已有构建辅助脚本沿用此前审查的原流程。允许的联网域名仅为components.espressif.com、components-file.espressif.com的元数据服务；没有HTTP逐请求记录，不把可能的缓存命中说成确定外部访问，组件包下载仍被限制脚本禁止。未调用收费ASR/TTS。

临时编译日志及失败配置产物保留在Workspace的esp32/tmp/easyinput与既有构建目录，不主动清理。当前设备仍是此前烧录版本，不能把新代码描述为已经在板上运行。下一步经用户明确确认后，重新核验目标身份和分区布局，只更新应用，再执行本规格六项实机验收；重点捕获热点延迟开启后的恢复及提示期间S1打断，不以写入校验代替功能通过。

### 烧录及初始运行观察（2026-10-17）

用户明确要求“烧录验证”。在esp32加载既有env.ps1，以`python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-mac`重新确认ESP32-S3、MAC c4:9e:7e:07:9f:d8。再次核验应用SHA256与上述最终值一致；`read-flash 0x10000 0xc00 tmp/easyinput/partition-network-recovery-check.bin`取得分区表，逐字节与本轮构建分区表一致；serial.tools.list_ports确认COM6的USB serial为同一MAC。

实际烧录命令：`python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin`。退出码0，tmp/easyinput/network-recovery-flash.log显示写入2756608字节及Hash of data verified。仅写应用区，未写NVS、分区表、模型、启动程序或OTA数据。

自动复位后再次核对USB serial，通过已审查脚本采集：`python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/network-recovery-validation.log`。初始心跳15～20秒显示setup=advertising、wifi/ws/raw=down、ble=advertising，内部空闲约54KiB、最大块31KiB、PSRAM约6034KiB；采样未见panic或ROM等待，但没有完整启动日志。当前是等待配对状态，不能按“已配网、暂时离线”评估自动重试；尚不清楚进入该状态的原因，不读取NVS凭据或据此声称烧录擦除了配置。

已请用户先短按S5验证离线配对提示，再用Muse App添加设备、短按S8确认并配网；不要长按S8或按BOOT。S5声音、联网成功、故障提示次数、自动恢复及对讲/电台避让均待现场反馈。没有主动调用收费ASR/TTS、重新编译、安装依赖或清理文件；串口日志和分区副本留在Workspace。

同一采样随后捕获多次S5短按，每次`local network notice=4`，喇叭输出206880帧（48kHz约4.31秒），与pair资源时长一致；旋钮音量有变化，心跳持续，样本内部空闲及PSRAM未见持续下降。该证据确认本地提示流程产生输出，不等于用户已听清，也不证明Wi-Fi恢复。设备仍advertising，等待配网与现场反馈；重复S5为用户主动查询，不计作自动提醒重复。

用户随后对“S5提示是否有声音、配网后绿灯是否常亮”回复“工作正常”。同一采样中setup依次经过confirm、confirmed、vm、done；170251ms网络状态变为4（ready），170～200秒心跳wifi/ws/raw均up、ble=off，内部空闲后段约48KiB、最大块25KiB、PSRAM约4401KiB。本轮S5离线提示及重新配网恢复有用户反馈和相应日志；尚未覆盖开机已保存网络但热点不可用的恢复场景，不将首次配网成功等同于自动重连验收。

为后续重新上电测试，审查并复用tmp/easyinput/capture-wifi-reboot.py（默认不复位，按USB serial重新关联同一设备）；仅扩充严格日志允许列表以收录网络状态、通知编号、喇叭输出计数和network wifi recovered。以AST仅加载过滤函数及其正则，执行6项检查通过，包含状态/计数允许、凭据样式拒绝及网络名隐藏。未运行整个脚本来执行测试，不进行系统网络修改或读取真实凭据。等待上一个有界采集窗口结束后再开启下一窗口，避免占用同一串口。

首窗口正常结束，采集440行。随后执行`python ./tmp/easyinput/capture-wifi-reboot.py 360 tmp/easyinput/network-cold-recovery-validation.log`，不传reset参数，6分钟有界监听同一USB serial。请用户关闭板子热点、正常重新上电，热点关闭75秒观察15/60秒提示，再开启原热点等待自动恢复；不按S1、S5、S8或说唤醒词，避免手动重试干扰与收费语音调用。结果待反馈及日志核验，不提前标为通过。

### 热点晚开自动恢复：通过

用户反馈“已自动恢复”。采集先记录运行中热点断开及网络状态1，随后USB断开/重新连接、运行时间重新起算，符合用户重新上电操作。重新上电仍setup=done、保存信道可用，未重新配对。初轮定向/全信道扫描未见已保存网络，之后约8、20、42、73秒自动发起新一轮扫描，不依赖手动按键。

本轮上电后16476ms通知1（延迟短音），61508ms通知2（Wi-Fi未连语音），各一次；输出分别24000及191760帧。79038ms取得IP，79039ms记录network wifi recovered；79126ms状态转为2（服务连接），89700ms状态4（ready），89718ms通知6（恢复短音）一次。90～115秒心跳wifi/ws/raw均up、ble=off，后段内部空闲约34KiB、最大块17KiB、PSRAM约4414KiB，未见持续下降或新的panic记录。

该段重新上电后未记录S5/S8或其它按键，自动Wi-Fi与Muse恢复有日志及用户反馈支持。热点实际开启时刻没有独立时间戳，不能据上述上电时间计算精确的“热点开启到恢复”耗时；提示输出日志不替代用户对音量、音色及灯效的单独反馈。此结果覆盖首个验收场景，不扩展为服务独立故障、长期运行或所有音频避让场景通过。

已请求保持网络在线，S2静音后S5应无声，解除静音后S5应播报连接正常。该项待用户反馈及日志核验；不关闭热点、不操作配对、不进行收费语音测试。

### 静音与主动查询：通过

用户明确反馈“静音无声，解除后播报正常”。同一日志173529ms记录S2按下，178969～179149ms记录S5短按，此后至下一次S2解除前没有对应网络通知或喇叭开始记录。186469ms再次S2，189249～189389ms再次S5，189442ms通知5（连接正常），189461ms喇叭开始、191880ms输出结束，110400帧对应2.3秒本地ready提示，muted=0。后续用户主动重复S5亦有相同提示输出，不视为自动重复播报。

同期wifi/ws/raw持续up，内部空闲约34～35KiB、最大块17KiB，PSRAM约4414KiB。此项验证S2静音对S5本地提示的抑制及解除后的主动查询，不等同于静音期间自动故障提醒、对讲/电台避让或长时间稳定性全部通过。

阶段结论：离线S5提示、重新配网、热点关闭开机后晚开热点自动恢复，以及在线S2静音/S5查询通过；仍待服务独立故障恢复、自动提醒在静音/电台期间的抑制、S1打断提示、重复S5与连接中查询、长期内存和运行资格。此次仅读取已脱敏日志、追加验收记录，未修改固件源码、重新编译/烧录、重置配对、调用收费语音或修改电脑网络设置。日志有界采集按原360秒期限运行，不宣称其已提前停止。

### 电台与主动提示共存：用户现场通过

用户按上一轮三步测试反馈“测试通过”：S6开启电台后按S5，提示不打断音乐；S7切台及旋钮音量正常；S6关闭后按S5恢复状态播报。按该操作范围记录为用户现场验收通过，不扩展为原有电台音质、60分钟定时、自动故障提示抑制或长期运行均已合格。

回读现有network-cold-recovery-validation.log，末段仍为此前S5查询和正常联网心跳，未捕获此次S6/S7操作及电台播放段，因此本项依据为用户反馈，没有完整对应日志链路。未为补日志强制重测或额外操作设备。本次仅追加文档，没有修改源码、重新编译/烧录、提交或调用收费语音。

当前完成的联网功能首轮验收：S5离线及在线播报、静音抑制/解除、重新配网、热点晚开自动恢复、电台期间主动提示避让及既有切台/音量交互。剩余专项：服务独立故障恢复、自动故障提示在静音/电台中的抑制、S1打断提示、连接中查询与长期稳定性。代码仍未提交，继续以本规格最终固件哈希为硬件版本依据。

### 中文提示音量校准（2026-10-17）

联网优化及交接记录已提交并推送用户Fork的codex/easyinput-v2-port，提交d9c0b88；随后按用户同意继续的建议进行电台音质对照。新有界采集radio-quality-review-20261017.log捕获S5通知5，主音量45%、未静音，完整输出110400帧，峰值1079。用户反馈“提示音量小”，不是对两台音乐表现的明确反馈，电台音质问题仍不据此判定。

本轮仅将中文资源额外衰减从1/5改为1/2（数字幅度2.5倍），保留短音1200幅度、包络、音频仲裁、旋钮音量和静音行为；不改默认或保存音量、不重新生成资源、不改电台网络超时或解码。不把数字幅度变化等同于主观响度同倍增长。待编译、用户确认烧录及实体听感验证后记录结论；当前设备仍运行cf95ef固件。

使用相同ESP-IDF6.0.1、软件AES配置及metadata-python限制，执行原build命令，日志tmp/easyinput/network-prompt-level-build.log，退出码0且无编译器warning/error；未增加安装、组件包下载或版本变更。最终应用2756608字节，SHA256=`f4e7d537b6c7522a7ab38349bae2a86999965e73412435b6521545d3079da306`。该目录当前产物已为音量调整版，设备仍为上一版cf95ef。

18项相关检查14项通过、4项因主机编译器不可用跳过；git diff --check通过，只有换行格式提示。离线核对四段PCM原峰值后，1/2幅度均无数字溢出；ready提示在主音量45%下预计峰值2699，对照现场原峰值1079。此为数字预测，不是实体响度验收。资源未改变、未调用云端TTS，本轮仅改源码一处常量并同步规格、资源说明及进度。尚未提交或烧录，等待用户确认后试音。

### 音量调整版烧录与试音准备

用户明确确认烧录并试音。先以USB serial核验COM6匹配c4:9e:7e:07:9f:d8、应用哈希为f4e7d上述最终值；首次read-flash返回串口忙（退出2），未打开串口或写Flash。原因是本会话radio-quality-review采集仍运行，通过其会话发送Ctrl+C结束，退出1为主动终止，不是设备崩溃。未删除日志、修改串口配置或安装工具。

再次枚举同一USB serial后执行`python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset read-flash 0x10000 0xc00 tmp/easyinput/partition-prompt-level-check.bin`，鲜读芯片为ESP32-S3、MAC同上，分区表逐字节与本轮构建一致。随后执行原应用写入命令，目标0x20000、波特率460800、flash-mode dio、flash-freq 80m、flash-size keep，产物build-easyinput-v2-soft-aes-s3/muse-gadget.bin；日志tmp/easyinput/network-prompt-level-flash.log。退出码0，写入2756608字节并Hash of data verified；仅更新应用，未写NVS、分区表、模型、OTA数据或启动程序。

复位后执行`python ./tmp/easyinput/capture-wifi-reboot.py 240 tmp/easyinput/network-prompt-level-validation.log`，按USB serial监听同一设备，不主动复位。16～26秒心跳setup=done、wifi/ws/raw=up、ble=off，主音量45%、未静音，证明现有配对网络及音量被保留。捕获恢复短音通知6，输出24000帧、峰值540；短音仍采用旧1200幅度，不能拿该短音峰值判定中文提示增益是否生效。内部空闲约33KiB、最大块17KiB，PSRAM后段约4415KiB；样本无panic记录，不代表长期资格。

已请求保持45%原音量短按S5判断中文提示清晰度，再S2静音/S5无声、最后解除静音。中文提示新峰值与实体听感待反馈；不按S1或说唤醒词，不主动发起收费ASR/TTS。本次未重新编译或改源码，音量修正尚未提交。

随后同一日志61340ms通知5，主音量45%，63777ms完整输出110400帧，峰值2699，符合此前数字预测；与旧版相同45%下1079的结果相比，中文提示增益已在实机输出链路生效。后续用户旋钮调到100%，重复S5输出峰值5998；不能将不同音量下的结果直接当作同条件对照。主观音量是否合适以及本版静音回归仍待用户反馈，不因数字峰值符合就标为听感验收通过。

### 提示音量校准：用户验收通过

用户明确反馈“音量合适，静音也正常”。结合45%同条件下1079→2699的输出对照，本轮中文提示增益和实体听感通过；静音回归记为用户现场反馈通过。当前采样未捕获对应S2或muted=1记录，不伪称取得完整静音日志链路，也不要求仅为补日志重复测试。

后段106～131秒心跳wifi/ws/raw持续up、ble=off，内部空闲约32KiB、最大块17KiB、PSRAM约4415KiB；期间多次用户主动S5查询完整输出。没有据此宣称长时间稳定性或电台音质全部通过。当前板上固件为f4e7d537b6c7522a7ab38349bae2a86999965e73412435b6521545d3079da306；联网优化代码已推送d9c0b88，本次音量源码及记录仍未提交。

本次仅回读脱敏日志、更新验收和进度，没有改源码、重新编译、烧录、调用收费服务或改系统设置。电台两台的音乐音质对照仍缺明确反馈；S1打断提示、独立服务故障与长期稳定性仍为专项待测。
# 2026-10-17 增补：当前热点的手动DNS

适用背景：Windows热点192.168.137.1对Qwen域名的原始UDP A/AAAA查询返回SERVFAIL，上游路由器192.168.1.1同类查询成功；电脑本地使用相同Key的Qwen-TTS短句合成及下载均HTTP200、音频正常。管理员设置Wi-Fi DNS命令未报错，但立即和5秒后读回均为原值，未证明系统DNS改变。用户批准改为板端手动DNS，先编译、另行确认烧录。

- EasyInput新增`HOMEHUB_WIFI_DNS_IPV4`字符串选项，默认空值沿用DHCP。仅当前Workspace本地构建defaults文件`esp32/tmp/easyinput/sdkconfig.dns.local`启用192.168.1.1，不写入通用板型默认配置或NVS。
- 每次收到DHCP取IP事件后、宣布连接就绪前，通过ESP-IDF官方接口设置主DNS；IP、网关与配网流程继续由DHCP管理。无新增备用服务器、自动切换或收费调用重试。
- 非法或零地址、设置失败时记录错误并报告连接失败，不静默宣布手动DNS生效。其他板型与空值配置原逻辑不变。
- 当前地址仅适用于该路由器可达的网络。换网或恢复自动模式时，将选项清空并重新编译烧录；不宣称该地址通用于所有热点。
- 验收：构建中确认配置值及代码进入固件；烧录另确认后检查`manual DNS applied`、DHCP地址和连接恢复，再按获准范围分别验收ASR及TTS。编译成功不能替代板上DNS可达性验证。

本轮编译已成功：配置值192.168.1.1与日志字符串均进入应用，8项依赖锁及7项第三方源码哈希一致；22项检查17通过、5跳过。应用2756608字节、分区余34%，SHA256=`c2038ce659412d14933d3619e09711bdef9fb277d2b77855438cf398b30a7964`，构建日志`esp32/tmp/easyinput/manual-dns-reconfigured-build.log`。尚未烧录或板上验收。同IP DHCP续租可能不触发GOT_IP，长时续租后手动DNS是否保持仍待专项验证。

用户随后确认烧录，仅更新原设备应用并完成写入校验。正常复位后的`esp32/tmp/easyinput/manual-dns-startup-complete.log`记录DHCP地址192.168.137.148、`manual DNS applied: 192.168.1.1`，VM API HTTP200，12秒就绪、13～28秒wifi/ws/raw均up；本次样本无DNS/401错误。证明板端设置已生效、联网恢复，尚未验证Qwen端点与ASR/TTS，不进行未获准收费测试。

随后用户另行批准一次最短唤醒问答，反馈文字及“你好”朗读均正常。`esp32/tmp/easyinput/manual-dns-voice-acceptance.log`确认合成HTTP200、音频下载HTTP200及34604字节完成、扬声器完整输出、回复缓存23040字节，无DNS错误；本轮语音链路恢复，板端手动DNS方案在当前网络通过短句验证。Windows热点DNS故障未由此修复，换网/长期续租资格及完整故事仍待专项验收，不扩大通过范围。

用户随后要求恢复自动DNS：已将本地defaults与当前构建配置中的`CONFIG_HOMEHUB_WIFI_DNS_IPV4`设为空，保留可选功能源码；自动模式不调用手动DNS设置，继续采用DHCP提供的服务器。重新编译及烧录状态以最新进度记录为准，烧录仍需另行确认；如果仍连接原异常Windows热点，语音解析故障可能再次出现，不自动回退到固定DNS。

自动DNS版已编译通过，生成宏为空且应用不含手动DNS设置日志，依赖基线一致，17项检查通过、5项跳过。固件2756608字节、分区余34%，SHA256=`beddb770e5eaadd509e56d46e7647df818a7677b89415e5e2c9fd928fd1c5e92`，日志`esp32/tmp/easyinput/auto-dns-build.log`。该版尚未烧录，设备仍为c2038c手动DNS版。

用户随后确认，仅将上述beddb7自动DNS应用烧入原设备，分区/身份一致、写入哈希校验成功；当前设备已恢复DHCP自动DNS。`esp32/tmp/easyinput/auto-dns-startup-validation.log`确认VM API HTTP200，主连接就绪、wifi/ws/raw均up。未重新调用ASR/TTS，自动DNS下语音及热点长期稳定尚未验收，不能沿用之前手动版的语音通过结论。

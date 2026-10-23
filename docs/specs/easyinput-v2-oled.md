# EasyInput V2：J4 OLED状态页

日期：2026-10-23。状态：首版29855c已烧录，用户确认显示、VOL联动、亮度调节正常；后续采样主连接已就绪，电台熄屏恢复与语音共存待验收。

## 硬件与范围

使用用户已接好的0.96寸128×64四线I²C SSD1315模块。接线依据easyinput-board-cy原理图第二页：J4-1 3V3接VCC，J4-2 RXD0/GPIO44接SCL，J4-3 TXD0/GPIO43接SDA，J4-4接GND。J4信号路径存在板载串联电阻；接线长度和模块实际3.3V上拉仍需实测。GPIO43/44由UART0改作I²C，要求原生USB Serial/JTAG控制台且不开启第二UART控制台。保留现有按键、旋钮、音频、分区和DNS配置。

首版为状态页：联网/配对、录音/识别/思考/合成/播放/暂停/错误、音量、灯光亮度、电池毫伏值和外接供电。复用现有ASCII小字体，不显示中文回复、不添加菜单或收费调用；电池首版只显示测量电压，后续估算百分比见下节，不宣称精确剩余容量。暂停状态优先于播放状态，静音提示最高。

## 电池估算百分比（2026-10-23）

实屏反馈：用户确认百分比、电压和USB已显示，但估算符号`~`偏高。核对原字体字形占第0～2行，而数字主体为第0～6行；仅在OLED文字绘制时将`~`下移2像素，使其占第2～4行，居于数字中线。不修改共享字体或其他字符；对应帧缓冲测试补充波浪号、相邻数字及页隔离断言。此调整先编译，烧录另行确认。

居中调整执行记录：沿用下述原环境、配置、受限元数据和构建命令，编译退出0，无编译器warning/error，日志`esp32/tmp/easyinput/oled-battery-tilde-build.log`。应用2756608字节，4MiB分区余34%，SHA256为`b851e1d4f823df9491a7ed01cba36f918903630c15fef73013b6b034c4b8f260`。运行原OLED测试命令，1项通过、2项因缺少主机C编译器跳过，`git diff --check`通过。此轮仅调整OLED文字绘制、相关测试及本文；未安装依赖、改全局配置、调用云端、执行清理或烧录。板上仍为上一版百分比显示，符号居中效果尚待烧录验收。

居中版烧录记录：用户另行确认“烧录”后，重新扫描COM6并实读原ESP32-S3的MAC `c4:9e:7e:07:9f:d8`，核验上述应用SHA256；读回分区表并确认前3072字节与本地一致、剩余1024字节全0xff。复查USB身份后，沿用仅写0x20000应用的esptool命令，退出0且数据哈希校验成功，未写模型、分区表或NVS。烧录日志`esp32/tmp/easyinput/oled-battery-tilde-flash.log`。硬复位后复扫同一USB身份，采集35秒至`esp32/tmp/easyinput/oled-battery-tilde-boot.log`。已请求用户确认波浪号居中及其他文字正常，当前实屏反馈待回复；未主动调用ASR/TTS或修改系统设置。

用户确认已接电池但型号/标称电压不明，并在充电指示灯绿色时授权只读采样：4214、4216、4216mV，external=1、charging=0。用户随后要求实现百分比。仅在OLED显示层加入电压插值估算，保留`~`标记、电压及USB/CHG/LOW状态：例如`BAT ~100% 4.21V USB`。0或负数等无有效采样时显示`BAT --% --V`，不把外接供电直接当作100%。显示百分比不保存到Flash，不影响充电、采样周期、3550mV低电压提示、音频或电源控制。

暂定工程显示刻度（mV→%）为3300→0、3500→5、3600→10、3700→20、3800→40、3900→60、4000→80、4100→90、4215→100，区间线性插值，结果限制0～100。上端采用本机满电采样参考；其余刻度没有对应电芯放电实验支持，不是电池规格或真实剩余容量曲线，不能用于续航承诺、充电保护或关机决策。0%也不表示电芯能量完全耗尽。电池型号、温度、负载、边充边用与ADC误差都会影响估算；后续取得电池资料和受控放电数据才能标定。满电参考只在源码定义，不自动学习、不写入NVS。

测试补充无效采样、满电边界、结果范围及单调性、USB/CHG显示与行宽；仍有两项C执行测试因缺少主机编译器跳过，不能视作已通过。此改动先编译，用户另行确认后才烧录验收。

执行结果：沿用本文件UI优化记录中的环境、构建命令、现有配置和官方只读元数据限制，没有安装/升级依赖或新增联网诊断。首次构建因snprintf数值范围不足触发format-truncation错误，仅补充显示范围检查后原命令重编译退出0，无warning/error；最终日志为`esp32/tmp/easyinput/oled-battery-percent-final-build.log`。应用仍为2756608字节，4MiB分区余34%；SHA256为`00179cdbdc92aad6043c231806441490d0fbce5078f18232f20637734cfd54a8`。执行`python -m unittest discover -s esp32/tests -p test_oled_text.py`，1项通过、2项跳过；`git diff --check`通过。仅修改本文、OLED绘制及对应测试；编译阶段未改全局环境、未烧录、未调用云端、未执行清理。

烧录验证：用户另行回复“烧录”后，复扫COM6并实读MAC `c4:9e:7e:07:9f:d8`及ESP32-S3芯片，校验上述应用SHA256。读取0x10000起4096字节分区区域，前3072字节与本地分区表一致、剩余字节均0xff；再次核验USB身份后，使用前述esptool仅写应用命令更新0x20000处固件，退出0并报告数据哈希校验成功，未写模型、分区表或NVS。日志`esp32/tmp/easyinput/oled-battery-percent-flash.log`。复扫同一USB身份后采集40秒，日志`esp32/tmp/easyinput/oled-battery-percent-boot.log`确认电池4212～4216mV、external=1、charging=0，网络state=4、Muse控制连接恢复、OTA应用验证成功、语音phase=7/error=0/ready=1，无panic记录。本次不主动调用云端。已请求用户核对屏幕百分比、电压及USB文字完整性，实屏验收尚待反馈。

## 状态页优化（2026-10-23）

### 2026-10-24：亮度映射与旋钮模式提示

名称确认：用户要求名称一并修改并烧录验证，采用前文推荐的屏幕名称`TinyPal`。OLED右上第1行改为TinyPal，板型仍为EasyInput V2，原有Muse配对身份和功能保持。

合并版执行记录：沿用本节构建环境、配置及命令，编译退出0、无编译器warning/error，日志`esp32/tmp/easyinput/tinypal-brightness-build.log`。应用2756608字节，4MiB分区余34%；SHA256为`74fc974dfadc5e82bc65f9b79b7face3297068d7a072efbc39632dc9772fd1c0`。名称断言加入原OLED渲染测试，重跑3项：1项通过、2项因缺少主机C编译器跳过，`git diff --check`通过。

用户已授权合并烧录。重新扫描COM6并实读ESP32-S3、MAC `c4:9e:7e:07:9f:d8`，校验上述应用SHA256。读取0x10000起4096字节分区区域，前3072字节与本地一致，剩余1024字节均0xff；复核USB身份后执行原esptool仅写应用命令更新0x20000，退出0且数据哈希校验通过，未写模型、分区表或NVS。日志`esp32/tmp/easyinput/tinypal-brightness-flash.log`。硬复位后复扫同一USB身份，开启180秒脱敏采集`esp32/tmp/easyinput/tinypal-brightness-validation.log`。

初始采集确认ready=1/error=0、电池4174～4178mV，无panic记录；但未发现附近保存过的Wi-Fi，setup=done、wifi/ws/raw=down，语音phase=5而非本地唤醒phase=7，不能报告联网或离线唤醒已恢复。已通知用户可先离线验收TinyPal、BRI/BRIGHTNESS、25/50/100%调光及模式退出；原热点恢复后再核对就绪工作绿灯，不重置配网。此刻实屏与调光观察尚待反馈。未安装依赖、改系统或DNS、调用云端、清理目录或提交推送。

用户反馈“OLED亮度没变化，其它功能正常”，因此TinyPal名称及模式提示等按用户反馈通过，OLED调光不能标为通过。已结束的180秒采集后段显示网络和Muse恢复、语音phase=7/error=0/ready=1；记录brightness模式及保存1%、31%、41%，没有100%保存记录。源码使用0x81对比度指令，按设置映射为1～64，但当前日志没有实际发送参数或逐次I2C返回码，不能直接判断控制器不支持、实际故障或仅视觉差异小。暂不提高上限或改控制方案。复核USB身份后开启`esp32/tmp/easyinput/tinypal-oled-brightness-endpoints.log`采集，请用户比较1%和100%端点，结果待反馈；本步不改源码、不编译/烧录、不联网查资料或调用云端。

端点测试结果：用户明确反馈“OLED仍完全没有变化”。日志确认359656ms保存1%、375460ms保存100%、391402ms恢复25%，且进入/退出brightness模式正常，语音phase=7/error=0/ready=1。当前本地固件SHA256仍74fc974dfadc5e82bc65f9b79b7face3297068d7a072efbc39632dc9772fd1c0。因此操作和设置值已确认，不能继续解释为仅在低档范围内变化不明显；OLED指令是否实际下发、休眠是否覆盖、模块实际响应仍无证据。已向用户请求增加最小参数/I2C结果诊断、只读核对dl.espressif.com的SSD1315资料并编译；许可待回复，烧录另行确认。本步未更改控制时序、亮度上限或驱动，不把无I2C错误等同于亮度已经生效。

诊断授权：用户回复“允许诊断与编译，烧录另行确认”。只读核对[SSD1315原厂资料](https://dl.espressif.com/dl/schematics/SSD1315.pdf)第38页（PDF索引37）命令表及第49页（索引48）命令说明：0x81是双字节对比度指令、有效参数0x01～0xff；当前0x01～0x40合法。第16页（索引15）说明I2C控制字节Co/D-C含义，与现有0x00命令包形式相符。这只证明代码形式符合SSD1315规格，不证明所接模块芯片身份或光学响应。

诊断版仅在OLED设置、休眠/熄屏状态或对比度参数变化时输出：requested百分比、sleeping/dark、0x81参数、action与传输结果。action=write携带真实I2C返回码；action=cached表示有效值未变化且没有重发；action=deferred表示电台实际熄屏期间暂缓应用亮度，不能冒充发送成功。0xae/0xaf显示开关另记录返回码。日志不含地址/凭据，不周期刷屏；不增加重试、提高上限、修改初始化或更换驱动。预期非休眠时1%=0x01、25%=0x05、50%=0x11、100%=0x40。

诊断版执行结果：仅修改本轮OLED诊断输出和本文，沿用env.ps1、既有配置和受限元数据环境，执行原命令`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，ESP-IDF 6.0.1退出0，无编译器warning/error。日志`esp32/tmp/easyinput/tinypal-oled-contrast-diagnostic-build.log`；应用2756608字节，4MiB分区余34%，SHA256为`d4032a35dfb764253080d5ff717cd7cc9ebd91136935d327e90b0052f80e747d`。模型哈希仍b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007。重跑test_oled_text.py，1项通过、2项因缺少主机C编译器跳过；git diff --check通过。本轮只读访问dl.espressif.com官方PDF及沿用既有构建元数据限制，未安装依赖、改系统/网络、调用云端、清理或烧录。板上仍为74fc97合并版，OLED无亮度变化尚未修复；需用户另确认烧录诊断版，再比较端点的实际发送记录。

用户另确认“烧录”后，复扫COM6并实读ESP32-S3、MAC `c4:9e:7e:07:9f:d8`，核验d4032a应用哈希。读回0x10000起4096字节分区区域，前3072字节与本地一致、剩余1024字节均0xff；复核USB身份后执行原仅写0x20000应用的esptool命令，退出0且写入数据哈希校验成功。未写模型、分区表、NVS；日志`esp32/tmp/easyinput/oled-contrast-diag-flash.log`。硬复位后复扫同一USB身份，开启240秒脱敏采集`esp32/tmp/easyinput/oled-contrast-diag-validation.log`，确认Muse控制连接、网络state=4、OTA验证成功、语音phase=7/error=0/ready=1，无panic记录。已请求用户在BRIGHTNESS下依次1%/100%各停3秒，再恢复舒适档；此刻尚未收到新的端点结果，不将诊断版烧录成功当作OLED亮度已修复。

诊断复测用户仍反馈屏幕亮度无变化。日志54080ms显示requested=1、sleeping=0、dark=0、command=0x81、value=0x01、action=write、result=ESP_OK；69497ms显示requested=100、sleeping=0、dark=0、value=0x40、action=write、result=ESP_OK；84209ms恢复25%并发送0x05。可确认设置及发送参数正确，没有休眠覆盖，也没有总线错误；不能由此证明模块已按参数改变段电流，不能断言具体芯片型号或硬件损坏。采集已结束释放串口。

继续只读核对同一官方PDF第39页（索引38）D9h命令表及第53页（索引52）说明，预充电两相的非零1～15档有效，当前初始化参数0xF1。提出验证性方案：保留Phase1=1，低档Phase2=1逐步到原最高15，即0x11～0xF1；0x81上限仍0x40，不提高原预充电上限、不加入闪烁调光。方案尚不能保证面板响应，低档可能很暗或显示不稳定，回滚为当前d4032a诊断版。已询问用户是否允许先开发编译、另确认烧录，许可待回复；当前没有实施该驱动方案或新增写入。

用户允许“试验版开发与编译，烧录另确认”。构建前将当前d4032a诊断应用保存在已忽略的Workspace文件`esp32/tmp/easyinput/oled-contrast-diag-d4032a-muse-gadget.bin`，校验SHA256一致，存在同名不同内容时停止，不覆盖备份。

试验实现只新增OLED预充电映射：Phase2=1+round(14×百分比²/10000)、Phase1固定1；非休眠时1%/25%/50%/75%/100%分别为0x11/0x21/0x51/0x91/0xF1，休眠取0x11。初始化仍保留原0xF1，显示任务首轮根据已保存亮度应用映射；只在实际参数变化时发送0xD9并记录返回码，成功后更新缓存，失败沿用原错误退出策略，不自动重试。原0x81对比度控制、电台熄屏与恢复、LED灯条/工作灯映射和NVS存储保持。

测试补充所有档位预充电非零且单调、端点及越界输入钳制、最高恢复0xF1；不把公式用例视作光学验证。OLED调光效果与低档显示稳定性仍待用户另确认烧录后现场验证。

试验版执行记录：修改easyinput_brightness.h、easyinput_oled.c及对应亮度测试/本文。沿用原env.ps1、配置与受限元数据环境，执行原IDF构建命令，ESP-IDF 6.0.1退出0，无编译器warning/error；日志`esp32/tmp/easyinput/oled-precharge-trial-build.log`。应用2756608字节，4MiB分区余34%，SHA256为`ee69b27a2e9d270cc82a3c9f188e34b8a416eba60c789f21ea938d083430482a`。模型SHA256仍为b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007。分别执行test_easyinput_brightness.py与test_oled_text.py，共4项，1项通过、3项因缺少主机C编译器跳过；git diff --check通过。已校验回滚文件被Git忽略，不提交固件。未新增依赖或联网诊断、修改全局环境/DNS、清理目录、调用云端或烧录。板上仍为d4032a诊断版，试验固件待用户另确认烧录。

2026-10-24用户另行确认“烧录”。复核COM6、USB序列号及实读MAC `c4:9e:7e:07:9f:d8`，读取0x10000起4096字节分区区域；前3072字节与本地分区表逐字节一致，其余1024字节全0xff。再次核验USB身份及上述ee69b27完整应用SHA256后，仅执行`python -m esptool --chip esp32s3 -p COM6 -b 460800 --before no-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin`。退出0，写入2756608字节并通过设备数据哈希校验，日志`esp32/tmp/easyinput/oled-precharge-trial-flash.log`。未写分区表、模型、NVS或启动程序，未调用云端、安装依赖、修改系统/DNS或清理文件。

硬复位后重新核验同一USB身份，启动240秒脱敏采集`capture-asr.py COM6 240 tmp/easyinput/oled-precharge-trial-validation.log`；早期样本显示语音phase=7/error=0/ready=1。已请用户比较25%、50%、100%，画面清晰再试1%，每档停约3秒并恢复舒适档；低档难读可旋转恢复100%。此刻实际亮度差异与显示稳定性仍待用户反馈，不能把写入校验或I²C成功当作调光已修复。回滚诊断镜像保持在上述Workspace备份位置，当前不执行回滚或清理。

本轮实屏验收：用户对上述亮度与画面稳定性测试反馈“工作正常了”，据此记录预充电调光的本轮人工验收通过。采集日志可见多档0xD9与0x81发送返回ESP_OK，包含25%预充电0x21、75%为0x91及低档0x11；1%时对比度已为0x01，记录cached未重复发送。软件日志与用户观察共同支持本轮调光生效，不推断模块实际芯片型号或其他模块同样有效。此次仅更新验证文档，没有新编译、烧录、云端调用或提交推送；重启后保存档位恢复及长时间低亮显示仍未在本轮单独验收。

用户确认LIGHT数值变化、实际灯光/屏幕变化不明显，要求按建议优化。本轮将LIGHT缩写改为BRI，用`>`标示旋钮当前目标，例如`>VOL  60%  BRI  25%`或` VOL  60% >BRI  25%`。亮度模式底行明确显示`BRIGHTNESS:  25%`；原有短按进出、5秒无操作返回音量、停止旋转2秒后保存均保留。电台选台使用已有radio_select_steps返回状态发布模式，选台时不误标调音量，显示`RADIO: TURN TO PICK`。静音、错误、暂停等上半部状态不被调节模式覆盖。

灯条、工作绿灯与OLED统一采用固定二次曲线：输出为上限×(设置百分比/100)²，整数四舍五入。灯条每通道最高仍为`1+(min(原颜色通道,50)-1)/4`（原颜色为0时输出0），不再强制每个暗色通道保留一级，因此低档可显著调暗；低档部分或全部灯条可能熄灭，不增加时域抖动或闪烁。工作绿灯PWM上限仍256，设备就绪时最低输出1；OLED对比度仍为1～64，显示休眠仍取1。相同百分比下视觉亮度会比旧版更暗，原NVS保存值不迁移、不重置；光学效果尚须实屏确认，不能承诺每5%都有可分辨变化。

| 设置 | 灯条单通道（原颜色≥50） | 工作绿灯PWM | OLED对比度 |
|---|---:|---:|---:|
| 25% | 1 | 16 | 5 |
| 50% | 3 | 64 | 17 |
| 75% | 7 | 144 | 36 |
| 100% | 13 | 256 | 64 |

新增公共亮度计算头文件以复用同一映射，主机测试覆盖单调性、最大输出保持、暗色可调暗以及旋钮模式/静音显示共存。未引入亮度自动调节、传感器、菜单或新配置项。本轮先编译、不烧录、不调用云端服务。

执行记录：首次编译暴露voice.h在配置头加载前使用条件声明，新增sdkconfig.h包含后沿用原环境/配置/命令重编译，不调整构建配置。执行`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，退出0、无编译器warning/error；日志`esp32/tmp/easyinput/brightness-mode-curve-final-build.log`。应用2756608字节，4MiB分区余34%；SHA256为`fd72abdb052ef979b3f776f6a95c3f21c30bfafc815e71db991f213606255253`。模型仍为b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007。

本轮修改OLED、voice旋钮模式只读发布、easyinput_board工作灯映射、led_status灯条映射，新增easyinput_brightness.h及对应测试，更新本文。分别执行`python -m unittest discover -s esp32/tests -p <文件名>`覆盖test_easyinput_brightness.py、test_oled_text.py、test_briefing.py、test_story.py，共16项，8项通过、8项因缺少主机编译器跳过；`git diff --check`通过。沿用既有SDK及官方元数据域名限制，未安装/升级依赖、改全局环境或DNS、调用云端、清理目录、烧录或提交推送。待用户确认烧录后验收25/50/75/100%区别、BRI模式标记、5秒退出、重启保存、静音/电台共存；板上仍为此前错误提示/等待续播优化版。

保留左上32×32机器人，上半部右侧四行改为设备名、网络状态、功能状态、操作提示，移除重复的Cyber Pet标题。右侧每行最多15个ASCII字符，底部保留音量/亮度、电池毫伏值/供电、情境快捷键三行。

功能状态优先级：静音、错误、暂停、录音/结束候选、识别、思考、等待音频、播放、网络未就绪，最后才是爱心/充电/低电量/睡眠/待机。静音显示S2解除，暂停显示S3续播，播放显示S4暂停，网络异常提示打开Muse App或长按S5查询状态。录音、识别和等待音频分别显示，避免将尚未出声的合成阶段标为正在播放。提示只描述原有操作，不改变按键或音频策略。

只有联网就绪、未静音/未暂停、非充电/非低电量、语音空闲且超过45秒无活动时，才显示睡眠表情并调暗；这是显示休眠，不是芯片睡眠。最后活动计时只归OLED任务维护，按键/旋钮仅发布原子的活动截止时间；电台熄屏期间的最近一次活动在恢复显示时仍纳入计算。电台熄屏优先规则保持。

验证：覆盖充电/爱心与静音、暂停状态重叠，断网及低电量不被睡眠覆盖，活动恢复亮度，各功能状态文字在15字符范围内；修正图标叠加绘制测试的错误断言。主机C编译器缺失时必须标明对应渲染测试跳过，不能算作通过。烧录与实屏验收仍需用户另行确认。

执行记录：沿用已审查的`tmp/easyinput/env.ps1`、metadata-python限制和现有配置，在esp32目录执行`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build`，ESP-IDF 6.0.1，退出0，编译日志未见warning/error。应用大小0x2a1000（2756608）字节，4MiB应用分区余34%；SHA256为`bfbacbbfd8f6ee9121d2427c15e4f11bf3d9fc3e81e3cf10d0b043c52017620d`。日志位于`esp32/tmp/easyinput/oled-ui-optimize-build.log`；模型镜像SHA256仍为`b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007`。

烧录记录：用户明确要求“刷机验证”后，重新扫描COM6，实读芯片ESP32-S3、MAC `c4:9e:7e:07:9f:d8`。读取0x10000起4096字节分区区域，前3072字节与本地分区镜像完全一致，剩余1024字节均为0xff。再次核对USB身份和应用SHA256后，执行`python -m esptool --chip esp32s3 -p COM6 -b 460800 --before no-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-soft-aes-s3/muse-gadget.bin`，退出0，写入2756608字节且工具报告数据哈希校验成功。仅更新应用，未写分区表、模型或NVS。日志`esp32/tmp/easyinput/oled-ui-optimize-flash.log`。

复扫同一USB设备后，使用现有脱敏采集器`capture-asr.py COM6 45 tmp/easyinput/oled-ui-optimize-boot.log`采集，确认Muse控制连接恢复、OTA应用验证成功、网络state=4、语音phase=7/error=0/ready=1，采集范围未见panic或重启循环；采集未覆盖上电最早阶段，因此尚无本轮OLED发现地址日志。用户自行触发S1后日志收到输入事件及识别文本；本轮没有由程序自动发起云端请求。另开启240秒UI检查采集，用户回复“工作正常”，确认新版四行布局、静音提示不被爱心/充电遮盖、解除静音、机器人/爱心及旋钮VOL变化正常。暂停提示、45秒显示休眠与活动恢复、电台熄屏/恢复仍未完成本版专项验收。

从仓库根目录运行`python -m unittest discover -s esp32/tests -p test_oled_text.py`，3项中1项文字宽度检查通过、2项因缺少主机C编译器跳过；`git diff --check`通过。修改仅本文、`esp32/main/easyinput_oled.c`、`esp32/tests/test_oled_text.py`。未安装/升级依赖、未修改全局环境、未调用ASR/TTS、未烧录或清理目录。构建继续限制为components.espressif.com及components-file.espressif.com的只读依赖元数据，不允许组件包下载。待用户确认烧录后，实屏验收静音/暂停优先级、睡眠调暗与恢复、电台熄屏共存；不能将本地构建通过视作硬件验收通过。

## 实现约束

配置开关HOMEHUB_EASYINPUT_OLED默认关闭，仅EasyInput语音板型可开启。100kHz，启动时仅探测SSD1315两个合法地址0x3C/0x3D；未找到设备或传输错误则记录错误并退出显示任务，不循环重试、不改地址协议、不影响已有音频逻辑。模块需正常供电且自带合适上拉；内部弱上拉不代替外部上拉资格确认。

SSD1315使用页寻址，清空显存后才点亮。每500ms读取只读状态，变化时才传帧，I²C操作不持有LED锁；任务优先级1，3072字节任务栈与两个1KiB帧缓冲位于PSRAM。OLED任务不执行Flash写入或模型初始化，避免额外占用TLS需要的内部RAM。OLED对比度跟随已保存的旋钮灯光亮度，最大值限制为64；该对应关系不代表光学亮度线性。电台实际熄灯标志有效时屏幕关显示，AI打断解除该标志时恢复，不改变电台所有权与60分钟计时。

## 验收

先编译及本地检查，用户另确认后才烧录。实机验证I²C发现地址、128×64边界/方向/无花屏、音量与亮度联动、静音与暂停状态、电压与USB状态、睡前电台熄屏与退出恢复，以及Wi-Fi/语音/重播不受影响。不得把编译通过当作屏幕已点亮；未连接/断线场景应无重启循环、按键和音频仍可工作。串口验证不主动调用收费ASR/TTS，若需云端验收再明确范围。

## 资料

- [SSD1315原厂资料（乐鑫托管）](https://dl.espressif.com/dl/schematics/SSD1315.pdf)：地址、页寻址、控制字节、电荷泵及对比度命令。
- [ESP-IDF 6.0.1 I²C](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/peripherals/i2c.html)：主机总线、地址探测和同步传输。


## 本轮执行与验证记录

新增main/easyinput_oled.c/h、main/oled_text.h、tests/test_oled_text.py及本文。修改main/CMakeLists.txt、main/Kconfig.projbuild、main/main.c接入可选显示任务；led_status.c/h、network_feedback.c/h、voice.c/h仅补充只读状态接口。未更改原按键映射、音频播放流程、分区或DNS；保留此前全部未提交修改。

本机在现有tmp/easyinput/sdkconfig.soft-aes-s3追加CONFIG_HOMEHUB_EASYINPUT_OLED=y，并将单项开关保存在tmp/easyinput/sdkconfig.oled.local（均为忽略的本地配置）。沿用原defaults列表，末尾加此OLED配置。实际构建命令：

```powershell
python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-soft-aes-s3 -D IDF_TARGET=esp32s3 -D SDKCONFIG=tmp/easyinput/sdkconfig.soft-aes-s3 build
python -B -m unittest tests.test_oled_text tests.test_briefing tests.test_qwen_tts_transport tests.test_story tests.test_network_feedback
python -B -m unittest discover -s tests -p 'test_*.py'
```

构建前加载现有tmp/easyinput/env.ps1，沿用IDF_COMPONENT_MANAGER=1及受限metadata-python。只允许components.espressif.com、components-file.espressif.com只读依赖元数据，禁止包下载/版本升级/managed_components自动清理。未安装新依赖或修改全局环境。文档核对仅访问乐鑫托管SSD1315原厂PDF及ESP-IDF官方文档；未调用云端生成或语音服务。

最终构建退出0，无编译器warning/error匹配，应用2756608字节，4MiB分区余34%；SHA256=`29855c1c040408bb481a1968aa156857716d652ea4da72bc716257f5c14cead4`。检查生成头文件确认OLED开启、USB Serial/JTAG控制台、无第二UART控制台及DNS仍为192.168.1.1；ELF含OLED驱动日志字串，显示代码已链接。8项依赖锁版本/哈希保持基线，分区表与先前设备读取一致。最终日志esp32/tmp/easyinput/oled-final-build.log；大小/哈希脱敏记录oled-build-result.json。

定向18项：10通过、8跳过（缺主机C/C++编译器，实际OLED绘制函数边界测试也未执行）。全仓156项：2失败、25错误、36跳过；仍涉及主机编译器缺失及基线radio日志标签，未修无关代码。日志oled-tests.log、oled-all-tests.log；git diff --check通过。不能宣称全部测试通过。

本轮未读取或输出真实密钥、未烧录、未提交推送、未执行清理。板上仍为此前3b1d5e手动DNS版，不含OLED；本地bin已更新为29855c，等待用户另行确认后鲜核同一MAC/分区再仅写应用。显示方向、地址、上拉、软硬件音频共存和实际亮度均未实机验收。192.168.1.1仍只适用于当前网络，换网需另行调整。


## 烧录与首次启动采样

- 用户明确要求烧录。鲜扫COM6/USB序列C4:9E:7E:07:9F:D8，读取MAC c4:9e:7e:07:9f:d8，分区0x10000起0xc00与本地逐字节相同；应用2756608字节、SHA256=29855c1c040408bb481a1968aa156857716d652ea4da72bc716257f5c14cead4核验一致，仅write-flash 0x20000，退出0且Hash of data verified。未写NVS/分区/模型，未改变用户Wi-Fi/配对/音量/亮度设置。设备已更新为29855c；旧3b1d5e已替换。
- oled-startup.log采集55秒，8～57秒应用心跳持续，原有电池任务、Wi-Fi扫描和本地网络提示工作，样本无panic/WDT/重启循环。但扫描报告no saved wifi network nearby，wifi/ws/raw均down；未见OTA image validated，不能声称联网或完整启动验收闭环通过。需用户开启原热点或另行处理换网；当前固定DNS192.168.1.1适用范围仍需保留。
- 首次串口采集约7.5秒开始，最早OLED发现地址日志未捕获，不能声称初始化成功。已请用户确认实际屏幕EasyInput/Muse、WIFI CONNECTING文字、旋钮音量/亮度联动及方向/花屏，并先不触发云端语音。未把缺日志当作OLED失败；物理画面验收仍待回复。
- 证据esp32/tmp/easyinput/oled-identify.log、oled-partition-read.log、oled-flash.log、oled-startup.log。采集已结束释放串口。本轮实际命令read-mac、read-flash、Python本地比较/哈希、write-flash应用与capture-asr.py COM6 55；未调用ASR/TTS/简报、未安装依赖/改系统/清理/提交推送。


## 用户观察确认

2026-10-23用户反馈“屏幕显示正常”，据此记录点亮与基本显示通过。此反馈未明确覆盖旋钮音量/亮度、电台熄屏恢复或联网，不升级其它验收项。本次只更新文档，没有新编译、烧录或云端调用。


## 八键验收期间的增量反馈

用户进一步确认VOL数值变化、亮度调节正常。eight-buttons-validation.log显示S1～S8均有按下/松开，S2静音开关正常；后续心跳wifi/ws/raw均up。eight-buttons-functions.log中S1持续按住3881ms仍未见录音/ASR/TTS，功能原因未定，不能将八键功能或语音共存全部标为通过。采集最终串口异常结束，交接鲜扫无串口；恢复USB后优先采集音频/唤醒初始化并检查语音准入。当前代码未因该反馈改动或重新烧录；细节、授权与下一步见flow/进展.md顶部交接。


## S1诊断版（待另确认烧录）

用户已批准在voice.c补最小只读诊断并编译。S1准入、事件入队/消费及10秒周期阶段快照只含布尔量/编号/错误码，不改录音策略，不输出凭据或录音。阶段映射及详细执行记录见flow/进展.md顶部。新构建SHA256=2c7de957986df68721e1af1acfb8a969c4704193bc725496f009a25f78fa7410，2756608字节，IDF6.0.1编译通过；定向18项10通过8跳过，未重跑全仓。分区、依赖、OLED与DNS配置保持。板上仍29855c，本地bin已更新为2c7de9；诊断版尚未烧录，S1原因仍未确定。


## S1阻塞定位与模型头核对

2c7de9诊断版已获用户确认仅写应用并验证启动。周期日志持续phase=10、error=260、ready=1、mute=0，主连接正常。用户另准只读0x822000起316字节，实际模型数量3221496196，与本地要求2不符；check_model_image会在此返回ESP_ERR_INVALID_SIZE并使语音任务挂起。现有证据不足以说明模型数据异常成因，不把时间上的OLED接入归结为因果。已申请仅重写本地“你好小益＋VAD”模型（579135字节、SHA256 b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007），尚未执行，等待用户确认。应用/分区/NVS/OLED设置保持，详细证据与后续边界见flow/进展.md。


## 模型区修复后验证

用户明确确认仅重写模型。鲜核原板与分区、模型哈希后仅写0x822000模型文件，设备哈希校验通过；应用保持2c7de9，未写NVS/分区或更改OLED。voice-model-repair-startup.log显示phase由10/error260恢复7/error0、本地listening locally及Wi-Fi/Muse就绪，OTA应用验证通过。恢复后出现两条AFE空缓冲警告，随后持续本地监听。模型头导致的语音任务挂起已解除；S1完整问答与其余功能仍需实际验收，不提前记为全部通过。


## 模型修复后S1问答通过

用户反馈回复正常，日志确认S1事件准入与消费、2.92秒录音由松键结束、ASR、Muse回复、两段Qwen-TTS下载与扬声器播放完成，并恢复phase7/error0本地监听；已有1656320字节重播缓存。确认带OLED的当前诊断版在此轮S1完整链路正常，不升级为长期稳定性全部通过。S3/S4和电台熄屏等另验。

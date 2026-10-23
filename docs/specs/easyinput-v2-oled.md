# EasyInput V2：J4 OLED状态页

日期：2026-10-23。状态：首版29855c已烧录，用户确认显示、VOL联动、亮度调节正常；后续采样主连接已就绪，电台熄屏恢复与语音共存待验收。

## 硬件与范围

使用用户已接好的0.96寸128×64四线I²C SSD1315模块。接线依据easyinput-board-cy原理图第二页：J4-1 3V3接VCC，J4-2 RXD0/GPIO44接SCL，J4-3 TXD0/GPIO43接SDA，J4-4接GND。J4信号路径存在板载串联电阻；接线长度和模块实际3.3V上拉仍需实测。GPIO43/44由UART0改作I²C，要求原生USB Serial/JTAG控制台且不开启第二UART控制台。保留现有按键、旋钮、音频、分区和DNS配置。

首版为状态页：联网/配对、录音/识别/思考/合成/播放/暂停/错误、音量、灯光亮度、电池毫伏值和外接供电。复用现有ASCII小字体，不显示中文回复、不添加菜单或收费调用；电池只显示测量电压，不虚构精确剩余百分比。暂停状态优先于播放状态，静音提示最高。

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

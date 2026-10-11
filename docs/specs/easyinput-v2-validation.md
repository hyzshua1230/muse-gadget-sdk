# EasyInput V2 首期实现与验证记录

日期：2026-10-10。基线：812c46f9fe42b69d916613a9dc702cbac5ff7b54。
开发分支：codex/easyinput-v2-port。

## 实现内容

- 本板构建 overlay：ESP32-S3、16MB Flash、8MB Octal PSRAM、原生 USB Serial/JTAG。
- 复用 partitions_muse.csv 的双 4MB 应用槽；烧录前核对设备当前分区，不能直接假设旧 NVS 兼容。
- 板级模块唯一持有 GPIO8；上电先准备安全信号状态，播放结束不切断共享供电。
- I2S0 右声道采集；31tap FIR 后 48kHz 转 16kHz。I2S1 48kHz 双槽播放，数字音量。
- 八键与旋钮按压消抖；S1/S2/S4/S8 业务独立，其他键仅记录输入。
- 软件静音重启不保存；取消代次区分新旧录音请求，取消不会自动开始下一轮。
- 五灯 GRB、颜色分量不超过50；录音音量优先于旋钮音量。绿灯表示 Muse 连接状态。
- 就绪后低电平800Hz/120ms扬声器提示音。文字回复不包含TTS。
- Muse订阅实际事件进展连续60秒停滞时报错，不以截断的本地字幕判定进展。
- 后台ADC采样与标定，device.health读取缓存电压、外部供电和充电状态，电量百分比未知。

## 验证状态

| 检查 | 当前结果 |
| --- | --- |
| ESP-IDF 实际版本 | v6.0.1 |
| 交叉编译器 | GCC 15.2.0，esp-15.2.0_20251204 |
| FIR 信号指标测试 | 3项通过：直流/线性相位、语音通带、高频抑制 |
| diff 空白检查 | 通过 |
| EasyInput 完整构建与 overlay 应用检查 | 通过：配置退出0、编译退出0，overlay 全部生效，token已配置但不记录内容 |
| 固件大小与应用槽 | 签名后1,445,888字节；4MiB槽余2,748,416字节（65.53%） |
| 实际二进制分区表 | 通过：NVS 0x11000/48KiB；应用槽0x20000、0x420000各4MiB；产品分区0x820000、0x821000 |
| 静态 DIRAM 布局 | 使用189,333字节，剩余152,427字节；不能替代运行时堆测量 |
| 共享后端/组件回归构建 | ideaspark、SenseCAP Indicator、AIPI全部通过；回归副本未配置用户token，只用于编译检查 |
| 完整主机测试 | 未通过：97项报告通过、24项跳过；1项失败和26项错误均为缺少主机C/C++编译器，含测试类初始化错误 |
| 设备识别、分区核对与烧录 | 2026-10-10通过：COM6，ESP32-S3，MAC c4:9e:7e:07:9f:d8；用户确认分区迁移后烧录退出0，写入校验通过 |
| 启动、按键、音频、联网和内存验证 | 未验证：烧录后40秒及追加60秒串口采集均无日志，已请求用户用板上开关关机再开机 |
| 电压精度 ≤100mV | 缺少外部测量，未验证 |

48kHz/32bit右槽与20ms供电/ADC等待为首轮资格测试参数。
器件原生格式、实际波形、帧率与时长须在运行验证中取得依据。
未进行实际编译前，这些实现不能报告为已经可运行或通过验收。

## 执行记录

- 只读核对：Get-Content、rg、git status、两张已授权原理图中的第二页。
- 分支创建：git switch -c codex/easyinput-v2-port。
- 实现：新增 easyinput_board.c/.h、voice_board_easyinput.c、sdkconfig.easyinput-v2；
  修改 Kconfig/CMake、主入口、按键、灯光、语音、Muse回复超时和device.health；更新板型文档/辅助脚本。
- 环境：在 esp32/tmp/easyinput/env.ps1 装配当前进程变量；实际执行 idf.py --version 和编译器版本检查。
- 测试：python -m unittest discover -s esp32/tests -p test_easyinput_audio_filter.py；git diff --check。
- 串口枚举：python -m serial.tools.list_ports -v，结果 no ports found。
- 编译配置：python <IDF_PATH>/tools/idf.py -B build-easyinput-v2 -DIDF_TARGET=esp32s3 reconfigure。
- 固件编译：python <IDF_PATH>/tools/idf.py -B build-easyinput-v2 build。
- 主机测试：python -m unittest discover -s tests -p 'test_*.py'；本次执行时发现226项，测试类初始化失败使完整用例未执行。
  后续补充4项生产按键任务测试（S1恢复键隔离、业务路由、S8单次长按、消抖），当前共230项；新增4项尚待主机编译器执行。
- 容量分析：python -m esp_idf_size --format text build-easyinput-v2/muse-gadget.map。
- 分区验证：python <IDF_PATH>/components/partition_table/gen_esp32part.py build-easyinput-v2/partition_table/partition-table.bin。
- 乐鑫组件依赖已下载到工程：cJSON 1.7.19~2、esp_websocket_client 1.2.3、led_strip 3.0.3。
  主要服务域名按执行前说明为 components.espressif.com、components-file.espressif.com、dl.espressif.com；配置日志没有逐请求域名记录。
- token按用户明确指令写入被Git忽略的本机overlay；验证只输出“已配置”，未将内容写入本文或公开日志。
- 编译环境仅在当前进程装配；补齐已安装ROM ELF路径，未修改系统环境或安装额外工具链。
- 构建日志：esp32/tmp/easyinput/configure.log、build.log、host-tests.log、size.log。
- 回归检查：在 esp32/tmp/checks/l 和 u 中用 git archive HEAD esp32 创建基线副本，覆盖本次修改的源码，不复制被Git忽略的本机配置。
  在独立构建目录中依次使用对应SDK默认配置调用 idf.py build：
  `build-ideaspark`、`build-sensecap-indicator`、`build-aipi`，全部退出0。
  对应日志为 ideaspark-build.log、sensecap-build.log、aipi-build.log。
  ideaspark应用槽剩余22%，SenseCAP Indicator剩余19%，AIPI剩余48%，均不触发10%余量评审线。
- 新增/修改测试脚本经过Python语法检查；缺少主机编译器的运行测试没有因此标记通过。
- 原始配置存在缺少ESP_ROM_ELF_DIR的非致命调试文件警告；后续进程已补齐该变量，固件编译通过。
- 未写入设备；主机编译环境只读核验尚待用户许可。运行验证与电气参数资格测试仍未完成。
- 临时环境与后续日志在 esp32/tmp/easyinput，已被仓库忽略。当前不建议清理未完成验证的日志与产物。

## 本轮结论

EasyInput本板构建、全部overlay配置检查、二进制分区核对与三个回归构建通过。
完整主机测试未通过，当前原因是没有可用主机C/C++编译器；已有安装环境的只读核验待用户授权。
尚未烧录，GPIO、电气稳定时间、麦克风格式、提示音、联网及并发内存等运行验收没有通过记录。
固件位于 esp32/build-easyinput-v2/muse-gadget.bin；本轮产物包含用户SDK配置，仅用于当前设备。

## 2026-10-10 烧录记录

- 串口重新枚举：`python -m serial.tools.list_ports -v`，识别COM6。
- 只读验身：`python -m esptool --chip esp32s3 --port COM6 --after no-reset chip-id`，MAC c4:9e:7e:07:9f:d8，ESP32-S3 revision v0.2，内置8MB PSRAM。
- 通过esptool读取分区候选区并仅输出解析后的分区元数据，未保存原始Flash内容。
  旧表位于0x8000：NVS 0x9000/24KiB、phy 0xf000/4KiB、factory 0x10000/3MiB、sound_a 0x310000/576KiB、sound_b 0x3a0000/576KiB。
- 用户明确确认目标设备和分区迁移；旧Wi-Fi和配对配置不能沿用，需要重新配置。
- 在build-easyinput-v2目录执行：
  `python -m esptool --chip esp32s3 --port COM6 --baud 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size keep --flash-freq 80m 0x0 bootloader/bootloader.bin 0x10000 partition_table/partition-table.bin 0x1d000 ota_data_initial.bin 0x1f000 phy_init_data.bin 0x20000 muse-gadget.bin`。
  退出0，写入数据校验通过；日志esp32/tmp/easyinput/flash.log。
- 没有整片擦除、eFuse写入、额外依赖安装或联网；本轮仅写入上述固件区域，未修改源代码。
- COM6有界采集40秒，追加60秒，均0行；日志esp32/tmp/easyinput/boot-verified.log。
  采集时DTR/RTS置低，并在保存前过滤凭据相关日志。
- 已请求用户使用板上开关关机再开机，不按BOOT；尚待确认正常启动和提示音。
  写入成功不代表运行验收通过；联网、按键、音频及电池精度仍未验证。
- 保留本机构建和日志用于后续诊断；不要将包含SDK配置的固件公开上传。

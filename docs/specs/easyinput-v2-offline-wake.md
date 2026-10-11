# Spec：EasyInput V2 ESP-SR离线唤醒首版

日期：2026-10-11。状态：“你好小益”固件及模型已烧入，Flash读取与PSRAM任务栈冲突已修复。模型初始化、启动、联网与回复播放有日志证据；用户确认不按键唤醒、自动文字及语音回复正常，基本功能验证通过。连续唤醒、误触发、静音切换和长回复资源压力验收待完成，不作为商用资格认证。

## 1. 已确认范围

- 使用兼容的内置唤醒词“你好小益”；用户已确认替换最初的“Hi，乐鑫”。固定组件包提供WakeNet模型`wn9_nihaoxiaoyi_tts2`及`vadnet1_medium`，不申请定制模型。
- 唤醒后自动录音，检测说话结束提交，最长15秒。
- 回复播放期间停止唤醒检测，S1仍可手动打断；S2静音时停止监听，解除静音恢复空闲监听。
- 保留现有S3续播/重播、S4暂停/取消和S8配对/恢复；识别文字与回复仍使用Qwen ASR、Muse、Qwen TTS。
- 唤醒词识别在设备本地完成。空闲监听音频不上传云端；只有唤醒或按S1触发的有效录音才提交。
- 不增加离线问答、MultiNet固定命令、播放期间语音打断、自动休眠、自定义词训练或备用服务。

## 2. 已核验事实与依赖

现有设备ESP32-S3、8MiB PSRAM、16MiB Flash。音频驱动已提供16kHz/PCM16单声道，来自48kHz右槽，经既有FIR抽取。当前只有按键录音时开启麦克风，不能让新增监听任务与`record()`同时读同一I2S。

依赖固定为`espressif/esp-sr` 2.6.0、`esp-dl` 3.3.12、`dl_fft` 0.6.0和`esp_new_jpeg` 1.0.2。后者来自ESP-DL的必需依赖清单。ESP-SR登记的IDF约束为>=5.0，另依赖cJSON；其esp-dsp规则不包含ESP32-S3，因此本板不引入该包。此声明包含现有IDF6.0.1，但不是本工程构建或运行兼容性证明。

官方登记ESP-SR归档约175.87MB，附属依赖、解压、模型和构建占用需下载后统计；不预先承诺总磁盘占用。下载主要经过`components.espressif.com`及其组件存储/CDN，核对来源和包校验；官方源码核验涉及`github.com/espressif`。

源代码、依赖锁和模型配置留在本Workspace；复用现有SDK和workspace内缓存，不修改全局环境、不安装其他IDF版本、不读取或输出真实凭据。先展开组件CMake、模型生成脚本及清单，确认是否还有外部下载或执行步骤；有额外行为先说明并确认。

## 3. 实现方案

### 音频所有权

建立唯一麦克风采集路径，按空闲监听、手动录音、唤醒录音及停止状态路由音频。按键任务仅发事件，不能执行I2S或模型推理。AFE的feed/fetch块大小通过所选版本接口查询，不假定与现有320样本读取块一致；使用有界拼帧缓冲。

AFE输入采用单麦克风格式`M`，首版关闭AEC和双麦算法，接入WakeNet及VAD。模型与AFE的具体配置字段以下载后的固定版本头文件为准。保持手动S1采音格式和现有FIR，不叠加未经验证的增益或降噪。

### 状态与交互

| 状态 | 行为 |
| --- | --- |
| 未配对、断网、音频不可用 | 不启动云端录音流程；不因唤醒触发配对或恢复 |
| 连接就绪且空闲 | 本地监听唤醒词；无持续灯光动画，独立绿灯保持连接语义 |
| 唤醒命中 | 切换录音，显示既有蓝色录音反馈；不播放会污染开头的提示音 |
| 自动录音 | VAD判定有效说话后，连续静音800ms提交；最长15秒 |
| 唤醒后未说话 | 5秒内未检测有效语音则取消，不提交空录音，恢复监听 |
| 识别、等待回复、合成、播放、等待续播 | 暂停唤醒检测，保留按键操作；不让模型抢占播放或重复开启会话 |
| 会话结束/取消 | 清除旧音频和模型积压，条件满足后恢复监听 |
| S2静音 | 停止本地监听及录音，不上传未提交音频，沿用红色静音提示 |

800ms与5秒作为首版工程参数，不增加用户配置项；测试后若需调整，按既有失败后配置变更规则确认。唤醒后的录音总等待上限15秒，包含等待问题开口的时间。VAD预缓存和唤醒后的音频交接必须避免丢失问题开头，也不能将旧会话音频带入新一轮。首版先等待唤醒词尾音结束：说完唤醒词后稍停、看到蓝色录音反馈再说问题，不承诺连说支持，应专门记录连说测试。

初始化缺模型、模型加载失败、AFE创建失败或采音失败时明确报告，不静默忽略初始化失败，也不主动切换备用实现；修复或更换方案先说明。

## 4. Flash和运行资源门禁

使用`esp32/partitions_easyinput_v2_wake.csv`；保持现有NVS、OTA状态、射频分区、两个4MiB应用槽及产品分区的偏移和大小。模型分区标签`model`，起点0x822000，容量640KiB，结束0x8C2000，小于16MiB。“你好小益”及VAD两个模型原文件合计578819字节，按官方打包格式加索引预计579135字节；最终以构建产物为准，不修改其他板共用分区。

新增分区表与模型写入属于本次烧录计划的额外变化，实际产物和精确地址核验后再请求确认；不得直接沿用上次同布局烧录授权。回滚为恢复旧应用和旧分区表，保留NVS；无需擦除新模型区域，但回滚前同样核验目标身份。

测量模型初始化、联网、监听、自动录音、TLS请求、长回复缓存及播放期间的内部RAM/PSRAM空闲量、最大连续块与任务栈。4MiB重播缓存与模型共同占用PSRAM，不能预先保证原131秒容量；如果现有缓存无法保持，需要先评审资源策略，不能静默缩减缓存或加入Flash音频降级。

## 5. 分步验收

1. 固定依赖和模型核验：实际包校验、许可证、头文件/脚本检查、模型产物大小与分区边界；ESP-IDF6.0.1编译成功，固件应用槽余量不少于10%。
2. 单独监听资格测试：记录AFE块长、唤醒词与模型名、实时性、内存和栈；初始化错误可定位，无I2S竞争、零数据或积压。
3. 功能验证：安静环境约0.5米20次唤醒，记录命中次数与延迟；首版目标至少18次，若未达标先定位而不自动调参。唤醒后说话可收到文字及语音回复，静音、S4取消、S1手动入口全部验证。
4. 无说话/截断验证：唤醒后不说话不产生云端提交；短问题开头未截断；最长15秒退出；分别记录唤醒词与问题分开及连说的结果。
5. 并发边界：播放、S3等待续播、S4暂停、断网重连期间无重复会话；S2静音至少20次切换，无继续采集或上传。
6. 稳定性：空闲监听30分钟记录误唤醒次数（安静环境首版目标0次）；连续30轮含长回复，记录内存、错误及重播缓存是否仍满足既有要求。其他噪声/距离结果如实记录，不泛化为准确率承诺。

实际操作、听音与测量按证据填写。依赖下载失败、构建失败或测试目标未达到时，新增安装、配置调整、换模型等步骤先列计划并等待确认。

## 6. 官方参考与本次执行记录

- [组件版本与依赖](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.0/dependencies?language=en)
- [组件介绍与归档大小](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.0/readme?language=en)
- [AFE输入及VAD/WakeNet接口](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)
- [模型分区与加载](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html)
- 官方ESP-SR归档已下载到`esp32/tmp/easyinput/esp-sr-2.6.0.zip`，184421537字节，本地SHA256为`9ccc86cd054768be6aa11535903e94f8f9a6c9fec8d339abc297023a080a8347`。该值用于本地产物追踪，尚未与登记摘要比对，不能单独视为官方校验通过。
- 已检查ESP-SR CMake、`model/movemodel.py`、`model/pack_model.py`和所选版本头文件。模型脚本只读取本地配置、复制模型并打包；重建时删除对应构建目录的`srmodels`子目录。用户已单独允许生成和重建`esp32/build-easyinput-v2-wake/srmodels`。不执行该脚本的其他路径或外部模型目录。
- 已增加`main/voice_wake.c/.h`、特性开关、条件依赖和专用叠加配置，并在`voice.c`接入自动录音、S1接管、静音/播放停止监听。原有灯效优化修改保留，不在本次重新设计。
- 实际下载命令：`compote registry sync --component espressif/esp-dl==3.3.12 --component espressif/dl_fft==0.6.0 --resolution latest ./tmp/easyinput/sr-dependency-review`。下载和缓存位于Workspace；归档与review目录暂保留用于核验，清理前另行确认。
- 实际测试命令：`python -m unittest tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter tests.test_easyinput_buttons tests.test_easyinput_replay_seek tests.test_muse_chat_session`。6项运行，其中5项通过、模型产物检查跳过；另3个测试类因缺少主机编译器跳过，共4项跳过。不把跳过项目视为通过。
- 已执行授权的模型打包命令，使用只包含模型选项的本地审查配置：`python ./tmp/easyinput/esp-sr-2.6.0-review/esp-sr/model/movemodel.py -d1 ./tmp/easyinput/sdkconfig.model-review -d2 ./tmp/easyinput/esp-sr-2.6.0-review/esp-sr -d3 ./build-easyinput-v2-wake`。真实镜像579214字节，SHA256为`af5cc51251a2417a37f4d7a725919d8ca21a1eb492b5bfbfa4686f0923d7590c`。分区测试3项全部通过，确认只有预期的两个模型、文件索引完整且容量足够。
- 全量主机测试命令：`python -m unittest discover -s tests -p 'test_*.py'`。运行129项，1项失败、25项错误、27项跳过；失败和错误均指向缺少主机C/C++编译器，未安装额外编译器。完整日志`esp32/tmp/easyinput/wake-host-tests.log`，不能声明全量测试通过。
- 用现有构建的参数和同一ESP-IDF交叉编译器，启用离线唤醒宏，对`voice.c`和`voice_wake.c`执行`-fsyntax-only`检查，两文件通过。审查辅助脚本及日志位于`esp32/tmp/easyinput/`，未安装新工具；它不替代最终依赖解析、链接或硬件测试。
- 官方归档保留其`ESPRESSIF MIT License`及说明文件。该许可针对Espressif产品，第三方源码与预编译库未改许可证或重新标记为本工程Apache代码。
- 附属依赖下载成功；同步工具还自动下载了dl_fft 0.8.0，但工程实际固定0.6.0。esp-dl 3.3.12的480个文件、dl_fft 0.6.0的29个文件和esp_new_jpeg 1.0.2的34个文件均已与官方CHECKSUMS清单逐文件核对SHA256和大小。
- ESP-SR 2.6.0的964个文件已与官方CHECKSUMS清单逐文件核对SHA256和大小。为复用已下载内容，将核验后的文件重打包为官方目录格式的Workspace本地组件镜像，归档191099749字节；重新压缩改变归档摘要，不改变组件文件内容。上述原归档SHA256仍仅用于追踪原下载包。
- 已审查附属组件CMake及ESP-DL `cmake/compile_finalize.py`。生成脚本只使用Python标准库，读取本地算子规格并在构建目录写头文件/源文件清单，无新增网络请求、pip安装或删除步骤。用户已允许执行该脚本并完整编译。
- 新镜像的空白分区和头部索引加载前检查已加入，避免官方加载器直接信任未写入的Flash内容。最终编译仍需核验实际模型镜像、应用槽余量及依赖锁；硬件监听、内存和准确率验收尚未执行。
- 完整构建命令已执行，退出码2：源码和启动程序编译通过，应用最终链接失败。ESP-SR预编译`libwakenet.a`引用`vtable for dl::audio::SpeechFeatureBase`，ESP-DL当前库未提供该符号；日志`esp32/tmp/easyinput/wake-build.log`。尚无可用于烧录的新应用镜像。
- 临时样例使用同一交叉编译器和实际ESP-DL编译参数，未修改工程配置：原`dl_fbank.cpp`在原优化参数及关闭devirtualize时均不生成基类虚函数表，`-O0`生成；单独显式构造Fbank的样例在原优化参数下即可生成该表。诊断脚本、样例与对象均位于`esp32/tmp/easyinput/`。这说明可用独立链接兼容文件提供符号，避免整体降低DSP优化级别；完整链接及运行效果仍待确认后的验证。
- 用户已确认兼容修复方案。仅离线唤醒构建增加`main/voice_wake_link_compat.cpp`，使用官方头文件让编译器生成所需虚函数表，辅助构造函数不在运行时调用；不改第三方源码、不更换版本、不修改全局配置。按同一构建命令重新编译，日志`esp32/tmp/easyinput/wake-build-compat.log`；最终链接后需检查辅助函数已被移除、所需虚函数表保留。
- 兼容文件首次接入后的链接仍失败，但对象符号检查确认基类虚函数表已生成：主程序静态库中的该对象没有被链接器取出。已在`voice_wake_init()`引用兼容文件内的空函数，使该对象被纳入链接；空函数仅初始化时调用一次，不分配音频资源，辅助Fbank构造函数仍不调用。按原方案修正源码引用后再次构建，日志`esp32/tmp/easyinput/wake-build-compat-anchor.log`。
- 最终完整构建退出码0，生成应用镜像2101248字节（0x201000），4MiB槽剩余2093056字节，约49.90%，通过至少10%余量门禁。应用SHA256为`f97fabf2c355e14062ae64a3b09ba083c4894e37251fe0c5617547c08e1a81d6`；模型仍为579214字节，SHA256与前述核验值一致。
- 实际最终符号检查：`xtensa-esp32s3-elf-nm.exe -C build-easyinput-v2-wake/muse-gadget.elf`。基类虚函数表存在，空引用函数存在，辅助构造函数`emit_speech_feature_vtable`已从最终ELF移除；不会在启动时构造Fbank或额外分配其音频内存。
- 实际最终测试：`python -m unittest tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter`，6项全部通过；`git diff --check`通过。全量主机测试缺少编译器的限制仍存在，不声明全量通过。
- `dependencies.lock`核实实际固定版本：ESP-SR 2.6.0、ESP-DL 3.3.12、dl_fft 0.6.0、esp_new_jpeg 1.0.2、IDF 6.0.1；未引入ESP-DSP。锁文件和管理组件目录沿用项目忽略规则，未提交本地配置或凭据。
- 最终`flash_args`仅核验、尚未执行：启动程序0x0，分区表0x10000，OTA状态0x1D000，射频初始化0x1F000，应用0x20000，模型0x822000。计划不写NVS；实际写入前仍需核验串口目标、读取现有分区并取得新增模型及分区表烧录授权。
- 用户随后选定“你好小益”。切换为现有`wn9_nihaoxiaoyi_tts2`模型，同步更新加载前名称校验、初始化筛选、就绪日志、板型叠加配置和模型测试；旧模型及二进制摘要的记录仅对应先前“Hi，乐鑫”构建，不作为本轮产物标识。
- 使用已安装的官方`python -m kconfserver`工具，对现有生成配置仅设置`SR_WN_WN9_HILEXIN=false`及`SR_WN_WN9_NIHAOXIAOYI_TTS2=true`并保存；工具输出丢弃，不查看凭据字段。随后复用既有ESP32-S3构建目录执行`python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake build`，日志`esp32/tmp/easyinput/wake-build-xiaoyi.log`。沿用已授权的模型目录重建脚本，不新增依赖、不改变分区容量、不烧录。
- “你好小益”版完整构建退出码0。应用2101248字节，SHA256为`2cc1c6a8ac77742896514be0604edcb6bdd78ba7a72af5d69644b91861093fe9`；模型579135字节，SHA256为`b099ae230940f66690f936973eb0ae3e9cf3210a9c310527a186753f42e6d007`。应用槽剩余约49.90%；模型保持640KiB容量及0x822000地址。
- 本轮`python -m unittest tests.test_easyinput_wake_partition tests.test_easyinput_audio_filter`的6项测试全部通过，实际镜像只含`wn9_nihaoxiaoyi_tts2`和`vadnet1_medium`，索引与容量正确。最终ELF保留所需基类虚函数表及空引用函数，不含辅助构造函数；`git diff --check`通过。全量主机测试限制及硬件验收未完成状态不变。
- 用户确认新增分区及模型烧录后，重新核验COM6为ESP32-S3 revision v0.2、MAC `c4:9e:7e:07:9f:d8`。只读保存原分区表到`esp32/tmp/easyinput/pre-wake-partition-20261011.bin`，确认原分区地址全部保持不变、仅追加model区。
- 首次烧录使用现有ESP-IDF环境的`python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep`，地址及文件对应前述flash_args；退出码0，各写入数据校验通过。未整片擦除、未写NVS、未修改eFuse。日志`esp32/tmp/easyinput/wake-xiaoyi-flash.log`。
- 烧录后最后一次ROM身份核验MAC一致，自动复位后仅用有界串口采集器读取日志；未要求按BOOT。启动日志`esp32/tmp/easyinput/wake-xiaoyi-boot.log`已在写入前过滤凭据相关行。应用启动后重复触发`esp_task_stack_is_sane_cache_disabled()`断言，不能视为验证成功。
- 使用同一ELF的`xtensa-esp32s3-elf-addr2line.exe -pfiaC`解码：`voice_task → voice_wake_init → check_model_image → esp_partition_read → esp_flash_read → spi_flash_disable_interrupts_caches_and_other_cpu`。该语音任务沿用PSRAM栈，模型Flash读取关闭缓存时访问外部栈被SDK拒绝。
- 修复仅在`CONFIG_HOMEHUB_OFFLINE_WAKE`开启时，将语音任务4KiB栈改用内部RAM；未开启唤醒的版本保留PSRAM栈，音频缓冲仍在PSRAM，未改模型、分区或阈值。按原构建命令编译退出码0，6项测试通过。修复应用2101248字节，SHA256 `b9ca90f33260da75c1046c4e648e47725157a1f6632656e6542d6c7298c420ec`，日志`esp32/tmp/easyinput/wake-build-internal-stack.log`。
- 用户重新连接开机后，再次验明同一MAC；沿已授权烧录范围，仅向0x20000写入修复应用，其余区域不重复写入。命令参数同前，仅保留应用地址及文件；退出码0、数据校验通过，日志`esp32/tmp/easyinput/wake-internal-stack-flash.log`。最终恢复前再次核验MAC一致，随后零次调用ROM识别，只采集启动日志`esp32/tmp/easyinput/wake-internal-stack-boot.log`。
- 修复后的45秒有界采集命中`offline wake ready: 你好小益`及`link.voice: ready`，AFE正确加载`wn9_nihaoxiaoyi_tts2`和`vadnet1_medium`，feed/fetch均512采样；观察窗口内没有断言、panic或重启循环。初始化时PSRAM空闲6029308字节、最大连续块5898240字节；42秒心跳显示内部RAM约48KiB、最大连续块42KiB，PSRAM约5887KiB。这仅是断网启动状态，不能替代TLS与长回复缓存压力验收。
- 网络扫描发现23个AP，但报告`no saved wifi network nearby`，心跳wifi/ws/raw均down；已向用户请求恢复原2.4GHz网络/热点，并开始180秒有界采集`esp32/tmp/easyinput/wake-xiaoyi-network-test.log`。没有读取Wi-Fi凭据、没有擦除NVS或自动重新配网。未联网时按首版设计不启动唤醒录音，功能测试暂待网络恢复。
- 用户反馈恢复原网络、独立绿灯常亮。原采集因USB重新连接中断，fresh串口枚举仍为COM6，USB序列号与先前已验明设备一致；重新开启180秒有界采集`esp32/tmp/easyinput/wake-xiaoyi-functional-test.log`，不调用ROM识别、不复位。新日志心跳确认wifi/ws/raw均up，正在处理一轮Muse文字回复及Qwen TTS合成。已请求用户等播放完成后，在约0.5米处不按S1，用“你好小益”唤醒并提问“请只回答你好”，确认蓝色录音反馈、自动提交、文字及扬声器输出。
- 用户回复“唤醒、文字和语音都正常”，作为本轮不按键唤醒与自动提交的现场观察证据。当前串口采集覆盖了既有一轮回复：Qwen合成及下载HTTP 200，扬声器输出2234880帧、峰值19199，播放46.6秒后恢复`listening locally`，重播缓存1489920字节。未捕获完整`wake detected`及该短句录音提交链路，不把用户观察冒充完整日志证据。
- 联网及该轮TTS期间观测内部RAM最低约14KiB、最大连续块11KiB；播放结束并恢复监听后内部RAM约26KiB、最大连续块19KiB，PSRAM约3513KiB（含已保留重播缓存）。观察窗口未出现panic或断言。仅一轮功能反馈及这段资源观测，不能推导20次唤醒成功率、30分钟零误触发、20次静音切换或30轮长回复稳定性已通过。
- 本次原ESP-SR归档、解压审查目录和组件本地镜像分别184421537、332387939、298530356字节，合计约778MiB，不含管理组件目录和完整构建产物。暂保留审查资料，不自动删除。

### 6.1 长回复与重播资源验证（2026-10-11）

- 用户连接设备后，使用已有环境执行 `python -m serial.tools.list_ports -v`，确认COM6的USB序列号与已验明设备一致；执行 `python ./tmp/easyinput/capture-asr.py COM6 600 tmp/easyinput/wake-memory-long-reply-test.log` 有界采集。不复位、不编译、不烧录，不安装依赖或访问外部域名；日志写入前过滤凭据相关行。
- 日志记录离线唤醒、自动录音6.34秒、Qwen ASR HTTP 200，以及4段TTS合成和下载HTTP 200。首次完整播放输出5472000帧，按48kHz输出计算约114秒，保留3648000字节16kHz重播音频（约3.48MiB），随后恢复本地监听。
- S3从0秒开始重播，S4在9.60秒暂停；随后S3续播并推进到14.22秒，再次暂停后S3从14.22秒继续。用户确认完整播放、重播、续播和再次唤醒均正常；本轮截至记录时的日志尚未覆盖最终再次唤醒，不将现场反馈写成完整日志证据。
- 心跳采样显示内部RAM最低约14KiB、最大连续块12KiB，PSRAM最低约1168KiB；缓存保留后PSRAM约1404KiB。观察记录中未出现内存分配失败、缓存被释放、panic或重启。心跳为周期采样，不能代表瞬时最低值；一轮通过不替代30轮稳定性及超过4MiB的边界验收。
- 闲时多次出现 `AFE: Ringbuffer of AFE is empty`。本轮唤醒成功，但需进一步核对采音与fetch等待节奏，尚未确定根因，不仅屏蔽日志、不据此调整唤醒阈值。本轮不修改固件代码、缓存容量或模型参数；采集日志保留在Workspace，无新增清理操作。

### 6.2 AFE读取等待修正（2026-10-11）

- 根据实际feed/fetch均512采样、16kHz采样率，每块音频时长为32ms；原两处 `fetch_with_delay` 仅等待30ms，比整块采音时间短，可能在正常采音时反复超时。此为代码与现场告警支持的原因判断，仍待修复版硬件对照验证。
- 仅在 `esp32/main/voice_wake.c` 增加共用100ms等待常量，应用于闲时唤醒与自动录音两处读取；保留500ms录音读取总等待窗口、现有模型/VAD阈值、4MiB缓存和采音任务节奏。不屏蔽AFE日志。
- 复用原SDK、构建目录及本地依赖镜像，执行 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake build`，日志为 `esp32/tmp/easyinput/wake-fetch-wait-build.log`，退出码0。应用2101248字节，SHA256 `11442d50e5eb77817ce168f30f485e28907b58b4041e9be24d467892bdac22c1`。模型分区及音频滤波6项测试通过，`git diff --check`通过。
- 停止前轮有界串口采集以释放端口；重新枚举COM6并用 `python -m esptool --chip esp32s3 --port COM6 --connect-attempts 2 --before default-reset --after hard-reset chip-id` 核验ESP32-S3 v0.2、MAC `c4:9e:7e:07:9f:d8`，退出码0。该身份核验会复位设备，未写Flash。修复版尚未烧录，告警消除、唤醒和按键响应待现场验证。
- 本轮无新增依赖、外部域名访问、全局配置修改或清理操作。后续拟仅更新0x20000应用，待用户确认，不更新分区、模型或NVS。

### 6.3 读取等待修复版烧录（2026-10-11）

- 用户明确确认烧录后，重新枚举COM6并用前述chip-id命令核验同一ESP32-S3、MAC。执行 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin`，日志 `esp32/tmp/easyinput/wake-fetch-wait-flash.log`，退出码0，写入2101248字节、数据Hash校验通过。仅写应用，不写模型、分区、NVS或eFuse。
- 烧录后重新枚举串口，以已有过滤采集器执行 `python ./tmp/easyinput/capture-asr.py COM6 120 tmp/easyinput/wake-fetch-wait-runtime.log`。日志命中 `offline wake ready: 你好小益`、`link.voice: ready`，模型feed/fetch均512。
- 日志同时记录Muse HTTP 401、`connect failed`、`deferred setup reset - reset setup`及软件重启；随后启动显示 `no wifi creds, waiting for BLE provision`。固件自身认证失败处理触发了设置重置，不能将本轮烧录未写NVS等同于运行后配网始终保留。已请用户恢复配对和网络；未读取凭据、未修改认证逻辑。
- 离线监听须联网就绪，当前不能以未出现AFE空缓冲告警判定修复有效。等待恢复网络后再观察闲时告警、唤醒、自动录音与按键响应。无新增依赖、外部域名访问或清理操作；设备运行中的Muse服务访问属于既有固件行为。

### 6.4 配对内存占用修复（2026-10-11）

- 用户提供App无法连接截图后重新采集 `esp32/tmp/easyinput/wake-fetch-wait-pairing.log`：蓝牙连接及配对握手已到 `pairing_client_finished`，随后 `failed to start pairing confirmation timer`、`error_pairing_unavailable`。代码 `start_confirm_timeout` 使用内部RAM创建4096字节栈任务；配对空闲内部RAM约9KiB、最大连续块4KiB，配对回调自身还占用任务栈，资源不足与创建失败一致。
- 用户允许调整并编译刷机验证。仅改 `esp32/main/voice.c`，移除开机立即加载ESP-SR模型，在语音任务空闲分支、Muse连接就绪且蓝牙控制器状态为IDLE时首次初始化；初始化成功后仅执行一次。通过真实控制器状态等待蓝牙关闭，模型仍在内部栈语音任务中读取Flash。保留100ms AFE读取等待、原模型、手动按键录音、VAD与重播缓存配置。
- 原构建入口编译退出码0，日志 `esp32/tmp/easyinput/wake-deferred-init-build.log`；应用2101248字节、SHA256 `c4c08f69e2b5519590bdf91c2f26d045c2541900e6b375d1c13746745ebe7e9d`。6项模型分区与滤波测试、`git diff --check`通过，未新增依赖、联网下载或系统配置变更。
- 停止上一轮串口采集，重新枚举并核验COM6、ESP32-S3 v0.2、MAC一致；使用6.3相同write-flash参数，仅写0x20000应用，日志 `esp32/tmp/easyinput/wake-deferred-init-flash.log`，退出码0、数据Hash校验通过，不写NVS/分区/模型/eFuse。
- 重新枚举后执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-deferred-init-validation.log`。启动配对等待阶段内部RAM约51KiB、最大连续块31KiB，PSRAM约6552KiB；较提前加载模型时的9KiB/4KiB明显提高。已请求用户重新配对、配网及唤醒短句验证。当前记录时尚未确认端到端配对及唤醒结果，不宣称修复验收完成。

- 后续现场日志确认 `confirm_required` → S8短按 → `pairing_confirmed`，Wi-Fi连接成功，两次VM API请求均HTTP 200。但 `noise_ctrl_connect` 创建12KiB内部栈的 `noise_ctrl` 任务失败，配网流程将此失败统一报告为 `auth_failed` 并清理未完成状态；不能以该状态判定密钥无效。用户反馈仍无法完成配网。配对阶段内存修复已有效，端到端仍未通过。
- `main/noise_control.cpp` 已有为12KiB任务栈在启动时预留内部内存的逻辑，但仅在 `CONFIG_MUSE_ENABLED && CONFIG_SPIRAM` 开启；当前EasyInput语音构建未启用MUSE_ENABLED，因此该预留未生效。下一步拟将已有预留逻辑覆盖离线唤醒构建，不缩小栈、不将协议栈迁至PSRAM；本轮失败后按用户规则先申请方案确认，尚未修改。

### 6.5 Muse连接栈预留修复（2026-10-11）

- 用户确认后，仅将 `esp32/main/noise_control.cpp` 原内部栈预留条件扩展为 `(CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_OFFLINE_WAKE) && CONFIG_SPIRAM`。复用启动时预留及创建任务前释放的既有逻辑，不缩小12KiB任务栈、不修改认证处理或引入PSRAM协议栈。
- 按前述原构建入口执行build，退出码0，日志 `esp32/tmp/easyinput/wake-noise-reserve-build.log`；应用2101248字节，SHA256 `fd20702fbbe9f2592dcff70f97c5ee738622d2bdb3240b012f7853691c348cbf`。6项现有模型分区与滤波测试、`git diff --check`通过。
- 停止前轮有界采集；重新枚举COM6并使用前述chip-id参数验明ESP32-S3 v0.2及同一MAC。沿用6.3的write-flash命令，仅写0x20000应用，日志 `esp32/tmp/easyinput/wake-noise-reserve-flash.log`，退出码0、数据Hash校验通过。分区、模型、NVS及eFuse未写入；无新增依赖、下载、全局修改或清理操作。
- 重新枚举后执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-noise-reserve-validation.log`，已请用户重新配对、配网及唤醒短句验证。记录时尚未取得端到端结果，仍不能宣称本轮配网或AFE告警修复验收完成。

- 6.5现场后续记录：S8确认、Wi-Fi连接成功，但VM阶段TLS失败，`mbedtls_ssl_setup returned -0x008D`、`create_ssl_handle failed`，请求未获得HTTP响应。心跳一度内部RAM5KiB/最大块3KiB、DMA0KiB。启动栈预留减少后续任务碎片风险，却使联网握手阶段内部可用资源更紧张，本轮端到端仍未通过。
- 已只读核对现有SDK及生成配置：mbedTLS已设置外部PSRAM分配，因此不再建议重复开启此配置。通用malloc阈值仍为 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`，较大的普通分配会优先使用内部RAM。拟在离线唤醒overlay将此阈值改为4096后进行配网实测，是否可消除TLS失败尚待验证；失败后换配置方案先请求用户确认，尚未实施。不增加Wi-Fi/LwIP迁移或关闭硬件加密等额外变化。

### 6.6 唤醒版普通分配阈值调整（2026-10-11）

- 用户允许后，仅在 `esp32/devices/sdkconfig.easyinput-v2-wake` 增加 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，原默认16384仍保留给其它构建。模型、12KiB连接栈预留、100ms AFE等待和延后模型初始化保持前述实现，不额外迁移Wi-Fi/LwIP或修改认证逻辑。
- 为避免已生成sdkconfig覆盖新的默认值，在原构建目录使用新的Workspace内配置路径，实际命令 `python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake -D SDKCONFIG=tmp/easyinput/sdkconfig.wake-memory-4k build`，仍加载原默认、EasyInput板型、本地凭据与唤醒overlay。凭据由构建工具消费，不读取或输出配置正文。日志 `esp32/tmp/easyinput/wake-memory-4k-build.log`；本条记录时编译尚未完成，待完成后核对生成阈值、产物及设备实测。
- 6项现有模型分区与滤波测试、`git diff --check`通过，未新增依赖、联网下载、全局修改或清理操作。新配置触发已有已审查且已授权的本地模型打包/ESP-DL生成步骤；不执行新第三方脚本。

### 6.7 评审后撤回预留并验证（2026-10-11）

- 用户要求整体评审后暂停了6.6版本烧录，评审见 `docs/specs/easyinput-v2-review-20261011-1830.md`。6.6编译实际退出码0、阈值4096生效，应用2101248字节，SHA256 `f7583cf014f87410cd1aea98134a6a163928d046f89124eac246235617a2e83d`；该版本未烧录。
- 用户要求继续后，撤回 `main/noise_control.cpp` 新增预留条件，恢复原 `CONFIG_MUSE_ENABLED && CONFIG_SPIRAM`。保留延后模型初始化、100ms AFE等待及4096实验阈值；这是撤回有回归证据的预留后验证组合，不声称只改变单个因素即可归因所有效果。
- 执行6.6相同SDKCONFIG路径的build，日志 `esp32/tmp/easyinput/wake-no-reserve-4k-build.log`，退出码0；应用2101248字节，SHA256 `bcc3b6c942b9bfbeb352f22640b863bc52cd85968a2e07970c01d1112bf2bc1d`，生成头文件阈值4096。6项现有测试及 `git diff --check`通过。
- 重新枚举COM6并核验同一ESP32-S3 v0.2、MAC；按前述write-flash命令只向0x20000写入应用，日志 `esp32/tmp/easyinput/wake-no-reserve-4k-flash.log`，退出码0、Hash校验通过，不写分区/模型/NVS/eFuse。无新增依赖、外部下载、全局修改或清理。
- 重新枚举后执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-no-reserve-4k-validation.log`，已请用户配网和唤醒短句验证。记录时尚未取得端到端结果；已有配对重启与长回复回归也仍待验证。

- 6.7后续日志确认S8配对、Wi-Fi、两次VM HTTP 200、Noise控制连接、注册及隧道均成功，BLE完成关闭；心跳wifi/ws/raw均up，内部RAM约73KiB/最大块31KiB、DMA约65KiB。本轮全新配网路径越过了此前两类资源失败，但不能将组合成功单独归因于某一参数。
- 联网后当前日志尚未命中模型初始化/监听。代码复核发现 `voice_hatch_refresh()` 仅在启动及S1就绪检查时执行，首次开机未配对时缓存 `s_linked=false`，配网成功后该缓存未刷新；延后初始化以 `muse_hatch_ready()` 为门禁，因此新配网可卡住自动监听。已请用户先用S1验证缓存刷新及随后唤醒，尚未修复，不称端到端完成。已有配对重启及长回复回归仍待测试。

- 6.7后续日志复现状态刷新问题：S1录音2.3秒、ASR/TTS HTTP 200、扬声器输出后，立即命中模型初始化及 `listening locally`；其后两次 `wake detected`、自动录音1.2/2.7秒、ASR/TTS HTTP 200、扬声器输出及恢复监听。当前观察记录未见AFE空缓冲告警；仍需用户确认实际听感，不能把短观察当长期验收。新配网后必须先S1触发缓存刷新是已确认功能缺陷，不作为正常交互保留；拟在配网成功提交设置后刷新语音状态，待下一步处理。

### 6.8 配网后语音状态刷新（2026-10-11）

- 用户要求补充修复，在 `main/app.c` 的语音条件编译中包含 `voice_muse_chat.h`，配网提交设置后、关闭BLE前调用既有 `voice_hatch_refresh()`。通过内部栈配网任务读取设置，原延后模型初始化继续等BLE控制器关闭，不移回PSRAM栈读Flash，不改变认证策略。
- 沿6.7构建配置执行build，日志 `esp32/tmp/easyinput/wake-pair-state-build.log`，退出码0。应用2101248字节，SHA256 `278e2396a4fea784324ba79f6d449c763043a557ddf75280e06f57937195fab5`；6项现有测试及 `git diff --check`通过。
- 重新枚举并验明COM6、同一ESP32-S3/MAC后，仅写0x20000应用，日志 `esp32/tmp/easyinput/wake-pair-state-flash.log`，退出码0、数据Hash校验通过；未写NVS/模型/分区/eFuse。无新增依赖、联网下载、全局配置修改或清理操作。
- 执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-pair-state-validation.log`，已配对启动恢复Wi-Fi，控制连接up。启动阶段出现一次AFE空缓冲告警，继续观察，不能报告零告警。全新配网验证需重置现有配置，已单独向用户请求选择；未收到允许前不要求或执行重置。当前未完成新配网后无需S1的现场验收。

- 6.8继续观察：多个远端域名 `getaddrinfo() returns 202` 导致控制连接重试，约220秒恢复控制连接、注册、隧道及Muse聊天订阅。期间内部RAM约35–43KiB、DMA约34–41KiB，未见本轮先前的 `mbedtls_ssl_setup -0x008D`；域名解析失败不直接归因于内部栈不足。恢复连接期间又出现几次AFE空缓冲告警，100ms修正不能报告完全消除告警，需进一步观察调度与feed时序。本轮待用户选择是否重置配置完成全新配网验收，未主动重置。

### 6.9 全新配网直接唤醒验收（2026-10-11）

- 用户明确允许长按S8重置现有Wi-Fi/配对设置后重新配网。重新枚举COM6，以已有过滤采集器执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-fresh-pair-validation.log`；无ROM识别、自动复位、刷写或读取凭据。用户按要求自行重置和配网。
- 日志依次命中配对确认、Wi-Fi、两次VM HTTP 200、`setup stage: vm -> done`。BLE在41130ms关闭释放内存，模型在41257ms就绪、41258ms开始监听；本轮未见S1输入。54685ms唤醒，自动录音3.4秒，ASR、TTS合成/下载均HTTP 200，扬声器输出46080帧后恢复本地监听。
- 用户确认“配网后直接唤醒，文字和语音都正常”。新配网无需S1刷新状态的缺陷已通过本轮端到端验收；原配对确认任务、Noise控制任务及TLS内存分配失败在本轮未出现。VM阶段心跳内部RAM最低约19KiB、最大块7KiB、DMA约11KiB；资源仍需长期压力验证，不能凭单轮推导普遍无故障。
- 新固件为6.8的SHA256 `278e2396a4fea784324ba79f6d449c763043a557ddf75280e06f57937195fab5`：保留延后模型初始化、配网成功刷新、4096普通分配阈值、100ms AFE等待；新增12KiB栈预留已撤回。组合通过不等同各因素单独归因完成。
- 建立控制连接阶段出现两次AFE空缓冲告警（44601/45778ms），之后完成唤醒与播放。告警未完全消除，不以屏蔽日志作为修复。当前只完成已有配对启动和一轮全新配网/短句；最新组合长回复/重播、连续唤醒、误触发与静音切换资格测试尚未完成。
- 用户反馈后停止该串口采集释放端口。更新规格及评审文档，无新增依赖、域名访问、全局修改或清理操作。

### 6.10 唤醒困难基线复测（2026-10-11）

- 用户反馈近距离需重复说词，初步自报5次成功3–4次。重新枚举COM6后执行 `python ./tmp/easyinput/capture-asr.py COM6 180 tmp/easyinput/wake-sensitivity-baseline.log`，仅只读采集，不复位、刷机或修改配置。
- 采集开头覆盖既有长回复的尾段，日志记录回复整轮183.3秒、朗读11.8秒，之后才进入 `listening locally`。不能将设备正在等待/播放回复期间的尝试当作模型漏检；首轮用户观察与日志不能逐次对应，因此不计算严格漏检率。
- 确认本地监听后，请用户在约0.5米安静环境、正常音量只说唤醒词5次、每次间隔约10秒、不接问题或按键。用户反馈5次都成功；日志亦捕获5次 `wake detected`，前4次完整记录无问题5秒取消并恢复监听，第5次已检出但采集窗口结束未覆盖后续恢复。此段未出现AFE空缓冲告警，不推广为长期零告警。
- 只读模型信息包含 `wakenet9l_tts2h12_你好小益_3_0.635_0.638`；查阅当前ESP-SR接口确认可以设置检测阈值。曾提出单因素试调0.60，但复测5/5后未实施，原模型阈值、采音增益、AGC及固件保持不变，避免无证据增加误唤醒风险。
- 仍需区分连接就绪与监听就绪：当前独立绿灯表示Muse连接，等待回复、播放、暂停及静音等状态按首版设计不监听。下一步建议评审明确的监听就绪反馈，再决定远场/噪声下是否需要阈值或采音优化。当前短复测不能证明所有环境均可靠，也不能断言最初失败全部由非监听状态造成。
- 采集已结束释放端口；本轮仅更新规格，无新增依赖、外部域名访问、全局修改或清理操作。

### 6.11 恢复监听灯光提示（2026-10-11）

- 用户要求继续优化后，在 `main/led_status.h/.c` 增加EasyInput监听开始/停止通知，在 `main/voice_wake.c` 实际启动采音及AFE feed后触发，停止监听时取消。提示为中间一颗低亮度青色灯800ms半正弦淡入淡出，仍经既有整体1/4亮度缩放。
- 独立于语音主状态保存短暂截止时间，仅在语音IDLE且无更高优先级灯效时显示，不覆盖静音/错误、录音、回复、重播等待或音量提示；不改阈值、增益、AGC、配网、内存策略或音频数据。实际唤醒/回复时既有灯效接管，不延迟识别或等待动画结束。
- 按当前SDKCONFIG路径编译，日志 `esp32/tmp/easyinput/wake-ready-light-build.log`，退出码0；6项现有模型分区及滤波测试、`git diff --check`通过。这些测试不验证实体灯光可见度。未增加依赖、联网下载、全局修改或清理操作；当前尚未烧录，灯光观感及打断/静音现场验证待完成。

- 用户明确要求“烧录 开始验证”后，重新枚举COM6并核验ESP32-S3 v0.2、MAC `c4:9e:7e:07:9f:d8`。仅向0x20000写入应用，实际命令 `python -m esptool --chip esp32s3 --port COM6 -b 460800 --connect-attempts 2 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size keep 0x20000 build-easyinput-v2-wake/muse-gadget.bin`；退出码0、Hash校验通过。应用2101248字节，SHA256 `9789482641e2d6c414e09300e131e66285a52804e1f188cb3f15c3fac4b23b74`，日志 `esp32/tmp/easyinput/wake-ready-light-flash.log`。不写分区、模型、NVS或eFuse。
- 自动复位后重新枚举并执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-ready-light-validation.log`。当前日志确认Wi-Fi、Muse控制连接及隧道恢复，心跳wifi/ws/raw均up；连接建立阶段仍出现AFE空缓冲告警。已请用户验证短句唤醒、回复后青色提示、再次唤醒及S2静音切换，现场结果待反馈，尚不称灯光或交互验收通过。无新增依赖、联网下载、全局修改或清理操作。

- 本轮用户反馈上述两项“符合预期”，确认回复结束后的柔和青色提示、再次唤醒及静音红灯/解除静音提示符合现场预期。日志记录185979ms唤醒，录音3.7秒，ASR HTTP 200识别“请只回答你好”，TTS合成及音频下载HTTP 200，扬声器输出38400帧，204172ms恢复本地监听；234025ms S2开启静音并停止监听，235545ms解除静音、235554ms恢复监听。日志证明状态流转，实体灯光观感以用户反馈为据；本轮短句与静音切换通过，不扩大为远场、噪声或长期稳定性验收。采集停止释放串口，保留日志，无清理操作。

### 6.12 一米距离与背景声复测（2026-10-11）

- 用户要求继续验证，复用esp-idf-cy串口诊断流程；激活既有当前进程环境，执行 `python -m serial.tools.list_ports -v`，确认COM6及USB序列号对应既有设备，不调用ROM识别或复位。执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-distance-baseline.log`，过滤敏感日志后写入Workspace。
- 安静环境、约1米、正常音量、只说唤醒词5次且间隔约10秒，用户反馈每次都亮灯；日志455032/465463/475500/485547/494638ms记录5次唤醒，每次无问题自动结束并恢复监听。本轮安静一米条件下5/5通过，不推广为更远距离或所有环境效果。
- 随后请求同距离背景声5次及30秒不说词观察，用户仅反馈“符合预期”，未给出具体次数、声源类型及误触发次数。日志572070/581922/619175ms仅记录3次唤醒，其中一次进入回复流程，测试期间存在非监听窗口；不能认定严格5/5或零误唤醒。背景声效果保留为用户定性反馈，完整统计仍待受控复测。
- 本采集窗口未命中AFE告警，心跳网络持续up；不推导长期零告警。本轮不修改阈值、增益、模型或固件，不编译、不烧录、不新增依赖、不联网下载、不修改全局配置、不清理文件。人工结束采集释放串口，保留日志；本次只补充该规格记录。

### 6.13 持续背景声误唤醒观察（2026-10-11）

- 提交代码后，重新枚举COM6确认既有设备USB序列号，执行 `python ./tmp/easyinput/capture-asr.py COM6 300 tmp/easyinput/wake-false-trigger-baseline.log`。请用户保持空闲、未静音，开启日常音量背景声约3分钟，不说唤醒词、不按键，观察蓝色录音灯及自动回复。
- 用户确认“没有误唤醒”；超过3分钟的采集窗口未记录 `wake detected`，心跳wifi/ws/raw持续up，内部可用RAM约36KiB、最大块18KiB、PSRAM约4961KiB。本轮短时误唤醒观察通过；背景声类型与声压未提供，不推导所有声源或长期零误唤醒，也不补算6.12噪声环境的唤醒成功率。
- 保留原模型、阈值及采音参数，不改代码、不编译、不烧录。人工停止采集释放串口，日志保留在Workspace，不新增依赖、访问外部域名、修改全局配置或清理文件。本条仅补充验证记录。

## 7. 独立构建入口

原`devices/sdkconfig.easyinput-v2`不默认开启唤醒。首版在其后加载本地token、Qwen配置和`devices/sdkconfig.easyinput-v2-wake`，使用独立的`build-easyinput-v2-wake`目录，不复用旧构建目录的生成配置。

附属依赖构建脚本审查完成并获授权后，实际执行：

```powershell
# 在esp32目录，仅激活已有SDK的当前进程环境。
. ./tmp/easyinput/env.ps1
$env:SDKCONFIG_DEFAULTS = 'sdkconfig.defaults;devices/sdkconfig.easyinput-v2;tmp/easyinput/sdkconfig.token.local;tmp/easyinput/sdkconfig.qwen.local;devices/sdkconfig.easyinput-v2-wake'
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file:///D:/Project/codex/muse-gadget-sdk/esp32/tmp/easyinput/sr-dependency-review/'
python "$env:IDF_PATH/tools/idf.py" -B build-easyinput-v2-wake -DIDF_TARGET=esp32s3 build
```

不查看本地凭据文件正文、不将生成配置纳入Git。只有最终编译通过、地址和目标身份核验完成，并获新的烧录授权后，才写设备。

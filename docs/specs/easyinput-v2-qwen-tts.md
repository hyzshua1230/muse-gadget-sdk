# EasyInput V2：Qwen 回复朗读

## 配置与范围

采用北京地域的qwen3-tts-flash、Cherry音色、Chinese语种。
本机配置为esp32/tmp/easyinput/qwen-tts.local.json，构建overlay为sdkconfig.qwen.local；均被Git忽略。
API Key只由本机程序用于鉴权和构建，不输出、不提交。启用HOMEHUB_QWEN_TTS，仅影响EasyInput语音后端。
固件包含该Key，仅用于自己的设备，不公开分发。

回复消息完成后，后台任务调用Qwen HTTP接口，取得WAV并送入原有播放器。
当前长回复修复见 [长等待与长回复语音规格](easyinput-v2-long-reply.md)。首版先取得整段音频再播放；超过600字符的文本按UTF-8字符切分调用（历史设计，现已替换为200字符分段播放）。
沿用会话已有的文本缓存上限，不承诺读取缓存以外的长回复全文。
S1打断播放并开始下一次录音，S2静音并取消，S4取消；网络请求至多等待10秒才释放，但已取消音频不会提交到播放器。
合成、下载、格式或内存失败显式报错，不切换其他服务。

## 音频与资源

本次实际接口输出为24kHz单声道PCM16 WAV。
WAV的data长度字段为占位值，因此按HTTP完整接收的实际字节数解析。
固件验证PCM格式、采样率、声道和边界后，复用原有重采样器转为16kHz播放。
只通过HTTPS请求百炼和返回的aliyuncs.com音频地址；不跟随重定向，不把API Key发给音频下载地址。
当前每段下载/PCM限制4MiB，首次朗读不再限制整条累计PCM；完整重播缓存仍限4MiB，来自PSRAM。
长回复的峰值内存和首音延迟尚需实机验收。

## 接口验证记录

用户确认剩余额度大于零，并授权一次短句试音。
请求文本：你好，我是你的语音助手。
主机接口调用成功，下载115244字节，实际PCM 115200字节，时长2.4秒。
峰值12223、RMS2390.8、饱和样本0；这些数据确认非静音，不能代替扬声器试听。
原始WAV为qwen-tts-test.wav；修正长度字段供试听的副本为qwen-tts-preview.wav，均位于esp32/tmp/easyinput。
没有将Key、接口原始响应或带签名的音频URL写入验证记录。

实际运行命令通过已安装Python的urllib调用HTTPS接口，没有新增依赖。
域名：dashscope.aliyuncs.com（合成）、返回的阿里云OSS北京域名（音频下载）。
后台任务、取消和播放集成仍需固件编译和板上验证；接口调用成功不能标为板上验收通过。

## 构建与待验收状态

2026-10-10：S3固件编译退出0，确认IDF_TARGET_ESP32S3、HOMEHUB_QWEN_TTS均启用，Key配置存在（未记录内容）。
固件位于esp32/build-easyinput-v2/muse-gadget.bin，1,445,888字节，4MiB应用槽约剩66%。
编译日志未报告warning/error，新Qwen源文件编译生效，git diff --check通过。
在esp32目录激活tmp/easyinput/env.ps1，给SDKCONFIG_DEFAULTS追加`;tmp/easyinput/sdkconfig.qwen.local`后执行：
`python D:/Espressif/v6.0.1/esp-idf/tools/idf.py -B build-easyinput-v2 -DIDF_TARGET=esp32s3 build`。
日志为esp32/tmp/easyinput/qwen-s3-build.log。

前一次命令的相对输出目录解析到仓库根目录，生成默认C5构建。
该产物位于build-easyinput-v2，不用于烧录；没有擅自删除，后续可确认后清理。
只使用上述esp32下经过S3目标核验的产物。

重新枚举得到COM6，USB序列号对应c4:9e:7e:07:9f:d8。
在不复位设备的前提下采集了旧固件15秒日志：setup=done、wifi=up、ws=up、raw=up，
内部空闲约97KiB、最大块58KiB，PSRAM空闲6345KiB；电池缓存4210mV，外部供电，未充电。
这些是旧固件状态，不能作为Qwen功能运行通过的证据；电池精度仍未外测。
日志为esp32/tmp/easyinput/pre-qwen-runtime.log，保存前过滤凭据。

新固件尚未烧录；扬声器朗读、打断、取消、网络失败和峰值内存尚未真机验证。
没有安装新依赖、修改系统环境、写eFuse或推送代码。
本机配置、音频和构建日志继续留在Workspace内，便于后续验证。

## 板上首轮验证

用户确认目标设备和写入范围后，2026-10-10烧录到COM6的ESP32-S3，MAC c4:9e:7e:07:9f:d8。
烧录前重新读取芯片身份，读取0x10000分区表并确认与构建产物逐字节一致。
烧录启动程序、分区表、OTA状态、PHY和应用，退出0、数据校验通过；未擦除NVS或写eFuse。
日志：esp32/tmp/easyinput/qwen-flash.log。

关机再开机后，设备恢复配对、Wi-Fi和Muse连接，控制连接验证了新OTA镜像。
两次S1录音分别为3.18秒和2.90秒，语音消息得到chat/stream确认，收到115字符和12字符的完成回复。
Qwen合成请求的TLS证书验证通过，但后续音频下载连接报错：
`No matching trusted root certificate found`、`Failed to verify certificate`、`mbedtls_ssl_handshake returned -0x3000`。
两次均显式结束为QWEN TTS FAILED；用户确认手机有文字回复、扬声器无声音。
没有观察到本轮WAV下载或PCM播放成功，因此不能标为朗读通过。
期间心跳内部空闲约92KiB；合成请求时约75KiB；请求失败后恢复约92KiB。
这是采样时刻，不是严格测得的峰值内存。
日志：esp32/tmp/easyinput/qwen-runtime.log，保存前过滤凭据相关内容。

本地配置核验确认已使用MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL，不能将问题归因于精简证书包。
下一步：在用户授权后只读核验OSS公开TLS证书链，再针对实际缺失或交叉签名原因提出修复。
不关闭证书校验。联网核验尚待用户确认。

## 交叉签名修复

用户要求解决故障后，对dashscope-result-bj.oss-cn-beijing.aliyuncs.com进行只读TLS证书链核验。
服务器链：OSS叶证书 → GlobalSign GCC R46 OV TLS CA 2025 → 交叉签名GlobalSign Root R46 → GlobalSign Root CA（R1）。
IDF 6.0.1完整包内有自签名R46，没有服务器链最后指向的R1。
使用包内R46证书成功验证OSS中间证书签名，确认可从已有可信根建立有效链。
包内R46的SHA256指纹为4fa3126d8d3a11d1c4855a4f807cbad6cf919d3a5a88b03bea2c6372d93c40c9。
原构建未启用MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY，导致按服务器末端证书的issuer查找时失败。

用户明确授权修复、重新编译和刷机验证后，仅在EasyInput板型overlay及本机构建配置中启用：
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`。
不下载新根证书、不信任过期根、不关闭TLS校验，不修改电脑证书库或IDF安装。
IDF Kconfig说明该功能额外堆使用约700字节，实机峰值尚需观察。
回滚：关闭该选项并重编译、刷回；仍保留HTTPS校验。
本轮没有新TTS合成调用，公开证书链核验不消耗合成额度。

### 修复固件构建与烧录结果

修复构建退出0；S3目标、Qwen功能、完整证书包、交叉签名验证及MBEDTLS_X509_TRUSTED_CERT_CALLBACK全部确认启用。
签名固件1,445,888字节，4MiB应用槽约剩66%；构建日志未报告warning/error，git diff --check通过。
构建沿用上述esp32目录内的命令，日志为tmp/easyinput/qwen-cross-signed-build.log。

用户明确要求修复并刷机验证后，再次枚举COM6并通过esptool读取到同一ESP32-S3及MAC c4:9e:7e:07:9f:d8。
执行原写入范围的write-flash命令（0x0/0x10000/0x1d000/0x1f000/0x20000），退出0，写入校验通过。
日志为tmp/easyinput/qwen-cross-signed-flash.log；没有整片擦除、NVS清除或eFuse写入。

修复固件启动后连续两段有界日志显示setup=done、wifi=up、ws=up、raw=up。
已保存的Wi-Fi和配对继续可用，内部空闲约107–108KiB，PSRAM约5911KiB；未观察到崩溃、重启或看门狗故障。
日志为tmp/easyinput/qwen-cross-signed-runtime.log，保存前过滤凭据相关内容。
本次窗口尚未捕获S1按键或新的语音请求，因此音频下载、朗读和打断尚不能标为通过。
已请求用户按住S1说短句再松开，并反馈是否听到声音。

实际修改为devices/sdkconfig.easyinput-v2新增一个选项及中文原因注释，以及本机生成的sdkconfig对应选项。
未新增依赖、未修改系统配置、未下载新根证书；公开TLS检查仅访问前述OSS域名。
保留本机构建和日志用于后续语音验收，当前不自动删除任何产物。

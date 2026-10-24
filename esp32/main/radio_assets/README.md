# 电台台名提示

经用户授权，使用本机已有的Microsoft Huihui Desktop中文声音生成。
classical：上海经典九四七；ambient：环境音乐台。

WAV是16kHz生成原件；旧classical/ambient PCM为16kHz单声道，新stations合集格式见下文。
不使用云端TTS、不在设备运行时下载或写Flash。播放时混入48kHz喇叭通路并复用音量/静音设置。

2026-10-24：12台选台提示使用同一本机中文声音生成，合并为stations.pcm，
stations.h保存各台的样本偏移。顺序与radio.c的urls一致：动感101、经典老歌103.7、
北京文艺广播、中国之声、律动轻音乐、007秘密特工、新加坡Kiss92、醉心933、
城市958、987、英国Kiss100、Heart。固件只嵌入stations.pcm；旧两台素材保留作历史原件。

后续容量优化：各段先以63抽头Hamming窗、3.4kHz截止低通，再从16kHz二倍降采样至8kHz。
stations.pcm现为8kHz、16bit小端单声道，共525840字节；stations.h的RADIO_PROMPT_RATE与偏移同步更新。
48kHz输出时每个提示音样本重复6帧，保留原时长/音调，不修改直播MP3解码链路。
本地转换脚本及16kHz原合集备份位于Workspace的esp32/tmp/easyinput，不调用云端服务或外部编码器。

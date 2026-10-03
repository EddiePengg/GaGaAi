#pragma once

// 固件版本：串口启动横幅 "gaga ai fw-idf <版本>"（版本行 = 验收锚点）；
// hello 信令 {"type":"hello","fw":...}（protocol.md §3）复用同一宏
// 0.2.0 = M1 按住说话链路 + M5 realtime 全双工（2026-09-23）
// 0.3.0 = 消息卡 UI（一问一答一张卡，M6 反馈闭环）（2026-09-24）
// 0.4.0 = 深空黑正式主题 + 设备端设置系统（音量/亮度/息屏/提示音/关于，
//         NVS 持久化，ADR-031"设备即应用"）（2026-09-24）
// 0.4.1 = 字库补 U+2014/U+2013 破折号 + 设备端文本过滤（渲染不了的字符
//         原地剔除，防方块字，ADR-032）（2026-09-24）
// 0.5.0 = 三唤醒（IMU 双击/抬手 + WakeNet"Hi,乐鑫"）+ 低功耗（息屏降频/
//         挂机深睡）+ 模块化重构（ADR-037/038）+ 导航模型（下拉面板/右滑
//         返回/鸭子页，ADR-035/036）+ talk 优化（provider 化/字幕开关/ASR
//         清屏，ADR-034）+ 回复全文容量 2048B/PSRAM/滚动条（2026-09-25）
// 0.6.0 = 内存专项 + WiFi 家模式互斥落地（2026-09-27 深夜，ADR-065）：
//         LinkManager 总机（BLE↔WiFi 自动故障转移+可逆拆栈）、WiFi 瘦身
//         配置包、刷屏缓冲退 4 行、wifi_cfg 扩展 broker/鉴权字段、串口
//         'w'/'N'/'G' 命令
// 0.6.1 = 链路可视化 + 手动切换（2026-09-28 凌晨）：设置页"当前链路"行 +
//         "工作模式"三档循环（Auto/外出·BLE/在家·WiFi），link_mode NVS
//         持久化重启恢复；强制WiFi 开机延迟 3s 切换（绕开 boot 堆碎片税）
// 0.7.0 = 语音唤醒落地（2026-09-28 通宵）：Jarvis（wn9_jarvis_tts，model 分区
//         低地址 0x812000 治 issue #135 幽灵）+ 唤醒即开录 + 8s 静音自动收尾
//         （短段静默丢弃）+ 3min 硬顶 + 状态栏改版（链路/内存/小时间）
// 0.7.1 = 服务器主动 TTS 语音通知（2026-09-27 深夜，ADR-066）：notify/notify_end
//         信令 + NotifySession 三态机（Idle/Playing/Silent）+ gaga_notify 播放泵
//         （复用 talk 下行 OggDemux/OpusDec/抖动缓冲）+ "GAGA 提醒"消息卡 +
//         needsDevice 名单加 "notify" 前缀（多设备过滤）
// 0.7.2 = 通知播放丢帧竞态修复（2026-09-28 真机实锤）：TTS 音频帧在 notify 后
//         60ms 内到达，而激活路径（等叮咚+上电 24k）有 ~1s 窗口——帧在此前全丢
//         = 有卡有叮咚无声音。accepting_ 闸门：notify 到达即收帧入队，不等
//         播放通路就绪；activate 不再清队列
// 0.7.3 = FrameDecoder 滑窗自救（2026-09-28 真机实锤）：App BLE 写泵丢包
//         → 流错位 → 旧解码器把脏字节误读成 0xfdXX 超长假帧吞掉全部后续
//         真帧（notify 音频全灭、报 未知帧 type=0x53=ogg 魔数）。修法：
//         payload 上限 8KB + 非法帧头/超长即 Hunt 逐字节滑窗找下一个合法
//         帧头（≤1 个物理包自愈，不再等 1s 残帧超时）；RX 诊断日志降 ESP_LOGD
// 0.7.4~0.7.7 = notify 真机联调诊断迭代（2026-09-28）：WiFi MQTT 重连清
//         重组状态、'H' broker-only 串口配置（不动 WiFi 密码）、wifirx/hdr
//         逐包对账日志（默认 ESP_LOGD，'G' 开）
// 0.7.5 = 'H' broker-only 串口配置命令（2026-09-28）
// 0.7.8 = 分片帧首包 payload 消费修复（2026-09-28 真机实锤，notify 全链路
//         打通）：旧解码器读完首包头直接进子包头，把首包自带 509B payload
//         当子包头解析（ogg "OggS" 被误读成 02 00 00 假帧 → 整流误判吞掉）。
//         修法：分片帧先按 509B 约定收首包 payload 再进子包头。此 bug 为
//         潜伏至今的分片接收路径缺陷（分片下行此前无真实业务使用）。
//         修复后实测：notify 3.46s 音频完整播放（包=173 pcm=83040 欠载=0
//         丢帧=0）。server 配套：下行 RLock 串行化，音频分片流期间心跳/
//         信令禁止插队（分片帧内嵌单包帧 = 协议级不可恢复错位）
// 0.7.9 = espcoredump 尸检落盘常驻（2026-10-03，ADR-079）：panic/断言/TWDT
//         暴毙把寄存器+任务栈写 coredump 分区（表尾 256K，model 地址不动），
//         连机 espcoredump.py 可读回解码。动机：白天 ~2h 周期性 panic 重启
//         （复位原因 4，临终无预警），USB 回溯重枚举即丢。配套：串口 'E'
//         故意 panic 自测全链路；sdkconfig.base coredump 段 + partitions.csv
//         coredump 分区。分区表变更：littlefs 0x16EE000→0x16AE000。
#define FW_VERSION "0.7.9"
#define DEVICE_ID  "gaga-01"  // 设备名（hello 信令里上报；2026-09-27 起也随 rec/rec_status 信令上报，服务端按 device 记账——多设备串扰修复）

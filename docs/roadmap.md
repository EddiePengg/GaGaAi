# 路线图与进度

## 里程碑

### M1：服务端管道 ✅ 已完成（2026-09-22）
- [x] MQTT Broker（Mosquitto；本地开发用 brew 直起，生产随 `server/docker-compose.yml` 交付 → ADR-014）
- [x] 接收音频 → ASR → 飞书 webhook → 回执（faster-whisper small 本地识别 → ADR-013）
- [x] 电脑上直接 POST 音频文件测试全链路（`POST /debug/audio`）
- 验收：命令行 curl 一段录音 → 飞书群出现文字 → 收到 receipt ✅ 三层验证记录见 `server/README.md` §验证
- **升级（2026-09-23，ADR-022）**：ASR 改为火山流式直通（`ASR_PROVIDER=volc_stream`，sauc bigmodel_async，Opus 帧边到边转发），本地 whisper 降为失败兜底。实测 rec_stop→终稿：本地批处理 ~3.7s → 流式 1.6~2.2s；真机人声识别 "能听到我说话吧？鸭子。" 一字不差

### M2：Android 哑管道 ✅ 已完成（2026-09-22）
- [x] 前台服务保活（connectedDevice + 常驻通知 + START_STICKY）
- [x] BLE Central：扫描 GAGA- 前缀 → MTU 517 → 退避重连
- [x] MQTT：gaga/up 发布、gaga/down 订阅、HiveMQ 自动重连
- [x] assembleDebug 构建通过（app-debug.apk 3.5MB）
- [x] **保活组合拳（2026-09-23，ADR-024，v0.1.6）**：开机自启 + 15min 看门狗闹钟 + onTaskRemoved 自愈 + 电池白名单一键跳转 + 常驻通知状态灯（跟随 BLE/MQTT 状态）+ 打开 App 自动拉起服务
- [x] **CDM 伴生设备（2026-09-23，ADR-025；2026-09-24 修订自动化）**：系统弹框配对嘎嘎 → CompanionDeviceService（设备出现自动拉服务、绑定期进程保级）+ 存 MAC 后重连走直连（免扫描，绕开后台扫描限流）。修订：配对框加 GAGA- 前缀过滤器（空 filter 只列已配对设备、不做 BLE 扫描）；点配对自动断开 BLE→弹框→配完自动重连；链路就绪自动记 MAC（直连不再依赖配对）
- [x] **通知状态灯改造（2026-09-24，v0.1.9）**：常驻通知标题 "GaGaAI Listening"，BLE/MQTT 状态用 ✅/⏳/❌ 表达（不再写"已连接"字样）
- [x] **App 主界面趣味化（2026-09-24，v0.1.9）**：暖黄主题 + 鸭子当家（服务运行时摇摆动画）+ "GaGaAI / quack quack · I'm listening" 头部 + 圆角卡片化（状态/终端日志/服务器/保活四卡）+ 状态行 emoji 化（🟢⚪📡✅⏳❌，与通知共用 UiStyle 渲染）
- [x] **MQTT 乒乓互踢修复（2026-09-24，ADR-039，v0.2.2）**：同 Client ID 反复 session taken over 的根因是服务被系统快速反复重建时每个实例各建一个 HiveMQ 客户端并发互踢；MqttManager 单例化 + start 幂等（同地址复用连接）+ 重连退避 3s 起步
- [x] **主界面按设计稿重构（2026-09-25，v0.3.0）**：渐变云朵头部（标题/标语/齿轮快捷入口/鸭子当家）+ 红色停止/鸭黄启动胶囊大按钮 + 链路状态改三节点状态流（⚙️→📡→🌐 圆环配色 ✅绿/⏳黄/❌灰 + 绿勾徽章 + 整体状态一句话）+ 服务器/配网/保活卡横排紧凑化 + 修复 EditText 抢焦点导致启动页面自动滚动
- [x] **设置面板 + 终端日志显隐（2026-09-25，v0.3.1）**：齿轮打开 App 内设置（显示终端日志开关，默认开、Prefs 持久化；隐藏时日志仍后台记录）+ 系统权限设置入口（应用详情页）
- [x] **UX 直觉化改造（2026-09-25，v0.3.2）**：WiFi 配网改"从附近 WiFi 列表选择 + 填密码"（系统扫描结果、信号/锁图标、可重扫，不再手打名字）；服务器卡折叠化（默认只显示 🟢/⏳/🔴 连接状态行，点"修改"展开表单，保存自动收起）；保活卡改设置列表式（🔋电池白名单/🦆伴生设备两行，右侧状态色 已开启✓绿/未开启›琥珀，取消配对收进伴生行管理弹窗）；整体状态徽章变彩色胶囊且可点击出修复指引（问题清单 + 启动服务/检查服务器一键处理）
- [x] **全面屏遮挡修复（2026-09-25，v0.3.3）**：Android 15+ 强制 edge-to-edge 后 statusBarColor 失效，头部标题被前置摄像头挖孔遮挡、齿轮与状态栏电池图标重叠；改为 WindowInsets 动态注入（状态栏+挖孔+导航条实际高度 → 头部顶距/内容底距），任意设备自适应
- [x] **UI 全量英文化（2026-09-25，v0.3.4）**：所有用户可见文案（按钮/状态/日志/Toast/对话框/通知/设置面板）改英语，包括状态串（"Connected …"）与 UiStyle.mark 判定前缀的联动修改；代码注释按仓库规范仍为中文
- [x] **WiFi 配网入口暂时移除（2026-09-25，v0.3.9）**：固件侧尚无 wifi_cfg 处理，UI 入口先撤（GagaService 的 ACTION_SEND_WIFI 分支与 WiFi 扫描选择器代码保留在 git 历史，待固件支持后恢复）
- [x] **头部渐变定稿（2026-09-25，v0.4.1）**：尝试层叠云朵自定义 View 后按用户反馈放弃（观感杂乱），定稿为奶油白→浅黄→鸭黄三段平滑线性渐变 + 底部 36dp 圆角收边；页脚改暖底墨字。全程无新依赖，纯 drawable 实现
- [x] **ColorOS 后台权限强制引导（2026-09-24，ADR-037，v0.1.8）**：进 App onboarding 弹一次不可关闭引导 → 跳应用详情页→耗电管理（ColorOS 16 封死所有耗电页直达，真机实测）；之后被系统杀（o-kill）只写日志不弹窗。同轮修复：CDM 配对回调 ClassCastException 闪退（Android 13+ EXTRA_DEVICE 是 ScanResult）+ 权限重置后看门狗拉前台服务循环 crash（SecurityException 兜底）
- [x] **设备端断连提示（2026-09-23，IDF 线）**：BLE 断开时按住说话/长按对话 → 屏幕提示"请打开手机App" + 滴滴
- [ ] 真机安装 v0.1.6 + ColorOS 保活设置 + CDM 配对实测（待用户操作）

### M3-idf：ESP-IDF 平行固件线（ADR-020，2026-09-22 开工）
第一期"最小可用鸭子"已完成（2026-09-23 真机验证）：
- [x] PlatformIO + ESP-IDF 5.5.3 工程骨架（`esp32-idf/`，32MB 分区表与 Arduino 线同布局）
- [x] 微雪官方 BSP `waveshare/esp32_s3_touch_amoled_1_75c` ^3.0.0 接入（idf_component.yml → dependencies.lock 锁版；归属 1.75C——本机 MCLK=GPIO16 实锤，见 Bug1）
- [x] BSP 点亮屏幕 + LVGL 9.4 黑底界面（CO5300 + CST9217 触摸均由 BSP 拉起；自组显示链路保留 panel 句柄供息屏）
- [x] NimBLE 广播 `GAGA-0A4E` + NUS GATT（6e400001/2/3，preferred MTU 517，实测 515）；bleak 真连通过，断连自动重广播
- [x] 串口版本行 `gaga ai fw-idf 0.2.0`（原生 USB-Serial-JTAG 控制台）
- [x] **帧协议收发平移**（protocol.md §2 与 Arduino 线字节级一致）+ GATT 分包/重组
- [x] **按键/状态机/UI 平移**（ADR-016：BOOT=GPIO0 右下、PWR=AXP2101 PKEY 边沿轮询右上；右下单击也唤醒亮屏——2026-09-23 用户 UX 改进）
- [x] **录音链路真通**：ES7210 采集（esp_codec_dev 自建 STD 立体声 I2S，MCLK=GPIO16 定案）→ Opus 16k/20ms → BLE 分包上行；真机 10s/32s 录音零丢弃零崩溃，Mac 桥转发 MQTT，服务端完整 ASR 出字上飞书
- [x] 提示音"滴滴"（ES8311 合成 1kHz 正弦异步播放）；**2026-09-23 修复"按下没声"+"开头丢字"**（ADR-023/026/027）：音频作业串行化 + 先开麦后播滴 + 确认窗缓冲不丢 + 按需上电（ADR-023 的"双工常开"因功耗被 ADR-027 推翻）
- [x] 无操作 10s 自动息屏（DISPOFF+SLPIN）；LVGL 像素快照自证渲染（串口 'p'）
- [x] LVGL 中文字体（自生子集 886 字，OFL 思源黑体；lv_font_conv 必须 --no-compress）
- 三个真机 bug 的根因分析+修复全记录：docs/bug-analysis-0923.md
- 验收：构建通过；真机启动日志含版本行；屏幕显示正常；BLE 广播可见可连；录音上行到服务端出字 ✅

### M4：全链路闭环（项目核心价值）
- [x] **M1 + M2 + M3 串联（2026-09-23 IDF 线真实打通）**：串口触发按住说话 → 设备 Opus 上行 → Mac BLE↔MQTT 哑管道桥 → 服务端 faster-whisper ASR → 飞书群出字（"🦆 今天天气怎么样?适合骑车吗?"）→ receipt 下行回设备。10s 录音 420 帧零丢弃；32s 连续录音 1513 帧零崩溃
- [x] **手机 App 真机串联（2026-09-23 用户实测通过）**：鸭子 → BLE → OPPO App v0.1.4 → VPN → 服务端 → 飞书，两次真人语音全部准确识别（含长句）；receipt + reply（识别原文回显）双下行正常
- 验收：按一下说话 → 飞书群收到文字 → Hermes 响应 → 设备"叮"+✅ ✅ **达成**

### M5：长按右上键实时对话（豆包 Realtime 3.0 Seeduplex 全双工）
- [x] 信令协商（talk_request / talk_ready / talk_end / talk_asr / talk_reply，protocol.md §3、data-model.md §1）
- [x] 服务器桥接火山引擎 Realtime API（`server/src/gaga_server/realtime/`，ADR-021；2026-09-23 真实 API 两轮对话实测通过）
- [x] 音频媒体通道：MQTT 帧透传（未开 WebSocket——ADR-021：上行 Opus 直通 + 服务端 20ms 节奏器，下行 ogg_opus 帧化回 gaga/down）
- [x] **固件侧 talk 会话骨架（2026-09-23，IDF 线）**：TalkSession 状态机 + 24kHz 双工音频通路 + 上行泵（24k 采集→2:3 抽取→Opus 16k 直通上行）+ 下行播放泵（Ogg 解封装→Opus 解码→ES8311 播放，jitter 队列）；真机已验证 talk_request→talk_ready→Active→talk_end 全流程与上行帧流（rms 随声音起伏）；下行放音与真人对话待用户实测
- [ ] 退出意图自动结束（enable_user_query_exit 已开、处理代码已就位；五轮实测未触发 20000002，疑似 TTS 合成音韵律不足，待真机人声验证）
- [ ] AEC 上岗、打断支持（response.cancel 客户端已封装，待固件侧联调；固件本期不做主动打断——上行帧照常发，全双工模型自己判打断，protocol.md §6）
- 调研（2026-09-23，[官方文档](https://docs.volcengine.com/docs/DoubaoVoice/endtoend-realtime-voice-full-duplex-version?lang=zh)）：
  - 端点 wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue（鉴权头 X-Api-Key）
  - **上行支持 speech_opus 16kHz/20ms——与固件 Opus 参数一致，服务器可原包直通**
  - 下行 ogg_opus 24kHz；支持 Function Calling（→ Hermes/工具集成的口子，本期不接）、
    退出意图识别（自动结束会话）、流式 ASR 文本（可上屏显示用户说的话）
  - 上行必须严格按真实节奏发帧；关麦须发 input_audio_mute.commit 事件（服务端已自动 mute/unmute 保活）

### M6：体验层（并行小任务）
- [x] **消息卡反馈闭环 UI（2026-09-24 fw-idf 0.3.0，ADR-028）**：消息卡列表主页（20 条 RAM 环形，点卡进详情）+ 顶部状态栏（时间/电量/BLE，圆屏收中央）+ 松手即时出卡（receipt 填 ASR 文本）+ reply 就地填充/息屏自动亮屏直达详情 + 叮咚双音 + Markdown 服务端拍平（textfmt.py）+ RTC 对时（信令 ts 自动校准）。字库扩 GB2312 全集 7448 字（flash ~1MB）。**2026-09-24 修破折号**：真机报障"——"显示不出——GB2312 映射的横线是 U+2015、AI 输出的是 U+2014，形同码异，字符集补 `—`(U+2014)/`–`(U+2013) 后重生成字库（覆盖 8061 码点，cmap 解析验证 ✓）。**配套：设备端文本过滤（ADR-032，fw 0.4.1）**——动态文本逐字符查本机字形表，渲染不了的原地剔除（emoji/字库外符号静默消失，不再方块）；FontFilter.cpp 已登记 CMakeLists，与并行开发线（motion/power/debug）无耦合，待整体构建回归。
- [x] **接入端 channels/ 抽象 + 飞书全量官方 API（2026-09-24，ADR-030）**：发送 im/v1/messages（receipt 带 msg_id）+ 轮询收 Hermes 回复 → reply 下行；CHANNEL 环境变量选平台（微信/Telegram 三步接入法见 channels/__init__.py）；webhook 退役。**端到端已实测（探针）**：官方 API 发"适合骑车吗"→ Hermes 18s 回 post 富文本 → 轮询捕获 → on_message 全文到手（期间修掉 ListMessage 默认排序拉最旧页的坑，须 sort_type=Desc）；剩真机整体回归：重启 gaga-server 后设备消息卡点亮
- [x] **深空黑正式主题 + 设备端设置系统（2026-09-24 fw-idf 0.4.0，ADR-031）**：UI 全面重设计——纯黑底（AMOLED 黑像素零功耗）+ 白主文字 + 鸭黄 accent，卡片 remove_style_all 自绘样式根除默认主题白边；状态栏重排贴圆顶（电量·BLE圆点 | 时间 montserrat_24 | 设置入口），与主体拉开 62px 呼吸区；设置页全触摸操作（音量/亮度 slider、息屏时长 5/10/30/60s 循环、提示音开关、关于页），Settings 模块 NVS 持久化（`state/Settings.cpp`）。真机验证：快照非黑像素 95%+（黄底）→ 31%（深色主题）；设置页五行渲染分带均匀分布 ✓；验证期间用户真实对话全链路（录音→ASR→飞书→Hermes→reply 下行）在新固件上正常跑通
- [x] **三唤醒 + 低功耗一天路线 + 模块化重构（2026-09-25 fw-idf 0.5.0 真机回归，ADR-037/038）**：QMI8658 寄存器驱动 + 硬件 Tap 双击引擎（**真机就绪**，SensorLib ACK=0x00 坑已排）+ 抬手姿态状态机（待用户物理验证）；息屏降频 240→80MHz（真机循环验证 ✓）+ 挂机 10min 深睡；main.cpp 1092→330 行模块化。**WakeNet 关键词唤醒暂缓**（esp-sr 内存模型+mmap 崩溃专项未解，KeywordWake 留占位与启用清单，详见 dev-log 2026-09-25）
- [x] **回复显示不全根治（2026-09-25）**：用户报障回复被截——设备端 MsgLog.reply 384→2048（全文级）、20 条卡迁 PSRAM（47KB 内部 RAM 付不起）、UI 渲染缓冲扩 2.2KB（static 防爆栈）、三容器加可见滚动条；BLE 重组/服务器侧排查确认不截
- [x] **导航模型定稿：右滑返回 + 鸭子页（2026-09-25，ADR-036）**：indev 级手势（LV_EVENT_GESTURE，与列表滚动共存）——首页左滑进鸭子页、子页面右滑返回；鸭子页=陪伴层（自绘扁平小鸭子 + 按时段问候 + 今日概览 + 最近回复气泡）；任务管理器明确不做（单应用无对象+与列表滚动冲突）。串口 'C'。**待真机**：手势手感/鸭子摆位
- [x] **顶部下拉快捷面板（2026-09-25 fw-idf 0.5.0，ADR-035）**：顶部下滑/轻点呼出控制中心——亮度/音量大滑条 + 语音唤醒/字幕开关 + 收起；220ms 滑入动画、点外关闭、15s 自动收、录音/talk 让路；串口 'Q' 验证。**待真机**：手势阈值/右角分界手感
- [x] **实时对话三优化（2026-09-25，ADR-034）**：服务器 realtime provider 化（归一化事件契约；volc 迁移完成、gemini Live 实现待 key 联调、"星辰"三步接入法）；固件"对话字幕"设置开关（防长文本重排卡顿，纯语音模式）；ASR 分层清屏（transcription.started → 空 talk_asr → 设备清上一句，叠字根治）
- [ ] AOD 表盘（黑底省电设计；深色主题已就位，AOD 是它的降亮度特例）
- [ ] IMU 装饰效果（重力小球/粒子流）
- [ ] 在家 WiFi 直连模式
- [x] LVGL 中文字体（2026-09-23 IDF 线：自生子集 C 数组字体入库 esp32-idf/src/ui/fonts/，OFL 思源黑体 886 字；没走 LittleFS——C 数组更稳；lv_font_conv 必须 --no-compress，ESP lvgl 9.4 不渲染 RLE 压缩位图。**2026-09-24 扩 GB2312 全集 7448 字**：ASR/回复动态文本任意汉字，886 子集必豆腐；≈1MB flash，lvgl 直接按 flash 映射地址读字模，运行时 RAM 零占用）

## 当前状态

**fw-idf 0.5.0（2026-09-25 清晨真机收线）：设备稳定运行——双击引擎就绪、抬手待物理验证、鸭子页/设置页/快捷面板/消息列表四视图快照全过、息屏降频/恢复循环正常；WakeNet 暂缓（esp-sr 三坑+model 分区 boot loop，启用清单在 KeywordWake 文件头）；实时对话三优化（provider 化/字幕开关/ASR 清屏）在服务器与固件侧就绪待联调。**

**fw-idf 0.4.0（2026-09-24 深夜）：深空黑正式主题 + 设备端设置系统上线（ADR-031"设备即应用"路线第一步）。** M1~M4 全链路（按住说话→BLE→App/桥→服务器→飞书→receipt/reply 回设备）在 0.4.0 上真实使用中回归通过；M5 真人全双工待联调（骨架已通）；M6 体验层进行中（消息卡/飞书官方 API/深色主题+设置已完成，AOD/IMU/WiFi 直连待做）。剩余关键路径：手机 App 揣兜实测、WiFi 直连模式（摆脱手机 BLE 依赖，"拿起来就能用"的最后一块）。

**Arduino 线（esp32/）已退役删除（2026-09-25，ADR-041）**：源码存档 `reference/esp32-arduino-line.tar.gz`，（所备份镜像经用户确认非原厂，连同 Arduino 源码存档一并删除）；芯片参考资料独立为根 `reference/`。

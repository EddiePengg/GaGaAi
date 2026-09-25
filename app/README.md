# gaga ai Android App

gaga-ai 系统的手机端哑管道：BLE（ESP32-S3）↔ MQTT（服务器）双向原样字节转发，App 不解析帧内容。协议详见 `../docs/protocol.md`。

## 技术栈

- Kotlin，minSdk 26，target/compileSdk 35，普通 View + XML（无 Compose）
- AGP 9.4.1（内置 Kotlin，无需单独 Kotlin 插件）+ Gradle 9.7.1
- MQTT：HiveMQ MQTT Client 1.4.0（MQTT 3.1.1，QoS 1，keepalive 60s 默认，自带指数退避自动重连）
- 前台服务 `connectedDevice` 类型常驻，START_STICKY 被杀后自动恢复

## 构建

```bash
cd /Users/eddiepeng/Documents/gaga-ai/app

# JDK 用 Android Studio 自带的 jbr（Gradle 9.7.1 可在 JDK 25 上运行）
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"

./gradlew assembleDebug
```

产物：`app/build/outputs/apk/debug/app-debug.apk`

`local.properties` 已写入 `sdk.dir=/Users/eddiepeng/Library/Android/sdk`。

## 代码结构

```
app/src/main/java/com/gagaai/app/
├── MainActivity.kt        # 状态展示、启动/停止按钮、Broker 配置、运行时权限
├── service/
│   └── GagaService.kt     # 前台服务，宿主 BLE + MQTT，常驻通知，START_STICKY
├── ble/
│   └── BleManager.kt      # 扫描 GAGA-/Service UUID → 连接 → MTU 517 → 订阅 TX
│                          # Notify；下行按 MTU-3 分包写 RX；断连指数退避重扫
├── mqtt/
│   └── MqttManager.kt     # HiveMQ 客户端，发 gaga/up、订 gaga/down，QoS 1，
│                          # clientId=app-<androidId>，自动重连
└── util/
    ├── Prefs.kt           # Broker 地址 SharedPreferences
    └── BridgeState.kt     # 服务 → UI 的状态总线（同进程回调）
```

## 使用

1. 安装后打开 App，授予蓝牙扫描/连接、定位、通知权限。
2. 填入服务器提供的 MQTT Broker 地址和端口（默认 1883），点"保存 Broker 配置"。
3. 点"启动服务"。设备（广播名 `GAGA-XXXX`）上电后会自动连接，状态栏显示"gaga ai 服务运行中"。

## OPPO / ColorOS 后台保活设置

ColorOS 对后台管控激进，装完后务必做以下设置（路径以 ColorOS 13/14 为例，不同版本略有差异）：

1. **锁定最近任务卡片**：打开 App 后进入多任务界面，下拉 gaga ai 卡片（或点卡片右上角菜单），选择"锁定"。锁定后一键清理不会杀掉它。
2. **电池不优化**：设置 → 电池 → 应用耗电管理（或"应用管理 → gaga ai → 耗电管理"），选择"允许后台运行 / 不优化"。也可在 设置 → 电池 → 更多设置 → 优化电池使用 里把 gaga ai 设为"不优化"。
   App 内也有**一键跳转**（v0.1.6 起）：「保活 → 加入电池白名单」直接弹系统授权框。
3. **允许自启动**：设置 → 应用管理 → gaga ai → 自启动管理，打开"允许自启动"。部分版本在 手机管家 → 权限隐私 → 自启动管理 里。
4. **通知权限**：保持允许，前台服务依赖常驻通知，被禁后 ColorOS 更容易回收服务。
5. **深度睡眠应用**（部分 ColorOS 版本才有该入口）：设置 → 电池 → 更多设置 → 深度睡眠应用，确保 gaga ai **不在**名单里。找不到该项的版本跳过。

## 代码级保活（v0.1.6，ADR-024/025）

设置是第一道，代码里还有六道自愈通道，全挂了也能爬起来：

| 通道 | 触发场景 |
|---|---|
| START_STICKY | 系统杀进程后自动重启服务 |
| 开机自启（BootReceiver） | 手机重启（含 QUICKBOOT） |
| 看门狗闹钟（KeepAlive，15min） | 被杀/冻结后最迟 15 分钟复活 |
| onTaskRemoved 自愈 | 手滑滑掉多任务卡片/一键清理 |
| 打开 App 自动拉起 | 任何情况下点开 App 即恢复（"强行停止"后的唯一复活路径） |
| 伴生设备出现自拉（GagaCompanionService） | 嘎嘎出现在附近时系统绑定服务并拉起 |

**用户明确"停止服务"**后以上全部让位（`Prefs.userStopped`），直到重新打开 App。

常驻通知同时是**链路状态灯**：`蓝牙：… MQTT：…` 实时刷新，通知在=服务活，一眼判断要不要先开 App。

## CDM 伴生设备配对（ADR-025）

「保活 → 配对嘎嘎（伴生设备）」→ 系统弹出设备选择框 → 点选 GAGA-xxxx，一次配对永久关联：

- 系统把本 App 当嘎嘎的配套 App，**绑定期间进程保级、允许后台起前台服务**（手表/耳机 App 同款待遇）；
- 存下设备 MAC，BLE 重连走 **connectGatt 直连**（不扫描）——绕开 Android 后台扫描限流
  （`BleManager` 文件头注释里"ColorOS 后台停发结果"就是它），重连更快更稳；
- 顺带不再依赖定位权限拿 MAC。

取消配对：「取消配对」按钮（清关联 + 清 MAC，回退扫描模式）。

服务本身已用 START_STICKY + 前台服务，被系统杀掉后会自动拉起并重连 BLE/MQTT；上面的设置是为了减少被杀的概率。
5. 蓝牙保持开启；App 会在蓝牙重新可用后自动恢复扫描。

服务本身已用 START_STICKY + 前台服务，被系统杀掉后会自动拉起并重连 BLE/MQTT；上面的设置是为了减少被杀的概率。

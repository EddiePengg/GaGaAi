# reference/ —— 官方资料存档

各开发板官方资料的原样快照：防止原链接失效/更新后无处可查。
**这里的文件只存档、不参与编译，禁止改动内容**（代码侧要改的是 `variants/` 里的抄录版）。

## waveshare-s3-amoled-1_75（微雪 ESP32-S3-Touch-AMOLED-1.75）

- `waveshare-s3-amoled-1_75/pin_config.h` —— 2026-09-22 原样快照。
  出处：官方 Arduino 示例包镜像仓库
  <https://github.com/doublemarkpro/ESP32-S3-Touch-AMOLED-CodexPedometer>
  的 `examples/arduino/libraries/Mylibrary/pin_config.h`
- 官方 Wiki：<https://www.waveshare.net/wiki/ESP32-S3-Touch-AMOLED-1.75>
- 官方 ESP-IDF 仓库（HARDWARE_REFERENCE.md 经原理图交叉核对，寄存器级细节参考；
  ESP-IDF 风格不引入工程，ADR-007/ADR-017）：
  <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75>

引脚的代码侧权威来源是 `variants/waveshare-s3-amoled-1_75/pins.h`（从上述快照抄录，
并与 `../docs/hardware.md` §引脚定义 同步——AGENTS.md 铁律）。

## 用户提供的官方资料（2026-09-23）

⚠️ 注意型号：以下是 **1.75C** 变体的资料（触摸芯片 FT3168，与 1.75 的 CST9217 不同）。
本项目真机实测与 1.75 资料兼容（显示/音频/按键均已验证），引用时以真机实测为准。

- 资源总入口：https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C/Resources-And-Documents/
- ESP-IDF 开发指南：https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C/ESP-IDF/
- 原理图 PDF：https://www.waveshare.net/w/upload/8/83/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf
- 官方仓库（1.75C）：https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C
- ESP32-S3 数据手册：https://documentation.espressif.com/esp32-s3_datasheet_cn.pdf
- ESP32-S3 技术参考手册：https://documentation.espressif.com/esp32-s3_technical_reference_manual_cn.pdf

# 屏幕"假死"案（未结，待复现抓现场）

## 案情

2026-09-30 用户报"开机闪绿屏后全黑，疑似硬件坏"。排查结论：**硬件正常，病根是"10% 亮度 + 深空黑纯黑主题 = 肉眼不可见"**。10%↔100% 亮度切换可稳定复现"好/坏"，排除排线假说。

## 2026-10-03 增补：panic 周期性重启案（"自动黑屏关机"的真身）

串口常驻录像（`logs/blackout_dvr_*.log`，/tmp/gaga_dvr.py）实锤：白天正常使用
**约每 2 小时一次 panic 重启**（复位原因 4），临终前心跳/堆/BLE 全部正常、
毫无预警；重启过程屏幕全黑十几秒，用户观感 = "自动黑屏关机"。此前 USB 枚举
消失的真因之一是线没插好，之二就是 panic 重启时的 USB 重枚举。

**固件 0.7.9 起 espcoredump 常驻**（ADR-079）：panic 现场自动写 flash coredump
分区（256K 表尾），下次连机可读回解码。日常使用流程：感觉它又黑了 → 插电脑
→ 按下面配方读尸检 → 定位代码行。只留最近一具尸体，连着崩两次会覆盖。

### 读尸检配方（2026-10-03 全链路验证通过）

```bash
cd esp32-idf
python3 -m pip install --user esp-coredump   # 一次性
export IDF_PATH=~/.platformio/packages/framework-espidf
python3 -m esp_coredump --chip esp32s3 -p /dev/cu.usbmodem2101 \
    info_corefile -t raw \
    -g ~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb \
    --save-core ../logs/coredump_<标签>_core.elf \
    .pio/build/waveshare-s3-amoled-1_75c/firmware.elf
```

- ⚠️ 两个坑：`-m` 在这版工具里是 `--print-mem` 布尔开关，**elf 是位置参数**；
  解码 elf 必须与被读尸检**同一次构建**（SHA256 校验，不符直接拒）。
- 自测锚点：串口发 `'E'` = 故意 abort()（SerialCmd），尸检应指回
  `main/debug/SerialCmd.cpp` 的 abort() 那一行——指回了 = 链路通。
- 已归档样例：`logs/coredump_selftest_20261003_*.log`（触发+重启日志）、
  `logs/coredump_selftest_decode.txt`（解码全文）、`logs/coredump_selftest_core.elf`（尸体）。

## 遗留疑点（下次复现时排查）

用户报过一次**亮度已是 100% 时**仍"按右上角不亮"（连续录音 50-60 条后）。无法用亮度解释，疑似息屏路径偶发 bug。候选：

1. `Display.cpp:167` 亮屏抢不到 LVGL 锁 → "sleep/wake 未拿到 LVGL 锁，面板指令跳过"（软件以为亮、面板没收到 DISPON）
2. 重负载后刷屏 `Draw bitmap failed: ESP_ERR_NO_MEM`（内部堆/DMA 池耗尽）
3. 息屏渲染闸门 + `fullRefresh` 重刷失败

## 复现时的排查 SOP（用户配合：先插 USB 告知我，**不要重启**，重启会清证据）

串口 `/dev/cu.usbmodem2101` @115200，先静置抓 60s 日志（不按复位），重点读：

- 设置载入行：亮度值是多少
- `[dim] 距息屏 Ns` 探针是否还在打（判定器是否活着）
- 有没有 "sleep/wake 未拿到 LVGL 锁" / "Draw bitmap failed" / "息屏被抑制"
- 心跳行 `内部=… DMA池=…` 堆是否耗尽
- 发 's' 亮屏 + 'L' 调亮度，观察屏幕是否恢复、日志报什么
- 必要时 'p' 帧缓冲快照：LVGL 认为自己在画什么

## 已验证的工具

- 's' 亮/息屏切换；'L' 亮度循环 10→30→60→100（落盘）；'c' 彩底测试；'y' 注入测试卡；'F'/'p' 截图
- python3 + pyserial 可直接收发（本机可用）

# IMU / 系统调试定标工具集

日常使用**不需要**这些工具——它们只在调试、定标、排障时由开发者手动运行。
**同一时刻只能有一个程序占用串口**：跑任何一个之前，确认没有别的进程
（另一个工具、idf.py monitor、烧录任务）占着 `/dev/cu.usb*`，否则数据会被瓜分。

## 工具一览

| 工具 | 用途 | 运行方式（仓库根目录） |
|---|---|---|
| `accel_capture.py` | IMU 原始数据流落盘（'A' 流 → 文件，带静默重试） | `python3 scripts/calibration/accel_capture.py [时长秒] [输出文件]` |
| `heap_monitor.py` | 堆水位监视（每 60s 发 'h'，追内存泄露/卡死） | `python3 scripts/calibration/heap_monitor.py` |
| `orient_calib.py` | 转向定标台（网页实时六轴 + A/B 姿势标注 + 自动推导判向公式） | `python3 scripts/calibration/orient_calib.py` → 开 http://127.0.0.1:8766 |

## 注意事项

- **accel_capture / orient_calib 开着数据流时，固件的摇动触发会被抑制**
  （只记 would-fire 不真触发，这是设计行为）——采集/定标结束后记得退出
  进程，设备恢复日常行为（v5 起插 USB 不深睡，但流开着依然压触发）。
- `heap_monitor` 只发 'h' 体检命令，与数据流无冲突，可与日常使用共存。
- 三个工具都自动探测 `/dev/cu.usb*`；找不到设备会直接报错退出。

## 历史数据

标注数据集（12 场景六轴采集 + 动作标注）：`docs/data/imu-sixaxis-20260925/`

#pragma once

#include <cstdint>

// 串口调试命令泵（原 main.cpp 的 serialTask + 全部快照/扫描/探针工具，重构搬出）。
// 无实体按键手段时的自测入口；键位三类：
//   【用户调试】'r' 录音切换  't' talk 切换  'b' 提示音  's' 亮息屏
//               'm' 采集声道  'w' 松手宽限  'l' 锁定模式
//               'S' 设置页开关  'V' 音量循环  'L' 亮度循环
//               'M' IMU 姿态 dump  'K' 抬手 Z 极性翻转  'Z' 立即深睡
//               'A' IMU 原始加速度流（摇动阈值离线调参）
//               'R' 手动旋转屏幕 0→90→180→270（自动转向验证/兜底）
//   【验收】'y' 注入问答卡  'u' 注入等待卡  'p' 像素快照  'P' CJK 字体探针  'c' 彩底测试
//   【排障】'd' mic dump  'e' codec 寄存器  'g' GPIO 活动  'i' I2C 扫描
//           'n' notify ping  'B' 1s 长音  'h' 堆水位
namespace gaga {

class AppContext;

class SerialCmd {
public:
    void begin(AppContext* ctx);  // 起串口命令任务（优先级最低，纯调试）
private:
    static void taskEntry(void* arg);
    void run();
    void handleKey(uint8_t c);

    // 排障工具（原 main.cpp 同名函数搬入）
    void i2cBusScan();
    void pinActivityProbe();
    void snapshotDump();
    void colorTestSnapshot();
    void imuDump();          // 'M'：姿态 + 唤醒状态机现场
    void heapReport();       // 'h'：内存体检（内部/连续块/PSRAM/任务栈水位）
    void frameDump();        // 'F'：整帧 RGB565 转储（Mac 端 scripts/screenshot.py 收）

    AppContext* ctx_ = nullptr;
    int  snapshotUseCjkFont_ = 0;  // 'p'/'P' 探针模式
};

}  // namespace gaga

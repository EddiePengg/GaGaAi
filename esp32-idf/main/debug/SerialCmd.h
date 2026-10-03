#pragma once

#include <cstdint>

// 串口调试命令泵（原 main.cpp 的 serialTask + 全部快照/扫描/探针工具，重构搬出）。
// 无实体按键手段时的自测入口；键位三类：
//   【用户调试】'r' 录音切换  't' talk 切换  'b' 提示音  's' 亮息屏
//               'm' 采集声道  'S' 设置页开关  'V' 音量循环  'L' 亮度循环
//               'M' IMU 姿态 dump  'K' 抬手 Z 极性翻转  'Z' 立即深睡
//               'A' IMU 原始加速度流（摇动阈值离线调参）
//               'R' 手动旋转屏幕 0→90→180→270（自动转向验证/兜底）
//   【链路/家模式】'w' WiFi 凭证行配置（JSON 行：ssid/pass/host/port）
//               'N' 链路模式循环 Auto→强制BLE→强制WiFi（LinkManager）
//               'G' 全局日志 DEBUG 开关（排障：看 WiFi init 内部失败点）
//   【验收】'y' 注入问答卡  'u' 注入等待卡  'p' 像素快照  'P' CJK 字体探针  'c' 彩底测试
//   【排障】'd' mic dump  'e' codec 寄存器  'g' GPIO 活动  'i' I2C 扫描
//           'n' notify ping  'B' 1s 长音  'h' 堆水位
//           'E' 故意 panic 自测（espcoredump 全链路验证：写盘→重启→读尸检）
namespace gaga {

class AppContext;

class SerialCmd {
public:
    void begin(AppContext* ctx);  // 起串口命令任务（优先级最低，纯调试）
private:
    static void taskEntry(void* arg);
    void run();
    void handleKey(uint8_t c);
    void handleLine(const char* line);   // 行模式提交（'W' WiFi 配置 JSON）

    // 排障工具（原 main.cpp 同名函数搬入）
    void i2cBusScan();
    void pinActivityProbe();
    void snapshotDump();     // 'p'/'P' 包装：PSRAM 守卫 + 投递 LVGL 任务
    void colorTestSnapshot();// 'c' 包装：同上
    void imuDump();          // 'M'：姿态 + 唤醒状态机现场
    void heapReport();       // 'h'：内存体检（内部/连续块/PSRAM/任务栈水位）
    void frameDump();        // 'F' 包装：同上（整帧转储本体在 frameDumpBody）

private:
    // 渲染类命令本体——只在 LVGL 任务上下文执行（dispatchToLvgl 投递）：
    // 串口任务 4KB 栈装不下 lv_refr_now 调用链（2026-09-28 晚三次栈溢出实锤，
    // 看堆守卫拦不住——瓶颈是栈不是堆），大栈渲染是 LVGL 任务的本职。
    void frameDumpBody();
    void snapshotDumpBody();
    void colorTestSnapshotBody();
    // 设置页三命令本体（'S'/'Q' 开关、'O' 翻页）：建页渲染链是深度最长的调用，
    // 2026-09-29 实锤在串口任务 6144 上必溢（金丝雀处决），与快照类同投 LVGL 任务。
    void settingsToggleBody();
    void settingsNextBody();
    void dispatchToLvgl(void (SerialCmd::*body)());

    AppContext* ctx_ = nullptr;
    int  snapshotUseCjkFont_ = 0;  // 'p'/'P' 探针模式
    int  lineMode_ = 0;            // 0=按键模式 1='W' WiFi 配置行输入中
    int  lineLen_  = 0;
    char lineBuf_[160] = {};
};

}  // namespace gaga

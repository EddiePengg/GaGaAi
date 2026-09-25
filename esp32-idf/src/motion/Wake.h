#pragma once

#include <cstdint>
#include <functional>

#include "motion/Qmi8658.h"

// 唤醒管理（ADR-040 摇动触发）：把 IMU 事件翻译成设备动作。
//
// ① 摇动触发：软件检测（QMI8658C 无硬件 Tap 引擎，轮询原始加速度）。
//    v2（2026-09-25 真机日志调参）：dev>0.6g 连续 ≥4 样本（~45ms）= 一个
//    "摆"（摆内 |a|>0.35g 排除自由落体），1 秒窗 ≥2 摆 = 摇动 → 触发录音。
//    区分摔/磕碰：孤立尖峰 1~2 样本成不了摆。调参用 'A' 串口原始数据流。
// ② 抬手亮屏：软件姿态状态机（挂脖场景）：
//      IDLE（静息建基线 B）→ 检到运动（|a| 偏离 1g 或方向大变）
//      → 1.5s 窗内出现"屏幕朝上稳定"（Z>0.72g、模长≈1g、与基线不同向）
//      → 亮屏 → 回 IDLE。走路晃动不亮（姿态仍朝前不朝上）。
// 检测只在息屏时跑（亮屏/录音/talk 期间跳过，省 I2C 也免误触）；
// 唤醒动作 = appState.setScreen(On) + notifyActivity（与按键亮屏同路径）。
//
// 调试：串口 'M' dump 姿态、'K' 手动翻转 Z 轴极性（设备焊装方向兜底）。
namespace gaga {

class AppState;
class Settings;

class Wake {
public:
    enum class Event : uint8_t { None, Tap, Lift };

    void begin(Qmi8658* imu, AppState* app, const Settings* settings = nullptr);

    // appTask 每 10ms 调；内部节流 50ms（息屏时检测，亮屏时 200ms 只查 tap）
    Event tick();

    // 抬手 Z 轴极性（焊装方向兜底；'K' 切换）。+1 默认：屏幕朝上 = az>0
    void flipZSign() { zSign_ = -zSign_; }
    // 'A' 原始加速度流开关（调参眼睛：每样本打一行，摇动触发只记标记不动作）
    void setStreamAccel(bool on) { streamAccel_ = on; }
    bool streamAccel() const { return streamAccel_; }
    // 自动转向（2026-09-25）：亮屏+静止时按重力面内投影绝对判向 → 回调
    // {0,180}（MADCTL 镜像反转）。朝向规则来自十场景数据集 ⑩ 段实证
    void onRotation(std::function<void(int)> cb) { onRotation_ = std::move(cb); }
    // dump IMU 敲击引擎寄存器（串口 'I' 触发）
    void dumpTapDebug();
    // 最近一次姿态（串口 'M' dump 用）
    float lastX() const { return lastX_; }
    float lastY() const { return lastY_; }
    float lastZ() const { return lastZ_; }

private:
    // 抬手状态机一步（新样本 in）；返回 true = 判定抬手
    bool liftStep(float x, float y, float z, uint32_t nowMs);
    void   resetBaseline();
    // 自动转向一步（dev,now）：静止+整 90° 落位+400ms 稳定才回调
    void   tickRotation(float dev, uint32_t now);

    Qmi8658*   imu_ = nullptr;
    AppState*  app_ = nullptr;
    const Settings* settings_ = nullptr;  // 摇动录音开关直读（nullptr=视为开）

    // ---- 节流 ----
    uint32_t   lastTickMs_ = 0;

    // ---- 抬手状态机 ----
    enum class LiftState : uint8_t { Baseline, Motion, FaceUp };
    LiftState  liftState_ = LiftState::Baseline;
    uint32_t   motionAtMs_  = 0;    // 进 Motion 的时刻（1.5s 判定窗）
    uint32_t   faceUpSince_ = 0;    // 连续"朝上稳定"起点
    float      baseX_ = 0, baseY_ = 0, baseZ_ = 1;  // 静息基线方向（单位向量）
    int        baseSamples_ = 0;    // 基线累计样本数
    int8_t     zSign_ = 1;          // Z 轴极性（'K' 可翻）

    // ---- 诊断 ----
    float lastX_ = 0, lastY_ = 0, lastZ_ = 1;
    bool   streamAccel_ = false;   // 'A' 原始数据流（调参期间抑制触发）

    // ---- 自动转向（2026-09-25 用户需求）----
    float   ax_ = 0, ay_ = 0, az_ = 1;   // 最新三轴（每 tick 刷新）
    int     rotCurDeg_     = 0;          // 当前已应用的旋转
    int     rotCandDeg_    = 0;          // 候选旋转（滞回中）
    uint32_t rotSteadySince_ = 0;        // 候选稳定起点（400ms 确认）
    std::function<void(int)> onRotation_;

    // 参数（实测可调）：运动阈值 / 朝上判定 / 稳定窗口
    static constexpr float    MOTION_A_DEV = 0.30f;   // |a| 偏离 1g 视为运动
    static constexpr float    MOTION_COS   = 0.70f;   // 与基线 cos < 0.70（≈45°）视为变向
    static constexpr float    FACEUP_Z     = 0.72f;   // Z 分量 > 0.72g = 屏幕朝上
    static constexpr float    FACEUP_BASE_COS = 0.75f;// 与基线 cos < 0.75 = 确实换过姿态
    static constexpr uint32_t MOTION_WINDOW_MS  = 2500;  // 运动后允许的判定窗
    // （2026-09-25 ⑨段数据：7 次拎起 3 次超窗放弃——1500ms 不够完成"拎起+看定"）
    static constexpr uint32_t FACEUP_HOLD_MS    = 250;   // 朝上稳定持续
    static constexpr uint32_t BASE_SAMPLE_MS    = 2000;  // 静息建基线时长
};

}  // namespace gaga

#include "motion/Wake.h"

#include <cmath>

#include "esp_log.h"

#include "compat.h"
#include "state/AppState.h"
#include "state/Settings.h"

namespace gaga {

static const char* TAG = "gaga.wake";

void Wake::begin(Qmi8658* imu, AppState* app, const Settings* settings) {
    imu_ = imu;
    app_ = app;
    settings_ = settings;
    resetBaseline();
    ESP_LOGI(TAG, "wake 就绪（IMU=%s，抬手状态机=Baseline）",
             imu_ && imu_->ready() ? "在线" : "IMU 缺席，仅按键唤醒");
}

void Wake::resetBaseline() {
    liftState_   = LiftState::Baseline;
    baseSamples_ = 0;
    baseX_ = 0; baseY_ = 0; baseZ_ = 1;
}

void Wake::dumpTapDebug() {
    if (imu_) imu_->dumpTapDebug();
}

// 主节拍：appTask 每 10ms 调用一次。
// 双击检测不受节流限制（每 10ms 采样），其余检查按各自节流。
Wake::Event Wake::tick() {
    if (imu_ == nullptr || !imu_->ready() || app_ == nullptr) return Event::None;
    const uint32_t now = millis();

    // ---- 摇动检测：不受节流限制，每 10ms 采样 ----
    // 水平分量判据（2026-09-26 用户洞察）：重力方向由低通滤波估计，
    // 运动加速度分解为"平行重力"（竖直弹跳/跑步）和"垂直重力"（水平摇动）。
    // 只有垂直分量参与摆检测——竖直弹跳被天然过滤。
    // 'A' 数据流开着时照常打原始值行。
    float ax, ay, az;
    if (imu_->readAccel(&ax, &ay, &az)) {
        const float mag = sqrtf(ax * ax + ay * ay + az * az);
        const float dev = mag > 1.0f ? mag - 1.0f : 1.0f - mag;
        ax_ = ax; ay_ = ay; az_ = az;
        // 重力低通估计（alpha=0.06，约 0.16s 时间常数 @10ms tick）
        static float gLpX = 0, gLpY = 0, gLpZ = 1;
        gLpX += 0.06f * (ax - gLpX);
        gLpY += 0.06f * (ay - gLpY);
        gLpZ += 0.06f * (az - gLpZ);
        const float gMag = sqrtf(gLpX*gLpX + gLpY*gLpY + gLpZ*gLpZ);
        // 水平分量 = 动态加速度去掉平行重力的部分
        float hDev = 0;
        if (gMag > 0.2f) {
            const float dx = ax - gLpX, dy = ay - gLpY, dz = az - gLpZ;
            const float dMag = sqrtf(dx*dx + dy*dy + dz*dz);
            const float par = (dx*gLpX + dy*gLpY + dz*gLpZ) / gMag;
            hDev = sqrtf(fmaxf(0, dMag*dMag - par*par));
        }
        // 哨兵（息屏时粗摆升档）
        tickRotation(dev, now);
        if (streamAccel_) {
            float gxd, gyd, gzd;
            const bool hasG = imu_->readGyro(&gxd, &gyd, &gzd);
            ESP_LOGI(TAG, "[A],%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                     (unsigned long)now, mag, dev, ax, ay, az,
                     hasG ? gxd : 0.0f, hasG ? gyd : 0.0f, hasG ? gzd : 0.0f);
        }
        static int streak = 0;           // 当前连续超阈样本数（摆长）
        static float streakMinMag = 99;
        static float streakPeak = 0;     // 本摆内最高 |a|
        static float swingPeak = 0;      // 本手势窗口内最大摆峰
        static float firstPeak = 0;      // 首摆峰值（持续度闸门）
        static int swings = 0;
        static uint32_t lastSwingMs = 0;
        static bool   gesturePending = false;
        static uint32_t pendingSinceMs = 0, lastFireMs = 0;
        const bool shakeArmed = (settings_ == nullptr || settings_->shakeEnabled());
        if (shakeArmed || streamAccel_) {
            // v5 判据：水平分量 >0.6g 连续 ≥3 样本 = 一个摆（摆内 |a|>0.35）
            // 手势 = 1s 内 ≥4 摆、末摆 ≥0.45×首摆、停手 500ms 确认 → 触发
            // 竖直弹跳（跑步/跳）水平分量弱 → 天然过滤；斜摇/水平摇 → 通过
            if (hDev > 0.6f) {
                streak++;
                if (mag < streakMinMag) streakMinMag = mag;
                if (mag > streakPeak) streakPeak = mag;
            } else {
                if (streak >= 3 && streakMinMag > 0.35f && !gesturePending) {
                    if (swings > 0 && now - lastSwingMs > 500) {
                        swings = 0; firstPeak = 0; swingPeak = 0;
                    }
                    swings++;
                    if (swings == 1) firstPeak = streakPeak;
                    if (streakPeak > swingPeak) swingPeak = streakPeak;
                    lastSwingMs = now;
                    if (swings >= 4 && swingPeak >= 2.5f &&
                        streakPeak >= 0.45f * firstPeak) {
                        gesturePending = true;
                        pendingSinceMs = now;
                    }
                }
                streak = 0;
                streakMinMag = 99;
                streakPeak = 0;
            }
            // 收尾 + 冷却
            if (gesturePending && dev < 0.2f &&
                now - lastSwingMs >= 500 && now - lastFireMs >= 2000) {
                gesturePending = false;
                swings = 0; firstPeak = 0; swingPeak = 0;
                lastFireMs = now;
                if (streamAccel_) {
                    ESP_LOGI(TAG, "[A] !! would-fire t=%lu（流模式：抑制触发）",
                             (unsigned long)now);
                } else {
                    ESP_LOGI(TAG, "摇动检测到 → 触发");
                    app_->notifyActivity();
                    app_->setScreen(ScreenState::On);
                    return Event::Tap;
                }
            }
            if (gesturePending && now - pendingSinceMs > 2500) {
                gesturePending = false;
                swings = 0; firstPeak = 0; swingPeak = 0;
            }
        }
    }

    // ---- 节流检查（其余功能不需要每 10ms 都跑）----
    const bool screenOn = (app_->screen() == ScreenState::On);
    // 摇动检测不受此节流管（上面每 tick 都跑）；这里管抬手/姿态：
    // 息屏 40ms（94Hz Idle 档下采样已到位）、亮屏 200ms（只查姿态）
    const uint32_t period = screenOn ? 200 : 40;
    if (lastTickMs_ != 0 && (now - lastTickMs_) < period) return Event::None;
    lastTickMs_ = now;

    // ---- 抬手（仅息屏时检测；'A' 流模式 = 干跑：亮屏时也评估但不动作；
    // 设置页"抬手亮屏"关 = 不评估，'A' 流开着例外（采集不被开关绑架））----
    const bool liftArmed = (settings_ == nullptr || settings_->liftEnabled());
    if ((screenOn || !liftArmed) && !streamAccel_) {
        resetBaseline();  // 不检测期间持续复位基线，重新开启后重新学
        return Event::None;
    }
    float x, y, z;
    if (!imu_->readAccel(&x, &y, &z)) return Event::None;
    z *= zSign_;  // 极性兜底（'K' 切换）
    lastX_ = x; lastY_ = y; lastZ_ = z;
    if (liftStep(x, y, z, now)) {
        if (streamAccel_) {
            ESP_LOGI(TAG, "[LIFT] !! would-fire t=%lu（流模式：抑制亮屏）",
                     (unsigned long)now);
        } else {
            ESP_LOGI(TAG, "[LIFT] 抬手亮屏（终态 az=%.2fg）", z);
            app_->notifyActivity();
            app_->setScreen(ScreenState::On);
        }
        resetBaseline();
        return streamAccel_ ? Event::None : Event::Lift;
    }
    return Event::None;
}

// 自动转向 v6.2：翻转方向定向 + 防抖（2026-09-26 卡死修复）。
// 原理（v6）：翻面 = 绕某轴的 180° 旋转，拿起瞬间的陀螺仪符号直接编码
// "从哪边翻上来"——与最终静止姿态无关，绕开装配坐标歧义。运动中积分
// gyro X/Y，静止确认后按 |积分| 大者（翻面轴因人而异）的符号定向；
// 积分不足则兜底用"面内重力主轴符号"。
// v6.1 病根（真机卡死实锤）：录音中横持设备（az≈0）微微摆动，面内分量
// 在 0 附近反复过零 → 兜底判向 0°/180° 连环翻转 → 全屏重绘 + 外沿光效
// 反复触发，与录音 UI 抢渲染/内存直到卡死。三重防抖：
//   ① 录音中不判向不翻转（录音界面有自己的动画，转向纯属捣乱）；
//   ② 静止判向一轮只评一次（原先过 400ms 后每 10ms 连评，日志刷屏）；
//   ③ 兜底判向加死区：面内主分量 |s|<0.35g（≈±20° 过渡区）保持现状，
//     外加 3s 翻转冷却，杜绝来回横跳。
void Wake::tickRotation(float dev, uint32_t now) {
    if (settings_ != nullptr && !settings_->autoRotateEnabled()) return;
    if (app_ == nullptr || app_->screen() != ScreenState::On ||
        app_->isRecording()) {
        rotSteadySince_ = 0;
        flipIx_ = 0;
        flipIy_ = 0;
        return;
    }
    if (dev > 0.3f) {
        // 运动中：积分翻转角速度（X/Y 双轴，dps×10ms→度）。翻面旋转轴
        // 因人而异（横滚/俯仰），只看单轴会把另一类翻法积成 0（v6 首版
        // 病根：只积 X → 用户翻法走 Y → 恒 0 → 永走兜底 → "怎么拿都不变"）
        float gx = 0, gy = 0, gz = 0;
        if (imu_->readGyro(&gx, &gy, &gz)) {
            flipIx_ += gx * 0.01f;
            flipIy_ += gy * 0.01f;
        }
        rotSteadySince_ = 0;
        return;
    }
    if (fabsf(az_) > 0.7f) {
        rotSteadySince_ = 0;   // 朝天/朝地：面内无重力分量，不判向
        return;
    }
    // 静止确认（400ms）后评一次就走，再静 400ms 才有下一轮
    if (rotSteadySince_ == 0) {
        rotSteadySince_ = now;
        return;
    }
    if (now - rotSteadySince_ < ROT_STEADY_MS) return;
    rotSteadySince_ = 0;
    int cand = rotCurDeg_;
    const float dom = (fabsf(flipIx_) >= fabsf(flipIy_)) ? flipIx_ : flipIy_;
    if (fabsf(dom) > 60.0f) {
        // 符号→方向映射待标定（拿一次正的看日志翻正负即可定死）
        cand = (dom > 0) ? 0 : 180;
        ESP_LOGI(TAG, "[ROT] 翻转积分 X=%+.0f Y=%+.0f → %d",
                 flipIx_, flipIy_, cand);
    } else {
        // 静态兜底：面内重力主轴符号（横持时 X/Y 必有一轴 ≈±1g）。
        // 死区内保持现状不翻——微摆过零来回判是 v6.1 卡死的直接根因
        const float s = (fabsf(ax_) >= fabsf(ay_)) ? ax_ : ay_;
        if (fabsf(s) >= ROT_STATIC_MIN) cand = (s < 0.0f) ? 180 : 0;
        ESP_LOGI(TAG, "[ROT] 积分不足（X=%+.0f Y=%+.0f）→ 兜底 s=%+.2f → %d",
                 flipIx_, flipIy_, s, cand);
    }
    flipIx_ = 0;
    flipIy_ = 0;
    if (cand == rotCurDeg_) return;
    if (rotLastFlipMs_ != 0 && now - rotLastFlipMs_ < ROT_COOLDOWN_MS) return;
    rotCurDeg_ = cand;
    rotLastFlipMs_ = now;
    if (onRotation_) onRotation_(cand);
}

// 抬手状态机：Baseline（学静息方向）→ Motion（在窗）→ FaceUp（稳定持证）
bool Wake::liftStep(float x, float y, float z, uint32_t nowMs) {
    const float mag = std::sqrt(x * x + y * y + z * z);
    const float magDev = (mag > 1.0f ? mag - 1.0f : 1.0f - mag);
    const float ilen = (mag > 0.01f) ? 1.0f / mag : 0.0f;
    const float ux = x * ilen, uy = y * ilen, uz = z * ilen;

    switch (liftState_) {
    case LiftState::Baseline: {
        const float cosB = ux * baseX_ + uy * baseY_ + uz * baseZ_;
        if (baseSamples_ >= 10 &&
            (magDev > MOTION_A_DEV || cosB < MOTION_COS)) {
            ESP_LOGI(TAG, "[LIFT] Baseline→Motion（magDev=%.2f cosB=%.2f 基线样本=%d）",
                     magDev, cosB, baseSamples_);
            liftState_  = LiftState::Motion;
            motionAtMs_ = nowMs;
            return false;
        }
        const float k = 1.0f / (baseSamples_ + 1);
        baseX_ += (ux - baseX_) * k;
        baseY_ += (uy - baseY_) * k;
        baseZ_ += (uz - baseZ_) * k;
        const float bl = std::sqrt(baseX_ * baseX_ + baseY_ * baseY_ + baseZ_ * baseZ_);
        if (bl > 0.01f) { baseX_ /= bl; baseY_ /= bl; baseZ_ /= bl; }
        baseSamples_++;
        return false;
    }
    case LiftState::Motion: {
        if (nowMs - motionAtMs_ > MOTION_WINDOW_MS) {
            ESP_LOGI(TAG, "[LIFT] Motion 超窗（%lums）放弃 → Baseline",
                     static_cast<unsigned long>(MOTION_WINDOW_MS));
            resetBaseline();
            return false;
        }
        if (magDev < 0.15f &&
            uz > FACEUP_Z &&
            mag > 0.85f && mag < 1.15f) {
            const float cosB = ux * baseX_ + uy * baseY_ + uz * baseZ_;
            if (cosB < FACEUP_BASE_COS) {
                ESP_LOGI(TAG, "[LIFT] Motion→FaceUp（uz=%.2f cosB=%.2f）", uz, cosB);
                liftState_   = LiftState::FaceUp;
                faceUpSince_ = nowMs;
            } else {
                ESP_LOGI(TAG, "[LIFT] Motion 稳定但与基线同向（cosB=%.2f）放弃", cosB);
                resetBaseline();
            }
        }
        return false;
    }
    case LiftState::FaceUp:
        if (magDev < 0.15f && uz > FACEUP_Z &&
            nowMs - faceUpSince_ >= FACEUP_HOLD_MS) {
            return true;
        }
        if (magDev > 0.45f || uz < 0.5f) {
            // 0.45：2026-09-25 ⑨段数据两次 FaceUp 丢失均发生在 magDev≈0.36
            // （手持微颤），0.35 阈值过严
            ESP_LOGI(TAG, "[LIFT] FaceUp 丢失（magDev=%.2f uz=%.2f）→ Baseline",
                     magDev, uz);
            resetBaseline();
        }
        return false;
    }
    return false;
}

}  // namespace gaga

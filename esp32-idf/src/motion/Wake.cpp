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

    // ---- 摇动检测：不受节流限制，每 10ms 采样（ADR-040，v2 算法见下）----
    // 'A' 数据流开着时：每样本打一行原始值（时间,模长,偏离,xyz），
    // 且触发只记 would-fire 标记不动作——离线分析阈值，不被嘎嘎声污染数据。
    float ax, ay, az;
    if (imu_->readAccel(&ax, &ay, &az)) {
        const float mag = sqrtf(ax * ax + ay * ay + az * az);
        const float dev = mag > 1.0f ? mag - 1.0f : 1.0f - mag;
        ax_ = ax; ay_ = ay; az_ = az;
        tickRotation(dev, now);
        if (streamAccel_) {
            float gx, gy, gz;
            const bool hasG = imu_->readGyro(&gx, &gy, &gz);
            ESP_LOGI(TAG, "[A],%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                     (unsigned long)now, mag, dev, ax, ay, az,
                     hasG ? gx : 0.0f, hasG ? gy : 0.0f, hasG ? gz : 0.0f);
        }
        static int streak = 0;           // 当前连续超阈样本数（摆长）
        static float streakMinMag = 99;  // 本摆内最低 |a|（自由落体甄别）
        static int swings = 0;           // 本手势累计摆数（2 摆 = 摇一下）
        static uint32_t lastSwingMs = 0; // 最近一摆完成时刻
        static bool   gesturePending = false;  // 摆数达标，等"停手"确认
        static uint32_t pendingSinceMs = 0;    // 手势完成时刻
        static uint32_t lastFireMs = 0;        // 上次触发（冷却期）
        // 设置页"摇动录音"关 = 不触发；但 'A' 流开着时照常评估并打 would-fire
        // （采集数据不被开关状态绑架）。只闸动作，不能 return——
        // 后面的抬手亮屏检测还得跑。
        const bool shakeArmed = (settings_ == nullptr || settings_->shakeEnabled());
        if (shakeArmed || streamAccel_) {
            // 摇动检测 v4：手势会话制（2026-09-25 用户反馈"摇一下就发/嘎嘎
            // 两声瞬间开关"——v3.1 摆数门槛会在手势中途触发且无冷却）。
            //   摆 = dev>0.6g 连续 ≥3 样本，摆内 |a|>0.35g；2 摆 = 摇一下。
            //   手势 = 1s 内 ≥4 摆（完整两下）→ 进入"待停手确认"。
            //   触发 = 手势完成 + 停手静止 ≥500ms（摇的过程永不中途触发，
            //   也根治"连续摇 = 开关同瞬"）；触发后冷却 2s，同一套余动不再
            //   二次触发。挂脖持续摇晃因无 500ms 静止间隙不会误判收尾。
            if (dev > 0.6f) {
                streak++;
                if (mag < streakMinMag) streakMinMag = mag;
            } else {
                if (streak >= 3 && streakMinMag > 0.35f && !gesturePending) {
                    if (swings > 0 && now - lastSwingMs > 500) {
                        swings = 0;   // 断档太久：上一场手势作废
                    }
                    swings++;
                    lastSwingMs = now;
                    if (swings >= 4) {
                        gesturePending = true;
                        pendingSinceMs = now;
                    }
                }
                streak = 0;
                streakMinMag = 99;
            }
            // 收尾：待确认 + 停手静止 ≥500ms + 冷却已过 → 触发
            if (gesturePending && dev < 0.2f &&
                now - lastSwingMs >= 500 && now - lastFireMs >= 2000) {
                gesturePending = false;
                swings = 0;
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
            // 超时作废：手势完成后 2.5s 还没停手（一直在摇）→ 作废重计
            if (gesturePending && now - pendingSinceMs > 2500) {
                gesturePending = false;
                swings = 0;
            }
        }
    }

    // ---- 节流检查（其余功能不需要每 10ms 都跑）----
    const bool screenOn = (app_->screen() == ScreenState::On);
    const uint32_t period = screenOn ? 200 : 50;
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

// 自动转向 v5.1：X 轴符号判向（极性按用户实测"完全反了"翻转，2026-09-25）。
// 定轴依据：标注双姿势数据——自然轻拿（70°）与竖直（90°+）面内重力方向
// 几乎相同（atan2 ≈175°/181°，即 ax<0），唯一稳定特征 = ax 符号。
// 规则：ax < 0（自然观看）→ 翻转 180°；ax ≥ 0（上下颠倒拿）→ 0°。
// 此前 ay 符号规则两桶均值仅 ±0.06（σ0.15）= 读噪声随机翻转，废弃。
// 平时（亮屏+静止）持续维持可读；挂脖朝地（|az|>0.7）不判向。
void Wake::tickRotation(float dev, uint32_t now) {
    if (settings_ != nullptr && !settings_->autoRotateEnabled()) return;
    if (app_ == nullptr || app_->screen() != ScreenState::On) {
        rotSteadySince_ = 0;
        return;
    }
    if (dev > 0.2f) {
        rotSteadySince_ = 0;   // 运动中不判向（防摆动途中乱翻）
        return;
    }
    if (fabsf(az_) > 0.7f) {
        rotSteadySince_ = 0;   // 朝天/朝地：面内无重力分量，不判向
        return;
    }
    const int cand = (ax_ < 0.0f) ? 180 : 0;
    if (cand != rotCandDeg_ || rotSteadySince_ == 0) {
        rotCandDeg_ = cand;
        rotSteadySince_ = now;
        return;
    }
    if (now - rotSteadySince_ >= 400 && cand != rotCurDeg_) {
        rotCurDeg_ = cand;
        ESP_LOGI(TAG, "[ROT] ax=%+.2f → %s", ax_,
                 cand == 180 ? "翻转" : "正位");
        if (onRotation_) onRotation_(cand);
    }
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

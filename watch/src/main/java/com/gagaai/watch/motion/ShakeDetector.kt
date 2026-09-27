package com.gagaai.watch.motion

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager

/**
 * "晃两下手腕"检测 v2（2026-09-27 ADR-060）：**单轴反向交替双脉冲**。
 *
 * 判据：任一轴（X/Y/Z）的相邻帧差分超阈值 = 一次脉冲；两次脉冲**同轴、
 * 方向相反**（一正一负）、落在配对窗内 = 一次指令 → [onDoubleShake]。
 * "上下甩两下"（手腕屈伸）和"左右摆两下"（偏摆）都是这个特征。
 *
 * 为什么从"合矢量差分"（v1，ADR-057）升级：
 * - 合矢量不分方向——走路/骑车颠簸都是竖直方向重复冲击，步频 ~1.5Hz
 *   两步间隔恰好落在 350ms~1400ms 配对窗里，v1 会被连走两步误触发；
 * - 单轴反向交替要求第二下甩回来，日常动作几乎凑不出（颠簸是同向的）。
 *
 * 为什么不做翻腕/握拳：翻腕直接撞 ColorOS 转腕灭屏（第一下就黑屏）；
 * 握拳在 IMU 上的信号比甩腕小一个量级，信噪比不够。
 *
 * 时序参数：
 *   [SINGLE_COOLDOWN_MS] 同一次甩动的余震去抖（加速度过零震荡 <350ms）
 *   [DOUBLE_WINDOW_MS]   两晃配对窗口：第 2 晃落在第 1 晃后 350ms~1400ms
 *   [DOUBLE_COOLDOWN_MS] 双晃触发后的冷却：连甩三下只算一次
 *
 * 生命周期纪律：只有订阅方调 [start]/[stop]，Activity onPause 一定要 stop，
 * 否则手表这颗 CPU 就一直被 sensor 唤醒着——用户对功耗极敏感。
 */
class ShakeDetector(
    context: Context,
    private val onDoubleShake: () -> Unit,
) : SensorEventListener {

    private val sm = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val acc = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val gyr = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

    /** 单脉冲触发阈值（单轴相邻帧差分，m/s²）。调试页可调。 */
    @Volatile
    var threshold = 3.0f

    companion object {
        const val SINGLE_COOLDOWN_MS = 350L
        const val DOUBLE_WINDOW_MS = 1400L
        const val DOUBLE_COOLDOWN_MS = 1500L
    }

    /** 调试页要看的实时值 */
    @Volatile
    var accX = 0f; @Volatile var accY = 0f; @Volatile var accZ = 0f
    @Volatile
    var gyrX = 0f; @Volatile var gyrY = 0f; @Volatile var gyrZ = 0f

    /** 最近一次最大轴差分幅度（调试页阈值可视化用） */
    @Volatile
    var lastDelta = 0f

    /** 触发第 1 晃的轴与方向（'x'/'y'/'z'，true=正向），调试页显示用 */
    @Volatile
    var pendingAxis = ' '
        private set
    @Volatile
    var pendingSign = false
        private set

    /** 双晃进度（调试页可视化）：true = 已有第 1 晃、在等反向第 2 晃 */
    @Volatile
    var awaitingSecond = false
        private set

    /** 第 1 晃发生时刻（awaitingSecond=true 时有意义） */
    @Volatile
    var pendingSinceMs = 0L
        private set

    /** 传感器是否真的读到了数据（用户要确认表里到底有没有这些传感器） */
    @Volatile
    var accAvailable = false
    @Volatile
    var gyrAvailable = false

    private var lastX = 0f
    private var lastY = 0f
    private var lastZ = 0f
    private var haveLast = false
    private var lastPulseAt = 0L
    private var lastDoubleAt = 0L

    fun start() {
        accAvailable = acc != null
        gyrAvailable = gyr != null
        haveLast = false
        acc?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
        gyr?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
    }

    fun stop() {
        sm.unregisterListener(this)
        awaitingSecond = false
    }

    override fun onSensorChanged(ev: SensorEvent) {
        when (ev.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                accX = ev.values[0]; accY = ev.values[1]; accZ = ev.values[2]
                if (haveLast) {
                    val dx = accX - lastX
                    val dy = accY - lastY
                    val dz = accZ - lastZ
                    lastDelta = maxOf(kotlin.math.abs(dx), kotlin.math.abs(dy), kotlin.math.abs(dz))
                    // 找最大差分轴——三轴同时动时认主导轴，避免斜甩被记成两个轴
                    var axis = 'x'; var delta = dx
                    if (kotlin.math.abs(dy) > kotlin.math.abs(delta)) { axis = 'y'; delta = dy }
                    if (kotlin.math.abs(dz) > kotlin.math.abs(delta)) { axis = 'z'; delta = dz }
                    if (kotlin.math.abs(delta) > threshold) {
                        onPulse(axis, delta > 0, System.currentTimeMillis())
                    }
                }
                lastX = accX; lastY = accY; lastZ = accZ
                haveLast = true
            }
            Sensor.TYPE_GYROSCOPE -> {
                gyrX = ev.values[0]; gyrY = ev.values[1]; gyrZ = ev.values[2]
            }
        }
    }

    private fun onPulse(axis: Char, positive: Boolean, now: Long) {
        if (now - lastDoubleAt < DOUBLE_COOLDOWN_MS) return   // 双晃后冷却
        if (now - lastPulseAt < SINGLE_COOLDOWN_MS) return    // 同一甩动的余震
        lastPulseAt = now
        if (awaitingSecond && now - pendingSinceMs <= DOUBLE_WINDOW_MS
            && axis == pendingAxis && positive != pendingSign
        ) {
            awaitingSecond = false
            lastDoubleAt = now
            onDoubleShake()
        } else {
            // 第 1 晃入账（超窗/异轴/同向的第 2 脉冲降级成新的第 1 晃）
            awaitingSecond = true
            pendingSinceMs = now
            pendingAxis = axis
            pendingSign = positive
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}

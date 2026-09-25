package com.gagaai.app.util

import android.content.Context
import android.content.SharedPreferences

object Prefs {
    private const val FILE = "gaga_prefs"
    private const val KEY_HOST = "broker_host"
    private const val KEY_PORT = "broker_port"
    private const val KEY_USER_STOPPED = "user_stopped"
    private const val KEY_PAIRED_MAC = "paired_mac"
    private const val KEY_PAIRED_NAME = "paired_name"
    private const val KEY_BATTERY_GUIDE_DONE_AT = "battery_guide_done_at"
    private const val KEY_SHOW_TERMINAL = "show_terminal"

    const val DEFAULT_PORT = 1883

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    var brokerHost: String = ""
        private set
    var brokerPort: Int = DEFAULT_PORT
        private set

    /** 用户在 App 里点过"停止服务"：看门狗/开机/任务移除一律不复活，打开 App 清零 */
    @Volatile
    var userStopped: Boolean = false
        private set

    /** 记住的鸭子 MAC（CDM 配对或任意一次连接成功后写入）：BLE 直连重连免扫描 */
    @Volatile
    var pairedMac: String = ""
        private set

    @Volatile
    var pairedName: String = ""
        private set

    /**
     * 用户在 OPPO/ColorOS 引导弹窗里点"已完成设置"的时间（0=从未确认）。
     * ColorOS 的"完全允许后台行为"没有公开 API 可读状态，只能靠用户确认 +
     * 进程被杀记录（ApplicationExitInfo）间接判断设置是否有效。
     */
    @Volatile
    var batteryGuideDoneAt: Long = 0L
        private set

    /** 是否显示主界面的终端日志卡（调试向内容，普通用户可在设置里隐藏） */
    @Volatile
    var showTerminal: Boolean = true
        private set

    @Volatile
    private var loaded = false

    @Synchronized
    fun load(context: Context) {
        val p = prefs(context)
        brokerHost = p.getString(KEY_HOST, "") ?: ""
        brokerPort = p.getInt(KEY_PORT, DEFAULT_PORT)
        userStopped = p.getBoolean(KEY_USER_STOPPED, false)
        pairedMac = p.getString(KEY_PAIRED_MAC, "") ?: ""
        pairedName = p.getString(KEY_PAIRED_NAME, "") ?: ""
        batteryGuideDoneAt = p.getLong(KEY_BATTERY_GUIDE_DONE_AT, 0L)
        showTerminal = p.getBoolean(KEY_SHOW_TERMINAL, true)
        loaded = true
    }

    @Synchronized
    fun ensureLoaded(context: Context) {
        if (!loaded) load(context)
    }

    @Synchronized
    fun saveBroker(context: Context, host: String, port: Int) {
        brokerHost = host.trim()
        brokerPort = if (port in 1..65535) port else DEFAULT_PORT
        prefs(context).edit()
            .putString(KEY_HOST, brokerHost)
            .putInt(KEY_PORT, brokerPort)
            .apply()
    }

    @Synchronized
    fun setUserStopped(context: Context, stopped: Boolean) {
        userStopped = stopped
        prefs(context).edit().putBoolean(KEY_USER_STOPPED, stopped).apply()
    }

    @Synchronized
    fun savePairedDevice(context: Context, mac: String, name: String) {
        pairedMac = mac.trim()
        pairedName = name.trim()
        prefs(context).edit()
            .putString(KEY_PAIRED_MAC, pairedMac)
            .putString(KEY_PAIRED_NAME, pairedName)
            .apply()
    }

    @Synchronized
    fun clearPairedDevice(context: Context) {
        pairedMac = ""
        pairedName = ""
        prefs(context).edit()
            .remove(KEY_PAIRED_MAC)
            .remove(KEY_PAIRED_NAME)
            .apply()
    }

    @Synchronized
    fun setBatteryGuideDone(context: Context, atMs: Long) {
        batteryGuideDoneAt = atMs
        prefs(context).edit().putLong(KEY_BATTERY_GUIDE_DONE_AT, atMs).apply()
    }

    @Synchronized
    fun setShowTerminal(context: Context, show: Boolean) {
        showTerminal = show
        prefs(context).edit().putBoolean(KEY_SHOW_TERMINAL, show).apply()
    }
}

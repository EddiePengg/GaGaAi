package com.gagaai.app.companion

import android.companion.CompanionDeviceService
import android.util.Log
import com.gagaai.app.service.KeepAlive
import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.Prefs

/**
 * 伴生设备服务（ADR-025）：与嘎嘎配对（CompanionDeviceManager.associate）后，
 * **系统**在设备出现/离开时绑定本服务——这是 Android 给手表/耳机类配套 App 的
 * 官方通道，绑定期间进程保级、允许后台起前台服务（Android 12+ 的豁免名单里）。
 *
 * 出现 = 嘎嘎在附近/已连接（用户即将使用）→ 主动确保转发服务活着（保活的第四条命）。
 * 离开 = 不做任何事：户外 BLE 断连属常态，交给 BleManager 重连循环，绝不顺手杀服务。
 *
 * 系统按 ACTION "android.companion.CompanionDeviceService" 绑定（Manifest 声明）。
 */
class GagaCompanionService : CompanionDeviceService() {

    companion object {
        private const val TAG = "gaga.companion"
    }

    @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
    override fun onDeviceAppeared(deviceAddress: String) {
        Log.i(TAG, "伴生设备出现 $deviceAddress")
        Prefs.ensureLoaded(this)
        BridgeState.log("🐥 GaGa appeared ($deviceAddress), ensuring bridge service")
        if (!Prefs.userStopped) KeepAlive.startService(this)
    }

    @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
    override fun onDeviceDisappeared(deviceAddress: String) {
        Log.i(TAG, "伴生设备离开 $deviceAddress（不动作）")
    }
}

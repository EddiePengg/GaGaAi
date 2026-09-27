package com.gagaai.app.companion

import android.companion.CompanionDeviceService
import android.content.Intent
import android.util.Log
import com.gagaai.app.service.KeepAlive
import com.gagaai.app.util.BridgeState
import com.gagaai.app.util.Prefs

/**
 * 伴生设备服务（ADR-025）：与嘎嘎配对（CompanionDeviceManager.associate）后，
 * **系统**在设备出现/离开时绑定本服务——这是 Android 给手表/耳机类配套 App 的
 * 官方通道，绑定期间进程保级、允许后台起前台服务（Android 12+ 的豁免名单里）。
 *
 * 出现 = 嘎嘎在附近/已连接（用户即将使用）→ ① 确保转发服务活着（保活的
 * 第四条命）② 记下出现地址（配对设备换地址时 MAC 直连能自动跟上）。
 * 离开 = 不做任何事：户外 BLE 断连属常态，交给 BleManager 重连循环，绝不顺手杀服务。
 *
 * 系统按 ACTION "android.companion.CompanionDeviceService" 绑定（Manifest 声明）。
 */
class GagaCompanionService : CompanionDeviceService() {

    companion object {
        private const val TAG = "gaga.companion"
        /** 设备出现时踢重连的内部广播（GagaService 注册接收） */
        const val ACTION_KICK_RECONNECT = "com.gagaai.app.KICK_RECONNECT"
    }

    @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
    override fun onDeviceAppeared(deviceAddress: String) {
        Log.i(TAG, "伴生设备出现 $deviceAddress")
        Prefs.ensureLoaded(this)
        BridgeState.log("🐥 GaGa appeared ($deviceAddress), ensuring bridge service")
        // 出现地址与已配对 MAC 不同 = 设备重启后换了随机地址（NimBLE 默认行为）
        // → 更新 MAC，直连重连立即跟上新地址，不再对着旧地址空转
        if (Prefs.pairedMac.isNotEmpty() && Prefs.pairedMac != deviceAddress) {
            BridgeState.log("🔁 GaGa 地址变化 ${Prefs.pairedMac} → $deviceAddress，更新直连目标")
            Prefs.savePairedDevice(this, deviceAddress, Prefs.pairedName.ifEmpty { "GAGA" })
        }
        if (!Prefs.userStopped) KeepAlive.startService(this)
        // 踢一脚重连：服务在跑但 BLE 还没回来说明重试在退避等待，
        // 设备已经出现了没必要等（广播直达）
        sendBroadcast(Intent(ACTION_KICK_RECONNECT).setPackage(packageName))
    }

    @Suppress("OVERRIDE_DEPRECATION", "DEPRECATION")
    override fun onDeviceDisappeared(deviceAddress: String) {
        Log.i(TAG, "伴生设备离开 $deviceAddress（不动作）")
    }
}

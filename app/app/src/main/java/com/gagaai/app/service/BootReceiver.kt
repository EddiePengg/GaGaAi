package com.gagaai.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.gagaai.app.util.Prefs

/**
 * 开机自启（ADR-024）：手机重启后把转发服务拉起来。
 * 不做的话重启后只能手动开 App——"拿起鸭子说话"直接哑火。
 * QUICKBOOT 是部分厂商（含 OPPO/HTC）的快速开机广播，与 BOOT_COMPLETED 并联。
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED,
            "android.intent.action.QUICKBOOT_POWERON",
            "com.htc.intent.action.QUICKBOOT_POWERON",
            -> {
                Prefs.ensureLoaded(context)
                if (Prefs.userStopped) return  // 用户明确停过：等打开 App 再说
                KeepAlive.startService(context)
                KeepAlive.armWatchdog(context)
                // 开机时进程短暂复活：即使 startService 被 ColorOS 自启动管控拦下，
                // 伴生观察也注册上了——嘎嘎恢复广播时系统会主动绑定拉活（ADR-025）
                KeepAlive.observeCompanionPresence(context)
            }
        }
    }
}

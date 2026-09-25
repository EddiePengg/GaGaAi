package com.gagaai.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.gagaai.app.util.Prefs

/**
 * 看门狗/自愈接收器（ADR-024）：
 * - ACTION_WATCHDOG：15 分钟一跳，服务没活就拉起，然后续期下一次；
 * - ACTION_REVIVE：滑卡片清任务后的延迟兜底拉起。
 * 用户在 App 里点过"停止服务"（Prefs.userStopped）则一律不复活——
 * 明确意图 > 保活，打开 App 会清掉该标记。
 */
class WatchdogReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        Prefs.ensureLoaded(context)
        if (Prefs.userStopped) return
        if (intent.action == KeepAlive.ACTION_REVIVE || !GagaService.isRunning) {
            KeepAlive.startService(context)
        }
        KeepAlive.armWatchdog(context)  // 无论是否拉起都续期，看门狗永远在线
        KeepAlive.observeCompanionPresence(context)  // 每 15 分钟进程复活一次，顺手重注册伴生观察
    }
}

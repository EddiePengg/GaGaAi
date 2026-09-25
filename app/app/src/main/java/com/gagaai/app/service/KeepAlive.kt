package com.gagaai.app.service

import android.app.AlarmManager
import android.app.PendingIntent
import android.companion.CompanionDeviceManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * 保活弹药库（ADR-024）：App 自身能做的四件事都在这里。
 *
 * - 看门狗闹钟：AlarmManager `setAndAllowWhileIdle` 自续期（15 分钟一跳），
 *   到点服务没活就拉起。不走 setExact——免 SCHEDULE_EXACT_ALARM 权限，对看门狗足够；
 * - 任务移除自启：滑卡片清任务后立即尝试 + 1.5s 闹钟兜底（闹钟广播带系统临时白名单，
 *   是少数允许后台起前台服务的窗口）；
 * - startService() 统一入口：ColorOS/Android 12+ 后台起前台服务可能抛
 *   ForegroundServiceStartNotAllowedException，全部吞掉，交给下一次闹钟再试——
 *   保活是概率游戏，单次失败不致命。
 */
object KeepAlive {
    private const val TAG = "gaga.keepalive"

    /** 看门狗周期：15 分钟（Doze 下可能顺延，无所谓，兜的是"永远不活"而不是分钟级 SLA） */
    private const val WATCHDOG_INTERVAL_MS = 15 * 60 * 1000L

    /** 滑卡片后闹钟兜底延迟 */
    private const val REVIVE_DELAY_MS = 1500L

    const val ACTION_WATCHDOG = "com.gagaai.app.action.WATCHDOG"
    const val ACTION_REVIVE = "com.gagaai.app.action.REVIVE"

    private const val REQ_WATCHDOG = 2001
    private const val REQ_REVIVE = 2002

    /** 拉起前台服务（吞掉后台启动限制异常，留给下一次闹钟） */
    fun startService(context: Context) {
        try {
            context.startForegroundService(GagaService.startIntent(context))
        } catch (e: Exception) {
            Log.w(TAG, "拉起服务失败（等下一次闹钟）：${e.message}")
        }
    }

    /** 布防看门狗闹钟（重复调用=顺延，幂等） */
    fun armWatchdog(context: Context) {
        val am = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        am.setAndAllowWhileIdle(
            AlarmManager.RTC_WAKEUP,
            System.currentTimeMillis() + WATCHDOG_INTERVAL_MS,
            pending(context, ACTION_WATCHDOG, REQ_WATCHDOG),
        )
    }

    /** 仅用户明确"停止服务"时调用——之后不复活，直到重新打开 App */
    fun cancelWatchdog(context: Context) {
        val am = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        am.cancel(pending(context, ACTION_WATCHDOG, REQ_WATCHDOG))
    }

    /**
     * 重新注册伴生设备"出现"观察（ADR-025）。API 33+ 起系统要求显式开启，
     * 且不保证跨重启持久——手机重启后 App 若从未运行，观察就没人注册，
     * 系统便不会在嘎嘎恢复广播时绑定拉活。所以进程每次短暂复活
     * （开机广播 / 看门狗闹钟 / 打开 App）都要重打一遍；幂等，无副作用。
     */
    fun observeCompanionPresence(context: Context) {
        if (Build.VERSION.SDK_INT < 33) return
        val cdm = context.getSystemService(CompanionDeviceManager::class.java) ?: return
        try {
            cdm.myAssociations.forEach { info ->
                runCatching { cdm.startObservingDevicePresence(info.id.toString()) }
            }
        } catch (e: Exception) {
            Log.w(TAG, "注册伴生观察失败：${e.message}")
        }
    }

    /** 滑卡片/一键清理后的自愈：立即试一次 + 闹钟兜底一次 */
    fun reviveAfterTaskRemoved(context: Context) {
        startService(context)
        val am = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        am.set(
            AlarmManager.RTC_WAKEUP,
            System.currentTimeMillis() + REVIVE_DELAY_MS,
            pending(context, ACTION_REVIVE, REQ_REVIVE),
        )
    }

    private fun pending(context: Context, action: String, requestCode: Int): PendingIntent =
        PendingIntent.getBroadcast(
            context,
            requestCode,
            Intent(context, WatchdogReceiver::class.java).setAction(action),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
}

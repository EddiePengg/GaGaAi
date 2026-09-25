package com.gagaai.app.util

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.app.ApplicationExitInfo
import android.content.ComponentName
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.widget.Toast

/**
 * OPPO/ColorOS 后台保活权限引导。
 *
 * 背景：ColorOS 的"设置 → 电池 → 应用耗电管理 → 允许完全后台行为"是私有管控
 * （powerkeeper 侧），与 Android 原生的电池优化白名单是两套系统——只开原生白名单
 * 在 ColorOS 上照样被杀。真机（ColorOS 16.1）实测：
 *   ① 没有任何公开 API 能读取"完全允许后台行为"的开关状态；
 *   ② 耗电管理相关页面全部需要签名级权限（com.oplus.permission.safe.SETTINGS /
 *      oplus.permission.OPLUS_COMPONENT_SAFE），第三方 App 无法直达；
 *   ③ 目标开关页 PowerControlActivity 未导出，连应用详情页的"耗电管理"入口
 *      都是设置 App 代跳的。
 * 所以可行方案只有：公开 API 跳应用详情页（用户再点"耗电管理"）+ 文字指引；
 * "检测"只能间接做——用 ApplicationExitInfo 判断进程上次是否被系统杀（被杀 =
 * 设置没生效），叠加用户确认时间戳避免拿历史记录反复打扰。
 */
object KeepAlivePermission {

    /** 系统杀进程的退出原因（用户主动杀/崩溃/ANR 不算——那些弹电池框会误导排查）。 */
    private val SYSTEM_KILL_REASONS = setOf(
        ApplicationExitInfo.REASON_SIGNALED,
        ApplicationExitInfo.REASON_LOW_MEMORY,
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE,
        ApplicationExitInfo.REASON_OTHER,
    )

    /** 老版 ColorOS（≤13 时代）可直达的自启动管理页；新版已全部封死，仅作尝试。 */
    private val LEGACY_DIRECT_PAGES = arrayOf(
        ComponentName("com.coloros.safecenter", "com.coloros.safecenter.permission.startup.StartupAppListActivity"),
        ComponentName("com.coloros.safecenter", "com.coloros.safecenter.startupapp.StartupAppListActivity"),
        ComponentName("com.oppo.safe", "com.oppo.safe.permission.startup.StartupAppListActivity"),
    )

    fun isOppoRom(): Boolean {
        val maker = "${Build.MANUFACTURER} ${Build.BRAND}".lowercase()
        return listOf("oppo", "oneplus", "realme").any { maker.contains(it) }
    }

    /**
     * 是否需要弹后台权限引导（v0.1.8 定稿：只在 onboarding 时弹一次）：
     * 唯一条件 = 用户从未点过「已完成设置」。
     * 确认之后哪怕又被系统杀也不再弹窗（用户明确要求只 onboarding 一次），
     * 被杀情况由 [logSystemKillsIfAny] 写进日志栏提醒。
     */
    fun needsGuide(activity: Activity): Boolean {
        if (!isOppoRom()) return false
        return Prefs.batteryGuideDoneAt == 0L
    }

    /** 上次确认之后进程被系统杀的次数（ColorOS o-kill 等）。只写日志，不弹窗。 */
    fun logSystemKillsIfAny(activity: Activity) {
        val confirmedAt = Prefs.batteryGuideDoneAt
        if (confirmedAt == 0L) return
        val kills = systemKillsAfter(activity, confirmedAt)
        if (kills > 0) {
            BridgeState.log("⚠ Killed by system $kills times since last setup — if it drops, recheck battery settings")
        }
    }

    private fun systemKillsAfter(activity: Activity, afterMs: Long): Int {
        if (Build.VERSION.SDK_INT < 30) return 0
        val am = activity.getSystemService(ActivityManager::class.java)
        return try {
            am.getHistoricalProcessExitReasons(activity.packageName, 0, 10).count {
                it.reason in SYSTEM_KILL_REASONS && it.timestamp > afterMs
            }
        } catch (_: Exception) {
            0
        }
    }

    /** 进 App 强制引导：不可点外关闭，只有"去设置"或"已完成设置"两条出路。 */
    fun maybeGuide(activity: Activity) {
        if (!needsGuide(activity)) return
        // 延到首帧后弹，避免和运行时权限框在同一帧叠加
        activity.window?.decorView?.post {
            AlertDialog.Builder(activity)
                .setTitle("Background permission needed")
                .setMessage(
                    "To keep GaGa's bridge service alive, please set up:\n\n" +
                        "1. In the App info page that opens next, tap 耗电管理 (Battery)\n" +
                        "2. Choose 完全允许后台行为 (Allow all background activity)\n" +
                        "3. Also allow auto-start for gaga ai in Settings → Apps → Auto-launch\n\n" +
                        "Come back here and tap \"Done setting up\" when finished."
                )
                .setCancelable(false)
                .setPositiveButton("Go to settings") { _, _ -> openBatteryControl(activity) }
                .setNegativeButton("Done setting up") { _, _ ->
                    Prefs.setBatteryGuideDone(activity, System.currentTimeMillis())
                }
                .show()
        }
    }

    /**
     * 跳向耗电管理。ColorOS 16 实测所有耗电页都被签名权限封死，
     * 所以主体链路是应用详情页（公开 API，必跳成功）；
     * 老版 ColorOS 的直达页能 resolve 就先试，失败自动落回。
     */
    private fun openBatteryControl(activity: Activity) {
        for (component in LEGACY_DIRECT_PAGES) {
            val intent = Intent().setComponent(component)
            if (activity.packageManager.resolveActivity(intent, 0) != null) {
                try {
                    activity.startActivity(intent)
                    Toast.makeText(
                        activity,
                        "Find gaga ai in the list, allow auto-start & background",
                        Toast.LENGTH_LONG,
                    ).show()
                    return
                } catch (_: Exception) {
                    break // 被权限拦了，走应用详情页兜底
                }
            }
        }
        try {
            activity.startActivity(
                Intent(
                    Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.parse("package:${activity.packageName}"),
                )
            )
        } catch (_: Exception) {
            // 极端 ROM 连应用详情页都屏蔽：退回系统设置首页
            activity.startActivity(Intent(Settings.ACTION_SETTINGS))
        }
        Toast.makeText(
            activity,
            "Tap 耗电管理 (Battery) → choose 完全允许后台行为",
            Toast.LENGTH_LONG,
        ).show()
    }
}

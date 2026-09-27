plugins {
    // 与 app/ 同一 AGP 版本（9.3.0：本机 Android Studio 支持的最高版本）
    id("com.android.application") version "9.3.0"
}

android {
    namespace = "com.gagaai.watch"
    compileSdk = 35
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "com.gagaai.watch"
        // 手表客户端 v0.1.0（2026-09-26 立项，ADR-051）：OPPO Watch X3
        //（OWW231，ColorOS Watch / Android 11，圆屏 466x466）。协议与设备线
        // 同源——同一条 MQTT、同一套帧格式，服务端零改动，手表就是"又一个嘎嘎设备"
        minSdk = 26          // ColorOS Watch 兼容下限；Opus 编码器需 29+，运行时探测
        targetSdk = 35
        versionCode = 14
        versionName = "0.3.3"
    }

    packaging {
        resources {
            excludes += setOf(
                "META-INF/INDEX.LIST",
                "META-INF/DEPENDENCIES",
                "META-INF/LICENSE",
                "META-INF/LICENSE.txt",
                "META-INF/license.txt",
                "META-INF/NOTICE",
                "META-INF/NOTICE.txt",
                "META-INF/notice.txt",
                "META-INF/ASL2.0",
                "META-INF/*.SF",
                "META-INF/*.DSA",
                "META-INF/*.RSA",
                "META-INF/io.netty.versions.properties",
            )
        }
    }
}

dependencies {
    // 与手机端哑管道同源：HiveMQ MQTT（ADR-044 的自管重连教训第一天就生效）
    implementation("com.hivemq:hivemq-mqtt-client:1.4.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
}

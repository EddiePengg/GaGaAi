pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

// 手表客户端（ADR-051）是与 app/、server/、esp32-idf/ 平级的独立组件：
// 自己的 Gradle 工程、自己的 applicationId、自己的 APK。
// 为什么不挂在 app/ 下面：app/ 的宪法是"永远是哑管道"，手表是设备侧客户端
// （自己组帧、走信令状态机），塞进去会让 AGENTS.md 那条铁律自相矛盾。
rootProject.name = "gaga-watch"

#pragma once

#include <cstdint>

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace gaga {

// 与 Arduino millis() 同语义的单色毫秒时钟（esp_timer 自启动计时，us → ms）
inline uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// 共享 I2C 总线互斥锁（真机踩实 2026-09-23：codec 开/关寄存器序列与 AXP2101 轮询并发
// 会把对方事务打烂——NACK/超时连环失败）。所有走 BSP I2C 总线的访问都必须持锁。
// 注意：esp_codec_dev 的 read/write（I2S 数据通路）不经过 I2C，不用持锁。
// 用递归锁：同任务嵌套加锁（初始化里再做 I2C 读写）不会死锁
inline SemaphoreHandle_t i2cMutex() {
    static SemaphoreHandle_t m = nullptr;
    if (m == nullptr) m = xSemaphoreCreateRecursiveMutex();
    return m;
}
inline void i2cLock()   { xSemaphoreTakeRecursive(i2cMutex(), portMAX_DELAY); }  // 进 I2C 临界区
inline void i2cUnlock() { xSemaphoreGiveRecursive(i2cMutex()); }                 // 出 I2C 临界区

// 任务栈统一放 PSRAM（内部 RAM 紧张：opus/NimBLE/LVGL 吃掉大半，
// 2026-09-23 实测 uplink 32KB 栈在内部 RAM 创建失败导致采集泵静默缺席）。
// 失败回落内部 RAM 并打日志。返回 pdPASS/pdFAIL。
inline BaseType_t taskCreatePsram(TaskFunction_t fn, const char* name, uint32_t stackBytes,
                                  void* arg, UBaseType_t prio, TaskHandle_t* handle,
                                  BaseType_t core) {
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, prio, handle,
                                                    core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW("gaga.task", "任务 %s PSRAM 栈创建失败，回落内部 RAM", name);
        ok = (core == tskNO_AFFINITY)
            ? xTaskCreate(fn, name, stackBytes, arg, prio, handle)
            : xTaskCreatePinnedToCore(fn, name, stackBytes, arg, prio, handle, core);
    }
    if (ok != pdPASS) {
        ESP_LOGE("gaga.task", "任务 %s 创建失败（栈 %luB）！", name,
                 static_cast<unsigned long>(stackBytes));
    }
    return ok;
}

}  // namespace gaga

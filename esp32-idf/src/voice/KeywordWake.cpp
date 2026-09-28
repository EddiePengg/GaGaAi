// 关键词唤醒（ADR-037）：esp-sr WakeNet9 "Hi,乐鑫"。
// 2026-09-28 凌晨复活：绕开 9/25 三坑——模型走 esp-sr 2.x 内置打包
// （Kconfig 选模型 → CMake 自动打包进 flash "model" 分区，不用手写 srmodels 加载），
// AFE 走 MORE_PSRAM；若再崩，TWDT_PANIC 直接打印肇事栈（取证体系已就位）。
#include "voice/KeywordWake.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "esp_partition.h"
#include "esp_afe_config.h"      // include/esp32s3 已在组件 include 路径（无前缀）
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "model_path.h"               // esp_srmodel_init（esp-sr 2.x 模型分区扫描）

#include "AppContext.h"
#include "audio/AudioPipe.h"
#include "compat.h"                   // millis() / taskCreatePsram
#include "net/LinkManager.h"
#include "rec/Recorder.h"
#include "state/AppState.h"
#include "ui/Ui.h"

namespace gaga {

static const char* TAG = "gaga.kws";

void KeywordWake::begin(AppContext* ctx) {
    ctx_ = ctx;
    ESP_LOGI(TAG, "KWS 就绪（'W'/设置页开关启动；唤醒词=\"Hi,乐鑫\"）");
}

bool KeywordWake::setEnabled(bool on) {
    if (on == enabled_) return true;
    if (on) {
        // WiFi 模式内存盘子装不下 WiFi+KWS 两家（28.6KB 实测）——BLE 模式专用
        if (ctx_->net != nullptr && strcmp(ctx_->net->activeName(), "WiFi") == 0) {
            ctx_->ui->showNote("回蓝牙模式再开唤醒");
            ESP_LOGW(TAG, "WiFi 模式下拒绝开启（内存不够两家分）");
            return false;
        }
        // 1) 音频前端上电（作业队列串行，等 mic 真正就绪）
        ctx_->audio->requestOpen(AudioPipe::RATE_REC);
        bool ready = false;
        for (int i = 0; i < 120; i++) {          // 最长 6s（作业排队+上电）
            if (ctx_->audio->micOpened()) { ready = true; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!ready) {
            ctx_->ui->showNote("麦克风上电失败");
            ctx_->audio->requestClose();
            return false;
        }
        // 2) 建 AFE：单麦格式、低成本档（省内存）、MORE_PSRAM。
        //    模型从 flash "model" 分区加载（esp-sr 2.x：Kconfig 选中的模型经
        //    movemodel.py 打包成 srmodels.bin 烧到该分区；esp_srmodel_init 扫描
        //    出清单喂给 afe_config_init）。
        //    ⚠️ 已知未解（2026-09-28 凌晨）：esp-sr 2.5.4 预编译 lib 与该头文件的
        //    srmodel_list_t 布局疑似不一致（运行时 sizeof=24 ≠ 头文件 48；loader
        //    报 "Successfully load" 但返回清单 name=NULL/num=-1 → AFE "not found"）。
        //    临时状态：KWS 管道（AFE/VAD/mic/任务/内存）全部可跑，仅唤醒词模型未挂。
        //    待办：查 espressif/esp-sr issue / 换 pin 版本重试（dev-log 有完整取证）。
        srmodel_list_t* models = esp_srmodel_init("model");
        if (models == nullptr) {
            ctx_->ui->showNote("模型分区未就绪");
            ctx_->audio->requestClose();
            return false;
        }
        {   // [dbg] 运行时分区直读 + loader 视角自检（同一编译单元视角，规避 ABI 误读）
            const esp_partition_t* pt =
                esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
            if (pt != nullptr) {
                uint8_t head[16] = {0};
                esp_partition_read(pt, 0, head, sizeof(head));
                ESP_LOGI(TAG, "[dbg] model分区 addr=%lx 头=%02x%02x%02x%02x '%c%c%c%c%c%c'",
                         (unsigned long)pt->address,
                         head[0], head[1], head[2], head[3],
                         head[4] >= 32 && head[4] < 127 ? head[4] : '?',
                         head[5] >= 32 && head[5] < 127 ? head[5] : '?',
                         head[6] >= 32 && head[6] < 127 ? head[6] : '?',
                         head[7] >= 32 && head[7] < 127 ? head[7] : '?',
                         head[8] >= 32 && head[8] < 127 ? head[8] : '?',
                         head[9] >= 32 && head[9] < 127 ? head[9] : '?');
            }
            const int idx = esp_srmodel_exists(models, (char*)"wn9_jarvis_tts");
            ESP_LOGI(TAG, "[dbg] loader视角 exists=%d（>=0 清单里真有这个模型）", idx);
        }
        afe_config_t* cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
        if (cfg == nullptr) {
            ctx_->ui->showNote("AFE 配置失败");
            ctx_->audio->requestClose();
            return false;
        }
        cfg->wakenet_model_name  = (char*)"wn9_jarvis_tts";
        cfg->memory_alloc_mode   = AFE_MEMORY_ALLOC_MORE_PSRAM;
        iface_ = esp_afe_handle_from_config(cfg);
        afe_   = iface_ ? iface_->create_from_config(cfg) : nullptr;
        heap_caps_free(cfg);
        if (afe_ == nullptr) {
            ctx_->ui->showNote("AFE 创建失败");
            ctx_->audio->requestClose();
            return false;
        }
        // 3) 拉起喂帧任务
        taskStop_ = false;
        if (taskCreatePsram(taskEntry, "gaga_kws", 6144, this, 4, &task_, 1) != pdPASS) {
            iface_->destroy(afe_);
            afe_ = nullptr;
            ctx_->audio->requestClose();
            ctx_->ui->showNote("唤醒任务起不来");
            return false;
        }
        enabled_ = true;
        ESP_LOGI(TAG, "🎙 KWS 已启动：mic 常开，本地听「Hi,乐鑫」（不上传）");
        ctx_->ui->showNote("语音唤醒已开");
        return true;
    }

    // ---- 关闭 ----
    enabled_ = false;
    taskStop_ = true;
    for (int i = 0; i < 60 && task_ != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(50));
    task_ = nullptr;
    if (afe_ != nullptr) {
        iface_->destroy(afe_);
        afe_   = nullptr;
        iface_ = nullptr;
    }
    if (!ctx_->app->isRecording()) ctx_->audio->requestClose();
    ESP_LOGI(TAG, "KWS 已关闭（前端断电回零耗）");
    ctx_->ui->showNote("语音唤醒已关");
    return true;
}

void KeywordWake::taskEntry(void* arg) {
    static_cast<KeywordWake*>(arg)->run();
}

void KeywordWake::run() {
    esp_task_wdt_add(nullptr);   // 卡死自保：喂不上狗 → 5s 整机复位
    const int chunk = iface_->get_feed_chunksize(afe_);   // 单通道每帧采样数
    // 读缓冲放 PSRAM（大块顺序数据，抽屉柜正合适）
    int16_t* stereo = static_cast<int16_t*>(
        heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int16_t* mono = static_cast<int16_t*>(
        heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stereo == nullptr || mono == nullptr) {
        ESP_LOGE(TAG, "KWS 缓冲分配失败（PSRAM）");
        enabled_ = false;
        task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    uint32_t lastMissMs = 0;
    int      micFailStreak_ = 0;
    while (!taskStop_) {
        esp_task_wdt_reset();
        // 录音/talk 期间让路（ADR-037）：不跟录音泵抢 mic
        if (ctx_->app->isRecording() || ctx_->app->isTalking()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // 立体声读入（阻塞至凑满或超时），下混单声道喂 AFE
        const int wantBytes = chunk * 2 * static_cast<int>(sizeof(int16_t));
        const int got = ctx_->audio->micRead(reinterpret_cast<uint8_t*>(stereo), wantBytes);
        if (got < wantBytes) {                 // 超时/失败残段：让出 CPU 再重试
            if (millis() - lastMissMs > 5000) {
                lastMissMs = millis();
                ESP_LOGW(TAG, "micRead 残段 %d/%dB", got, wantBytes);
            }
            // 安全带：连续失败超 3s = 麦被外部掐死（如开机自检竞态），重新上电
            if (++micFailStreak_ > 300) {      // ×10ms ≈ 3s
                micFailStreak_ = 0;
                ESP_LOGW(TAG, "mic 持续无数据，重新上电采集");
                ctx_->audio->requestOpen(AudioPipe::RATE_REC);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            vTaskDelay(pdMS_TO_TICKS(10));     // ⚠️ 不许空转：裸 continue 会把 CPU1 饿死（TWDT 实锤）
            continue;
        }
        micFailStreak_ = 0;
        AudioPipe::downmix(stereo, mono, chunk, 2);   // sel=2 双麦平均
        iface_->feed(afe_, mono);
        afe_fetch_result_t* r = iface_->fetch(afe_);
        if (r != nullptr && r->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "🎙 唤醒词命中！");
            ctx_->app->notifyActivity();
            ctx_->app->setScreen(ScreenState::On);
            // 唤醒即开录（语音助手闭环）：录音自己的"嘎"+录音界面就是全部反馈——
            // 不再加叮咚/便签（用户实锤：双重音效+多余提示是噪音，2026-09-28）。
            // 只有开不了录（已在录音/talk 中）时才保留"叮咚+我在"作答到。
            if (!ctx_->app->isRecording() && !ctx_->app->isTalking()) {
                ctx_->rec->toggle();
            } else {
                ctx_->audio->event(AudioPipe::SndEv::Received);
                ctx_->ui->showNote("我在 👂");
            }
            vTaskDelay(pdMS_TO_TICKS(1200));   // 命中防抖窗（模型自带冷却，双保险）
        }
    }
    heap_caps_free(stereo);
    heap_caps_free(mono);
    task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace gaga

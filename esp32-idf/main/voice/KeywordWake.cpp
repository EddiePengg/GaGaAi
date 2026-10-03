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
    ESP_LOGI(TAG, "KWS 就绪（'W'/设置页开关启动；唤醒词=\"你好小智\"）");
}

bool KeywordWake::beginPrefetch() {
    // 在 main_task（内部 RAM 栈）上预执行模型 mmap——esp_srmodel_init 内部
    // esp_partition_mmap 会冻结 cache 改 MMU，彼时若运行任务的栈在 PSRAM
    // 必触发 s_task_stack_is_sane_when_cache_frozen 断言（2026-09-28 晚实锤：
    // KWS 延迟到 appTask 自启后无限重启循环，backtrace 钉死本路径）。
    // static 缓存：真正的 setEnabled 复用本结果，不再二次 mmap。
    return modelsCached() != nullptr;
}

srmodel_list_t* KeywordWake::modelsCached() {
    static srmodel_list_t* s_models = nullptr;
    if (s_models == nullptr) {
        s_models = esp_srmodel_init("model");
        if (s_models != nullptr) {
            ESP_LOGI(TAG, "模型清单已预取（mmap 于安全栈）");
        }
    }
    return s_models;
}

bool KeywordWake::setEnabled(bool on) {
    if (on == enabled_) return true;
    if (on) {
        // WiFi 模式硬拒（2026-09-29 实锤改判）：昨晚"崩了再改"拍板时 WiFi 裁剪
        // 后实测剩 10.7K；今日差分实锤 WiFi 常驻占 ~30K（MAC 帧缓冲 11.9K +
        // MQTT 任务栈 6.4K + netif/lwIP 若干），余 9K 叠 AFE 8~11K → 刷屏 DMA
        // 必崩（用户亲历花屏卡死）。BLE 模式 39K 无此虑，回 BLE 自动恢复。
        if (ctx_->net != nullptr && strcmp(ctx_->net->activeName(), "WiFi") == 0) {
            ctx_->ui->showNote("WiFi 模式不支持唤醒，回蓝牙自动开");
            ESP_LOGW(TAG, "WiFi 模式拒绝开启唤醒（30K 常驻 + AFE 8~11K 必崩）");
            return false;
        }
        // 1) 音频前端上电（作业队列串行，等 mic 真正就绪）
        ESP_LOGI(TAG, "[dma] KWS 上电前 DMA池=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)));
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
        //    清单经 modelsCached() 预取——首次 mmap 发生在 app_main/main_task
        //    （内部 RAM 栈，见 beginPrefetch 注释），此处不在本栈碰 mmap。
        srmodel_list_t* models = modelsCached();
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
            const int idx = esp_srmodel_exists(models, (char*)"wn9_nihaoxiaozhi_tts");
            ESP_LOGI(TAG, "[dbg] loader视角 exists=%d（>=0 清单里真有这个模型）", idx);
        }
        afe_config_t* cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
        if (cfg == nullptr) {
            ctx_->ui->showNote("AFE 配置失败");
            ctx_->audio->requestClose();
            return false;
        }
        cfg->wakenet_model_name  = (char*)"wn9_nihaoxiaozhi_tts";
        cfg->memory_alloc_mode   = AFE_MEMORY_ALLOC_MORE_PSRAM;
        // afe_ringbuf_size 保持默认（2026-09-29 实测回滚）：试调 8 帧不但没省
        // （DMA 池照吃 10.8K——AFE 的大头是它自建 I2S 的 DMA 缓冲，不在
        // ringbuf 字段管辖），还引发 FEED 环满/空交替抖动、音频帧丢失（唤醒
        // 率反降）。此路不通，唤醒共存另寻他法（刷屏块高/预留池扩容）。
        iface_ = esp_afe_handle_from_config(cfg);
        afe_   = iface_ ? iface_->create_from_config(cfg) : nullptr;
        heap_caps_free(cfg);
        if (afe_ == nullptr) {
            ctx_->ui->showNote("AFE 创建失败");
            ctx_->audio->requestClose();
            return false;
        }
        // 唤醒灵敏度（2026-09-28）：默认阈值偏高，用户正常音量喊 Jarvis
        // 多次不命中（昨晚偶发能应 = 音量/时机踩线）。0.4~0.9999，越低越灵
        // （误唤醒同步上升）。0.6 实测仍偏难叫 → 0.5（用户拍板先试），误唤
        // 醒若烦人再回调。
        if (iface_->set_wakenet_threshold != nullptr) {
            iface_->set_wakenet_threshold(afe_, 0, 0.5f);
            ESP_LOGI(TAG, "唤醒阈值 → 0.5（0.6 仍难叫，降档提灵）");
        }
        ESP_LOGI(TAG, "[dma] AFE 创建后 DMA池=%u 内部=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(esp_get_free_internal_heap_size()));
        // 3) 拉起喂帧任务。
        // ⚠️ 栈必须内部 RAM（2026-09-28 Cache error 实锤）：wakenet 的 TIE
        // SIMD 汇编 + 系统保存向量寄存器上下文时，PSRAM 栈在特定 SP 位置
        // 触发 MMU entry fault（backtrace 钉死：xthal_save_extra_nw ← SysTick
        // ← KWS PSRAM 栈上的 dl_tie728 SIMD）。KWS 是唯一跑第三方 SIMD
        // 汇编的任务，牺牲 6KB 内部 RAM 换稳定；泵/talk 的栈继续在 PSRAM。
        // 开机/boot 早期内部堆碎片可能凑不齐连续 6KB → 回落 PSRAM 保底
        // （宁可用旧风险栈也不能"起不来"——完整不可用的代价更大）。
        taskStop_ = false;
        // 优先级 3（2026-09-28 晚，原 4）：与 UI 任务（gaga_app=4）平级会
        // 平分 CPU，桌面滚动明显掉帧；AFE 推理让 UI 半拍无感（唤醒判据
        // 是多帧累积，慢几毫秒不丢词），但 LVGL 抢不过 KWS 是秒级卡顿。
        BaseType_t okCore = xTaskCreatePinnedToCore(
            taskEntry, "gaga_kws", 6144, this, 3, &task_, 1);
        if (okCore != pdPASS) {
            ESP_LOGW(TAG, "内部 RAM 栈创建失败（碎片），回落 PSRAM 栈保底");
            okCore = taskCreatePsram(taskEntry, "gaga_kws", 6144, this, 3, &task_, 1);
        }
        if (okCore != pdPASS) {
            iface_->destroy(afe_);
            afe_ = nullptr;
            ctx_->audio->requestClose();
            ctx_->ui->showNote("唤醒任务起不来");
            return false;
        }
        enabled_ = true;
        ESP_LOGI(TAG, "🎙 KWS 已启动：mic 常开，本地听「你好小智」（不上传）");
        ctx_->ui->showNote("语音唤醒已开");
        return true;
    }

    // ---- 关闭 ----
    enabled_ = false;
    taskStop_ = true;
    for (int i = 0; i < 60 && task_ != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (task_ != nullptr) {
        // 2026-09-28 开关崩溃实锤：任务卡在 i2s_channel_read（关麦瞬间驱动
        // 可能不返回），3s 等不到退出就 destroy afe = 野指针 + TWDT 咬死 +
        // 内存雪崩。宁可强杀任务（释放栈/TWDT 除名），再销毁 AFE。
        ESP_LOGW(TAG, "KWS 任务未按期退出，强杀后销毁 AFE");
        vTaskDelete(task_);
        task_ = nullptr;
        vTaskDelay(pdMS_TO_TICKS(20));   // 让删除落地
    }
    if (afe_ != nullptr) {
        iface_->destroy(afe_);
        afe_   = nullptr;
        iface_ = nullptr;
    }
    // 引用计数时代（2026-09-28）：无条件还掉自己的引用——若录音进行中，
    // 计数仍 >0，前端不会断电（计数器代管"礼让"语义，判断分支删除）。
    ctx_->audio->requestClose();
    ESP_LOGI(TAG, "KWS 已关闭（引用归还；前端是否断电由计数归零决定）");
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
    int      powerRetries_  = 0;   // 安全带自救计数（局部变量随任务生命周期）：2 次快档后进慢档
    uint32_t slowRetryAtMs_ = 0;   // 慢速重试的下一允许时刻（30s 间隔）
    while (!taskStop_) {
        esp_task_wdt_reset();
        // 录音/talk 期间让路（ADR-037）：不跟录音泵抢 mic
        if (ctx_->app->isRecording() || ctx_->app->isTalking()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // 前端被 notify/talk 借走升 24k 档期间挂起（2026-09-28 晚速率栈配套）：
        // 此时喂 AFE 的是错速垃圾数据，且喇叭在响唤醒无意义；等档位回落
        // 16k 自动复工。顺带挡住安全带在 24k 窗口里把档位拽回 16k 的打架。
        if (ctx_->audio->micRate() != AudioPipe::RATE_REC) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        // 立体声读入，下混单声道喂 AFE。带总预算的轮询凑满（同泵的
        // micReadCycle 姿势，2026-09-28）：单轮 100ms——驱动异常时 timeout
        // 可能失效，单次 1s 阻塞意味着任务可被卡死 1s 以上直至 TWDT；
        // 每轮 100ms 内必然回来喂狗/响应 taskStop_，总预算 1.2s 兜底。
        const int wantBytes = chunk * 2 * static_cast<int>(sizeof(int16_t));
        int got = 0;
        const uint32_t readDeadline = millis() + 1200;
        while (got < wantBytes && millis() < readDeadline) {
            esp_task_wdt_reset();   // 读循环内也要喂（驱动卡超时之外的路径）
            const int n = ctx_->audio->micRead(
                reinterpret_cast<uint8_t*>(stereo) + got, wantBytes - got, 100);
            if (n <= 0) break;      // 通路断/读空失败：交残段逻辑
            got += n;
        }
        if (got < wantBytes) {                 // 超时/失败残段：让出 CPU 再重试
            if (millis() - lastMissMs > 5000) {
                lastMissMs = millis();
                ESP_LOGW(TAG, "micRead 残段 %d/%dB", got, wantBytes);
            }
            // 安全带：连续失败超 3s = 麦被外部掐死（如开机自检竞态），重新上电。
            // 两档策略（2026-09-28 终版）：头 2 次快速自救（间隔短，覆盖瞬时
            // 断电竞态）；仍失败进入慢速重试——每 30s 试一次不设上限（用
            // requestReopen：使用权已登记，重开不动引用计数）。彻底告别
            // "两轮救不回就永久摆烂"的间歇性装死（用户实锤：喊不应周期律）。
            // ⚠️ 仅当自己仍是栈顶档（16k）才救：notify/talk 的 24k 窗口里
            // mic 空窗是正常换档，拽回 16k = 两边打架（速率栈改造配套）。
            if (++micFailStreak_ > 300 &&          // ×10ms ≈ 3s
                ctx_->audio->micRate() == AudioPipe::RATE_REC) {
                micFailStreak_ = 0;
                ++powerRetries_;
                if (powerRetries_ <= 2) {
                    ESP_LOGW(TAG, "mic 持续无数据，重新上电采集（第 %d 次）", powerRetries_);
                    ctx_->audio->requestReopen(AudioPipe::RATE_REC);
                    vTaskDelay(pdMS_TO_TICKS(500));
                } else if (millis() >= slowRetryAtMs_) {
                    slowRetryAtMs_ = millis() + 30000;
                    ESP_LOGW(TAG, "mic 断流慢速重试（第 %d 次，30s 间隔）", powerRetries_);
                    ctx_->audio->requestReopen(AudioPipe::RATE_REC);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));     // ⚠️ 不许空转：裸 continue 会把 CPU1 饿死（TWDT 实锤）
            continue;
        }
        // 麦恢复：清零自救计数（下次断流重新享受快速档）
        micFailStreak_ = 0;
        powerRetries_  = 0;
        slowRetryAtMs_ = 0;
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

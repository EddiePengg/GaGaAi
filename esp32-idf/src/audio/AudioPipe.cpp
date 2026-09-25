#include "AudioPipe.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "snd_quack.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.audio";

// 提示音参数：1kHz 正弦；按下提示音加大音量时长（2026-09-23 用户反馈听不到）
static constexpr int BEEP_FREQ_HZ  = 1000;
static constexpr int BEEP_ON_MS    = 150;   // 一声 150ms（原 100ms 太短被忽略）
static constexpr int BEEP_GAP_MS   = 80;
static constexpr float BEEP_AMP    = 0.85f; // 振幅（满幅比例，原 0.5 偏小）
static constexpr float MIC_GAIN_DB = 24.0f;  // 出厂 Brookesia 配方（CODEC_DEFAULT_ADC_VOLUME）
static constexpr uint16_t MIC_GAIN_MASK = 0x3;  // 物理 MIC1|MIC2（bsp_extra_factory.h）

// I2S 引脚（本机实测定案 2026-09-23，docs/hardware.md §引脚定义）
// MCLK=GPIO16（1.75C 原理图接线）：决定性实验——MCLK=42 时 ES7210 采集全零
// （DIN10 全程无翻转），切 16 立刻出数（RMS 数百随声音起伏）；全链路
// （录音→ASR→飞书）已真实打通。1.75 资料里"MCLK=42 唯一一路"的说法对本机不适用。
#define GAGA_I2S_GPIO_CFG {                 \
        .mclk = GPIO_NUM_16,                \
        .bclk = GPIO_NUM_9,                 \
        .ws   = GPIO_NUM_45,                \
        .dout = GPIO_NUM_8,                 \
        .din  = GPIO_NUM_10,                \
        .invert_flags = {                   \
            .mclk_inv = false,              \
            .bclk_inv = false,              \
            .ws_inv   = false,              \
        },                                  \
    }

// ---------------------------------------------------------------- 初始化 ----
// 建 I2S 通道对 + ES8311/ES7210 codec + 音频作业线程（重复调用幂等）
bool AudioPipe::begin() {
    if (spk_ != nullptr && mic_ != nullptr) return true;

    // 1) 自建 I2S 通道对：TX/RX 都是 STD 立体声（出厂 bsp_audio_init_voice_24k
    //    逐项照抄，速率参数化）。约束③：槽位/模式此后不再变。
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chanCfg.auto_clear = true;  // DMA 欠载自动补零静默
    i2cLock();  // codec 初始化寄存器序列独占 I2C 总线（compat.h）

    esp_err_t err = i2s_new_channel(&chanCfg, &txChan_, &rxChan_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        i2cUnlock();
        return false;
    }

    // TX：STD 立体声（播放 ES8311；din 用不到）
    i2s_std_config_t txCfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE_REC),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = GAGA_I2S_GPIO_CFG,
    };
    txCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // 出厂配方
    txCfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    // RX：STD 立体声（ES7210 双麦：L=MIC1/R=MIC2，姿势 A——官方 05_Spec_Analyzer）
    i2s_std_config_t rxCfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE_REC),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = GAGA_I2S_GPIO_CFG,
    };
    rxCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rxCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;

    err = i2s_channel_init_std_mode(txChan_, &txCfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "TX STD init: %s", esp_err_to_name(err));
    if (err == ESP_OK) err = i2s_channel_init_std_mode(rxChan_, &rxCfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "RX STD init: %s", esp_err_to_name(err));
    if (err == ESP_OK) err = i2s_channel_enable(txChan_);
    if (err == ESP_OK) err = i2s_channel_enable(rxChan_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable: %s", esp_err_to_name(err));
        i2cUnlock();
        return false;
    }
    // 2) esp_codec_dev 数据接口（公开 API，与 BSP 同款组装）
    audio_codec_i2s_cfg_t i2sCfg = {
        .port      = I2S_NUM_1,
        .rx_handle = rxChan_,
        .tx_handle = txChan_,
    };
    dataIf_ = audio_codec_new_i2s_data(&i2sCfg);
    if (dataIf_ == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data 失败");
        i2cUnlock();
        return false;
    }

    // 3) ES8311 扬声器 codec（配置逐项照抄 BSP bsp_audio_codec_speaker_init）
    const audio_codec_gpio_if_t* gpioIf = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2cCfg = {
        .port = I2S_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t* ctrlIf = audio_codec_new_i2c_ctrl(&i2cCfg);
    esp_codec_dev_hw_gain_t gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 };
    es8311_codec_cfg_t es8311Cfg = {
        .ctrl_if     = ctrlIf,
        .gpio_if     = gpioIf,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = GPIO_NUM_46,      // NS4150B CTRL（高有效）
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = gain,
    };
    const audio_codec_if_t* es8311Dev = es8311_codec_new(&es8311Cfg);
    esp_codec_dev_cfg_t spkDevCfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311Dev,
        .data_if  = dataIf_,
    };
    spk_ = esp_codec_dev_new(&spkDevCfg);
    ESP_LOGI(TAG, "speaker codec (ES8311): %s", spk_ ? "ok" : "FAIL");

    // 4) ES7210 麦克风 codec（配置逐项照抄 BSP bsp_audio_codec_microphone_init）
    audio_codec_i2c_cfg_t micI2cCfg = {
        .port = I2S_NUM_1,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t* micCtrlIf = audio_codec_new_i2c_ctrl(&micI2cCfg);
    // ES7210 姿势 A：MIC1|MIC2 双麦非 TDM（官方 05 同款）
    es7210_codec_cfg_t es7210Cfg = {
        .ctrl_if = micCtrlIf,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t* es7210Dev = es7210_codec_new(&es7210Cfg);
    esp_codec_dev_cfg_t micDevCfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210Dev,
        .data_if  = dataIf_,
    };
    mic_ = esp_codec_dev_new(&micDevCfg);
    ESP_LOGI(TAG, "mic codec (ES7210): %s", mic_ ? "ok" : "FAIL");
    i2cUnlock();

    if (beepQueue_ == nullptr) {
        beepQueue_ = xQueueCreate(8, sizeof(Job));
        taskCreatePsram(audioTask, "gaga_audio", 4096, this, 2, &beepTask_, tskNO_AFFINITY);
    }

    // 音频前端按需上电（ADR-027，推翻 ADR-023 的"双工常开"——待机功耗不可接受，
    // 用户拍板：麦克风用的时候再开）：会话开始才上电（spk+mic 配对舞蹈一次做完，
    // 冷路径与掐滴竞态都被"先开后播"的作业次序根治），闲置断电回零耗。
    // 开机由 app_main 走一遍 上电→冒烟滴→断电 做硬件自检。
    ESP_LOGI(TAG, "音频前端按需上电（会话时上电 / 闲置时断电）");
    return true;
}

// 双工同开关（官方 bsp_extra_codec_set_fs 姿势）：mic+spk 必须同速率（约束②），
// 换档=两路一起关再一起开。已有同档则幂等。
bool AudioPipe::duplexOpen(int rate) {
    if (micOpened_ && spkOpened_ && micRate_ == rate && spkRate_ == rate) return true;
    micClose();
    spkClose();
    if (!spkOpen(rate)) return false;
    if (!micOpen(rate)) return false;
    return true;
}

// ---------------------------------------------------------------- 采集 ----
// 打开 ES7210 并配 24dB 增益；收尾做一次 REG0E 让线兜底（约束⑤）
bool AudioPipe::micOpen(int rate) {
    if (micOpened_) return micRate_ == rate;
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = rate;
    fs.channel         = MIC_CHANNELS;   // 立体声 2 槽（姿势 A）
    fs.bits_per_sample = 16;
    i2cLock();
    int rc = esp_codec_dev_open(mic_, &fs);
    if (rc == ESP_CODEC_DEV_OK) {
        // 出厂配方：物理 MIC1|MIC2 增益 24dB
        rc = esp_codec_dev_set_in_channel_gain(mic_, MIC_GAIN_MASK, MIC_GAIN_DB);
    }
    i2cUnlock();
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open @%d 失败 rc=%d（可能与播放侧采样率冲突）", rate, rc);
        return false;
    }
    micOpened_ = true;
    micRate_   = rate;
    spkAdcPowerDown();  // 约束⑤兜底：采集开始前确保 ASDOUT 高阻（幂等）
    ESP_LOGI(TAG, "mic open @%dHz 立体声", rate);
    return true;
}

// 关采集；若有延期的 spkClose 在此刻真正落地（约束④）
void AudioPipe::micClose() {
    if (!micOpened_) return;
    i2cLock();
    esp_codec_dev_close(mic_);
    i2cUnlock();
    micOpened_ = false;
    micRate_   = 0;
    ESP_LOGI(TAG, "mic closed");
    // 延期关闭的扬声器在此真正落地（TX 关早了 RX 会断粮，见约束④）
    if (spkWantClose_) {
        spkWantClose_ = false;
        spkClose();
    }
}

// 阻塞读一截 PCM（16bit 立体声交织）；返回实际字节数（<0 失败）。
// 持通路锁：uplink/KWS 等多任务会并发读，I2S RX 通道并发读会踩 DMA 队列
int AudioPipe::micRead(uint8_t* buf, int maxBytes) {
    if (!micOpened_ || rxChan_ == nullptr) return -1;
    size_t got = 0;
    const esp_err_t err = i2s_channel_read(rxChan_, buf, maxBytes, &got, 1000);
    if (err != ESP_OK) return -1;
    return static_cast<int>(got);
}

// ---------------------------------------------------------------- 播放 ----
// 开 ES8311 播放 + 拉高 PA；es8311_open 会把 ADC 重新上电，收尾必须 REG0E 让线（约束⑤）
bool AudioPipe::spkOpen(int rate) {
    if (spkOpened_) return spkRate_ == rate;
    // PA 先于 codec open 拉高（官方 Arduino 线姿势：init 前就拉高并保持）——
    // 给 NS4150B 留建立时间。注意：读回电平要用 INPUT_OUTPUT（纯输出时
    // gpio_get_level 输入通路断开恒 0，2026-09-23 曾误判成"PA 没驱动"，见 ADR-023）。
    gpio_set_direction(GPIO_NUM_46, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(GPIO_NUM_46, 1);
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = rate;
    fs.channel         = 2;   // 槽位恒立体声（约束③）：ES8311 以立体声打开，内容 L=R
    fs.bits_per_sample = 16;
    i2cLock();
    const int rc = esp_codec_dev_open(spk_, &fs);
    if (rc == ESP_CODEC_DEV_OK) esp_codec_dev_set_out_vol(spk_, volume_);
    i2cUnlock();
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "spk open @%d 失败（可能与采集侧采样率冲突）", rate);
        return false;
    }
    // PA 使能（NS4150B CTRL=GPIO46 高有效）：open 前已拉高
    spkOpened_ = true;
    spkRate_   = rate;
    spkAdcPowerDown();  // 约束⑤：open 里 ADC 被重新上电，立刻再关（ASDOUT 高阻让线给 ES7210）
    ESP_LOGI(TAG, "spk open @%dHz 2ch（L=R 同相），PA46=%d", rate, gpio_get_level(GPIO_NUM_46));
    return true;
}

// 关播放、断 PA；mic 还开着就先登记延期，等 micClose 再落地（约束④）
void AudioPipe::spkClose() {
    if (!spkOpened_) return;
    if (micOpened_) {
        // mic 开着：TX 一关 RX 断粮（约束④），登记延期，等 micClose 落地
        spkWantClose_ = true;
        ESP_LOGI(TAG, "spk close 延期（mic 开着）");
        return;
    }
    i2cLock();
    esp_codec_dev_close(spk_);
    i2cUnlock();
    gpio_set_level(GPIO_NUM_46, 0);  // PA 断电省电（与 spkOpen 的显式拉高配对）
    spkOpened_ = false;
    spkRate_   = 0;
    ESP_LOGI(TAG, "spk closed，PA=0");
}

// 写立体声 PCM（16bit 交织）；返回已写字节数（<0 失败）。
// 持通路锁：esp_codec_dev 非线程安全（audioTask/串口 'B' 可并发进）
int AudioPipe::spkWrite(const uint8_t* buf, int bytes) {
    if (!spkOpened_) return -1;
    const int ret = esp_codec_dev_write(spk_, const_cast<uint8_t*>(buf), bytes);
    return ret == ESP_CODEC_DEV_OK ? bytes : -1;
}

// 写单声道 PCM：内部复制成 L=R 立体声再写（槽位恒立体声，约束③）
int AudioPipe::spkWriteMono(const int16_t* mono, int samples) {
    if (!spkOpened_) return -1;
    // 分块交织成 L=R 立体声（栈上小块，避免大堆分配）
    static int16_t stereo[240 * 2];
    int done = 0;
    while (done < samples) {
        const int chunk = (samples - done) > 240 ? 240 : (samples - done);
        for (int i = 0; i < chunk; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = mono[done + i];
        }
        const int wrote = spkWrite(reinterpret_cast<const uint8_t*>(stereo),
                                   chunk * 2 * sizeof(int16_t));
        if (wrote <= 0) break;
        done += wrote / (2 * sizeof(int16_t));
    }
    return done;
}

// ES8311 REG0E 下电：让 ASDOUT 高阻让线给 ES7210——每次 spkOpen/micOpen 后必调（约束⑤）
void AudioPipe::spkAdcPowerDown() {
    if (spk_ == nullptr) return;
    i2cLock();
    // ES8311_SYSTEM_REG0E = 0x0E：0x00 = PGA + ADC 调制器下电（ASDOUT 高阻）
    const int rc = esp_codec_dev_write_reg(spk_, 0x0E, 0x00);
    i2cUnlock();
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 REG0E 下电写失败 rc=%d", rc);
    }
}

// 寄存器回读自检（对照 esp_codec_dev 驱动写入的期望值；全零排查的眼睛）
void AudioPipe::dumpCodecRegs() {
    // ES7210 关键寄存器（es7210.c 初始化序列的落点）
    static const uint8_t es7210Regs[] = {
        0x00, 0x01, 0x02, 0x07, 0x08, 0x11, 0x12,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x4B, 0x4C,
    };
    // ES8311：REG0D（系统电源）/ REG0E（ADC 电源，期望 0x00）/ REG31（DAC mute）/
    //         REG32（DAC 音量）/ REG12（DAC 使能）
    static const uint8_t es8311Regs[] = { 0x0D, 0x0E, 0x12, 0x31, 0x32 };

    i2cLock();
    if (mic_ != nullptr && micOpened_) {
        printf("[dump] ES7210:");
        for (uint8_t reg : es7210Regs) {
            int v = -1;
            esp_codec_dev_read_reg(mic_, reg, &v);
            printf(" %02X=%02X", reg, v & 0xFF);
        }
        printf("\n");
    } else {
        printf("[dump] ES7210 未 open（先 'r' 开录再 'e'）\n");
    }
    if (spk_ != nullptr && spkOpened_) {
        printf("[dump] ES8311:");
        for (uint8_t reg : es8311Regs) {
            int v = -1;
            esp_codec_dev_read_reg(spk_, reg, &v);
            printf(" %02X=%02X", reg, v & 0xFF);
        }
        // PA 使能脚电平（NS4150B CTRL=GPIO46，高有效；播放时应为高）
        printf(" PA46=%d", gpio_get_level(GPIO_NUM_46));
        printf("\n");
    } else {
        printf("[dump] ES8311 未 open\n");
    }
    i2cUnlock();
}

// 测量用长音：直出 1kHz 正弦 ms 毫秒（不走 beep 队列；debug 用，调用方阻塞等待）
// 播放中段内嵌 GPIO 活动探测（DOUT8/BCLK9/WS45/MCLK16 翻转计数）——
// 数字链路是否在跑一眼看穿（串口任务会被长音阻塞，外部探针赶不上）
int AudioPipe::playToneDebug(int ms) {
    if (!spkOpened_ && !duplexOpen(RATE_REC)) return -1;
    dumpCodecRegs();  // 播放前 dump 一帧（REG31 mute / PA / REG0D 电源）
    const int total = RATE_REC * ms / 1000;
    static int16_t pcm[1600];  // 100ms 一块
    bool probed = false;
    for (int base = 0; base < total; base += 1600) {
        const int chunk = (total - base) > 1600 ? 1600 : (total - base);
        for (int i = 0; i < chunk; i++) {
            const float t = static_cast<float>(base + i) / RATE_REC;
            pcm[i] = static_cast<int16_t>(sinf(2.0f * 3.14159265f * 1000.0f * t) * 0.5f * 32767.0f);
        }
        int left = chunk;
        const int16_t* p = pcm;
        while (left > 0) {
            const int wrote = spkWriteMono(p, left);
            if (wrote <= 0) break;
            p += wrote;
            left -= wrote;
        }
        if (!probed && base >= 1600) {  // 第二块（~200ms 处）探测
            probed = true;
            static constexpr gpio_num_t kPins[] = { GPIO_NUM_16, GPIO_NUM_9, GPIO_NUM_45, GPIO_NUM_8 };
            static const char* kNames[] = { "MCLK16", "BCLK9", "WS45", "DOUT8" };
            printf("[tone] 播放中 GPIO 翻转（200ms）:");
            for (int k = 0; k < 4; k++) {
                int last = gpio_get_level(kPins[k]);
                uint32_t flips = 0;
                const uint32_t start = gaga::millis();
                while (gaga::millis() - start < 200) {
                    const int v = gpio_get_level(kPins[k]);
                    if (v != last) { flips++; last = v; }
                }
                printf(" %s=%lu", kNames[k], static_cast<unsigned long>(flips));
            }
            printf("\n");
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // 尾音
    // 调试路径不自动关通路（'B' 长音后常接 'g' GPIO 探针，需通路留活）；
    // 会话断电由调用方 requestClose 兜底（ADR-027 按需上电模型）
    return total;
}

// GPIO 活动探测（I2S 引脚翻转计数，200ms 窗口）——播放/采集链路自证的仪器
void pinActivityProbeLog(const char* tag);
void pinActivityProbeLog(const char* tag) {
    static constexpr gpio_num_t kPins[] = { GPIO_NUM_16, GPIO_NUM_9, GPIO_NUM_45, GPIO_NUM_8 };
    static const char* kNames[] = { "MCLK16", "BCLK9", "WS45", "DOUT8" };
    printf("[%s] GPIO 翻转（200ms）:", tag);
    for (int k = 0; k < 4; k++) {
        int last = gpio_get_level(kPins[k]);
        uint32_t flips = 0;
        const uint32_t start = gaga::millis();
        while (gaga::millis() - start < 200) {
            const int v = gpio_get_level(kPins[k]);
            if (v != last) { flips++; last = v; }
        }
        printf(" %s=%lu", kNames[k], static_cast<unsigned long>(flips));
    }
    printf("\n");
}

// 用户音量设置（设置页 slider / 串口 'V'）：存值；播放通路开着就立即下发。
// ES8311 out vol 是数字 DAC 音量，播放中实时变化无爆音；不开锁等下次 spkOpen 生效
void AudioPipe::setVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    volume_ = pct;
    if (spkOpened_ && spk_ != nullptr) {
        i2cLock();
        esp_codec_dev_set_out_vol(spk_, volume_);
        i2cUnlock();
    }
    ESP_LOGI(TAG, "音量 → %d%%", volume_);
}

// ---------------------------------------------- 音频作业（FIFO 串行执行，ADR-027）----
// 上电/播放/断电 全部进同一条队列、同一个任务里顺序做——并发掐滴、冷路径两类事故的根治点。
// 入队 n 声"滴"（talk 态扬声器被下行占用 / 提示音总开关关，跳过不播）
void AudioPipe::beep(int count) {
    if (talkActive_) {  // talk 态扬声器被下行占用，提示音空转
        ESP_LOGI(TAG, "beep x%d（talk 态跳过）", count);
        return;
    }
    if (!beepEnabled_) {
        ESP_LOGI(TAG, "beep x%d（提示音已关）", count);
        return;
    }
    enqueue(Job{0, static_cast<uint16_t>(count)});
}

// 统一事件出口（2026-09-25 用户定稿，音效方案选择已废除）：
// 开录/发出 = 嘎（真鸭叫采样）；收到消息 = 叮咚双音（与"嘎"明确区分——
// 用户反馈：清一色嘎会误以为重新录了一遍，其实是来消息）；错误 = 噗噗。
void AudioPipe::event(SndEv ev) {
    if (talkActive_ || !beepEnabled_) return;
    switch (ev) {
    case SndEv::Listen:   enqueue(Job{5, 1}); break;  // 嘎 ×1
    case SndEv::Sent:     enqueue(Job{5, 1}); break;  // 嘎 ×1
    case SndEv::Received: enqueue(Job{3, 0}); break;  // 叮咚
    case SndEv::Error:    enqueue(Job{4, 0}); break;  // 噗噗
    }
}

// 播放真鸭叫采样（flash 映射直读流式写，零 RAM 拷贝）×count
void AudioPipe::playQuackSample(int count, bool closeAfter) {
    const bool openedHere = !spkOpened_ && duplexOpen(RATE_REC);
    if (!spkOpened_) {
        ESP_LOGW(TAG, "quack 采样：通路上电失败，跳过");
        return;
    }
    for (int n = 0; n < count; n++) {
        int done = 0;
        while (done < SND_QUACK_LEN) {
            int chunk = SND_QUACK_LEN - done;
            if (chunk > 240) chunk = 240;
            const int wrote = spkWriteMono(snd_quack_data + done, chunk);
            if (wrote <= 0) break;
            done += wrote;
        }
        vTaskDelay(pdMS_TO_TICKS(30));  // 让 DMA 尾音放完
        if (n + 1 < count) vTaskDelay(pdMS_TO_TICKS(70));  // 嘎间空隙
    }
    if (closeAfter) {
        micClose();
        spkClose();
    }
}

// 入队鸭叫"嘎"×count（降调锯齿合成）：1=开始/发出，2=收到回复。
// 声数=事件类别，闭眼数数即可（2026-09-25 用户定稿鸭子声音语言）
void AudioPipe::quack(int count) {
    if (talkActive_) return;
    if (!beepEnabled_) return;
    enqueue(Job{5, static_cast<uint16_t>(count)});
}

// 入队"噗噗"（鸭子放屁，错误专属）：低频锯齿+随机抖动+噗呲断续包络。
// 与嘎声家族（有音高）是两个声音物种，出错时不可能听错（用户设计）
void AudioPipe::poot() {
    if (talkActive_) return;
    if (!beepEnabled_) {
        ESP_LOGI(TAG, "poot（提示音已关，跳过）");
        return;
    }
    enqueue(Job{4, 0});
}

// 入队"叮咚"双音——回复到达提示（talk 态/提示音关 跳过）
void AudioPipe::chime() {
    if (talkActive_) {
        ESP_LOGI(TAG, "chime（talk 态跳过）");
        return;
    }
    if (!beepEnabled_) {
        ESP_LOGI(TAG, "chime（提示音已关）");
        return;
    }
    enqueue(Job{3, 0});
}

// 入队上电作业（arg 塞采样率/1000：Job::arg 只有 16 位）
void AudioPipe::requestOpen(int rate) {
    enqueue(Job{1, static_cast<uint16_t>(rate / 1000)});
}

// 入队断电作业（会话结束，整条前端回零耗）
void AudioPipe::requestClose() {
    enqueue(Job{2, 0});
}

// 投递作业；队列满丢弃并告警，同时点亮 beepBusy 供调用方等作业落地
void AudioPipe::enqueue(const Job& j) {
    if (beepQueue_ == nullptr) return;
    if (xQueueSend(beepQueue_, &j, 0) != pdTRUE) {
        ESP_LOGW(TAG, "音频作业队列满，丢弃 op=%u", static_cast<unsigned>(j.op));
        return;
    }
    beepBusy_ = true;  // 作业在飞：上电完→才播→才断电（beepBusy 守次序）
}

// 作业线程：串行消费队列——上电→播放→断电 的次序在此根治并发掐滴/冷路径
void AudioPipe::audioTask(void* arg) {
    auto* self = static_cast<AudioPipe*>(arg);
    // 启动时预合成一声 1kHz 正弦（150ms），播放时直接写 DMA，不必反复算 sin
    const int samplesPerBeep = RATE_REC * BEEP_ON_MS / 1000;
    int16_t* pcm = static_cast<int16_t*>(malloc(samplesPerBeep * sizeof(int16_t)));
    for (int i = 0; i < samplesPerBeep; i++) {
        const float t = static_cast<float>(i) / RATE_REC;
        pcm[i] = static_cast<int16_t>(sinf(2.0f * 3.14159265f * BEEP_FREQ_HZ * t) *
                                      BEEP_AMP * 32767.0f);
    }
    Job job;
    for (;;) {
        if (xQueueReceive(self->beepQueue_, &job, portMAX_DELAY) != pdTRUE) continue;
        switch (job.op) {
        case 0: {  // 播放：会话期=纯写；会话外（如 receipt"叮"）=临时上电，播完即断电
            const int count = job.arg;
            const bool openedHere = !self->spkOpened_ && self->duplexOpen(RATE_REC);
            if (!self->spkOpened_) {
                ESP_LOGW(TAG, "beep：通路上电失败，跳过");
                break;
            }
            for (int n = 0; n < count; n++) {
                int left = samplesPerBeep;
                const int16_t* p = pcm;
                while (left > 0) {
                    const int wrote = self->spkWriteMono(p, left);
                    if (wrote <= 0) break;
                    p += wrote;
                    left -= wrote;
                }
                if (n + 1 < count) vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
            }
            vTaskDelay(pdMS_TO_TICKS(30));  // 让 DMA 尾音放完
            ESP_LOGI(TAG, "beep x%d 播放完成", count);
            if (openedHere) {
                self->micClose();
                self->spkClose();
                ESP_LOGI(TAG, "音频前端断电（临时提示音结束）");
            }
            break;
        }
        case 3: {  // chime"叮咚"双音（回复到达）：高→低，同 beep 的临时上电/断电模式
            const bool openedHere = !self->spkOpened_ && self->duplexOpen(RATE_REC);
            if (!self->spkOpened_) {
                ESP_LOGW(TAG, "chime：通路上电失败，跳过");
                break;
            }
            static const float kFreqs[2] = {1318.0f, 988.0f};  // E6 → B5
            static const int   kDurMs[2]  = {120, 180};
            for (int n = 0; n < 2; n++) {
                const int total = RATE_REC * kDurMs[n] / 1000;
                for (int base = 0; base < total; base += samplesPerBeep) {
                    const int chunk = (total - base) > samplesPerBeep ? samplesPerBeep
                                                                     : (total - base);
                    for (int i = 0; i < chunk; i++) {
                        const float t = static_cast<float>(base + i) / RATE_REC;
                        // 简单线性包络衰减，听感比方波头尾"啪"干净
                        const float env = 1.0f - static_cast<float>(base + i) / total;
                        pcm[i] = static_cast<int16_t>(
                            sinf(2.0f * 3.14159265f * kFreqs[n] * t) * BEEP_AMP * env * 32767.0f);
                    }
                    int left = chunk;
                    const int16_t* p = pcm;
                    while (left > 0) {
                        const int wrote = self->spkWriteMono(p, left);
                        if (wrote <= 0) break;
                        p += wrote;
                        left -= wrote;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            ESP_LOGI(TAG, "chime 播放完成");
            if (openedHere) {
                self->micClose();
                self->spkClose();
                ESP_LOGI(TAG, "音频前端断电（临时提示音结束）");
            }
            break;
        }
        case 4: {  // "噗噗"：70~130Hz 锯齿随机游走 + 噗呲断续包络，两声
            const bool openedHere = !self->spkOpened_ && self->duplexOpen(RATE_REC);
            if (!self->spkOpened_) {
                ESP_LOGW(TAG, "poot：通路上电失败，跳过");
                break;
            }
            unsigned seed = 20260925u;  // 噗没有标准音，伪随机每次略不同反而真实
            for (int n = 0; n < 2; n++) {
                const int total = RATE_REC * 220 / 1000;  // 220ms/噗
                float phase = 0.0f;
                for (int base = 0; base < total; base += samplesPerBeep) {
                    const int chunk = (total - base) > samplesPerBeep ? samplesPerBeep
                                                                      : (total - base);
                    for (int i = 0; i < chunk; i++) {
                        seed = seed * 1103515245u + 12345u;
                        const float f = 75.0f + (float)((seed >> 16) % 55);
                        phase += 6.2831853f * f / RATE_REC;
                        if (phase > 6.2831853f) phase -= 6.2831853f;
                        const float saw = (phase / 6.2831853f) * 2.0f - 1.0f;
                        seed = seed * 1103515245u + 12345u;
                        const float sput = ((seed >> 16) % 100) < 80 ? 1.0f : 0.12f;
                        const float env = sput * (1.0f - (float)(base + i) / total * 0.55f);
                        pcm[i] = static_cast<int16_t>(saw * env * BEEP_AMP * 0.9f *
                                                      32767.0f);
                    }
                    int left = chunk;
                    const int16_t* p = pcm;
                    while (left > 0) {
                        const int wrote = self->spkWriteMono(p, left);
                        if (wrote <= 0) break;
                        p += wrote;
                        left -= wrote;
                    }
                }
                if (n == 0) vTaskDelay(pdMS_TO_TICKS(140));  // 两噗之间
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            if (openedHere) {
                self->micClose();
                self->spkClose();
            }
            break;
        }
        case 5: {  // 鸭叫"嘎"×arg：播真鸭叫采样（合成版代码保留作降级路径）
            const int count = job.arg;
            const bool openedHere = !self->spkOpened_ && self->duplexOpen(RATE_REC);
            // ⚠️ 只关自己开的通路（真机实锤 2026-09-25：无条件关 mic 会把
            // 录音会话的麦克风一起关掉 → 长按录音全空、"没有声音被录到"）
            const bool closeAfter = openedHere;
            self->playQuackSample(count, closeAfter);
            break;
        }
#if 0   // 合成版"嘎"（降调锯齿），采样不可用时的人工兜底，代码留档
        case 5: {  // 鸭叫"嘎"×arg：500→250Hz 降调锯齿+包络，190ms/声
            const int count = job.arg;
            const bool openedHere = !self->spkOpened_ && self->duplexOpen(RATE_REC);
            if (!self->spkOpened_) {
                ESP_LOGW(TAG, "quack：通路上电失败，跳过");
                break;
            }
            for (int n = 0; n < count; n++) {
                const int total = RATE_REC * 190 / 1000;
                float phase = 0.0f;
                for (int base = 0; base < total; base += samplesPerBeep) {
                    const int chunk = (total - base) > samplesPerBeep ? samplesPerBeep
                                                                      : (total - base);
                    for (int i = 0; i < chunk; i++) {
                        const float tt = static_cast<float>(base + i) / total;
                        const float f = 500.0f - 250.0f * tt;
                        phase += 6.2831853f * f / RATE_REC;
                        if (phase > 6.2831853f) phase -= 6.2831853f;
                        const float saw = (phase / 6.2831853f) * 2.0f - 1.0f;
                        const float env = tt < 0.10f
                            ? tt / 0.10f
                            : 1.0f - (tt - 0.10f) / 0.90f;
                        pcm[i] = static_cast<int16_t>((0.55f * saw +
                                                       0.45f * sinf(phase)) *
                                                      env * BEEP_AMP * 32767.0f);
                    }
                    int left = chunk;
                    const int16_t* p = pcm;
                    while (left > 0) {
                        const int wrote = self->spkWriteMono(p, left);
                        if (wrote <= 0) break;
                        p += wrote;
                        left -= wrote;
                    }
                }
                if (n + 1 < count) vTaskDelay(pdMS_TO_TICKS(70));  // 嘎间空隙
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            if (openedHere) {
                self->micClose();
                self->spkClose();
            }
            break;
        }
#endif  // 合成版留档
        case 1:  // 上电：spk+mic 配对舞蹈一次做完（冷路径根治），会话期保持到断电作业
            if (self->duplexOpen(static_cast<int>(job.arg) * 1000)) {
                ESP_LOGI(TAG, "音频前端上电 @%dk", static_cast<unsigned>(job.arg));
            } else {
                ESP_LOGE(TAG, "音频前端上电失败 @%dk", static_cast<unsigned>(job.arg));
            }
            break;
        default:  // 断电：整条前端回零耗（mic 先关再 spk，遵守约束④）
            self->micClose();
            self->spkClose();
            ESP_LOGI(TAG, "音频前端断电（会话结束，零耗待机）");
            break;
        }
        self->beepBusy_ = uxQueueMessagesWaiting(self->beepQueue_) > 0;
    }
}

// ---------------------------------------------------------------- 工具 ----
// 立体声下混单声道：sel=0 取 MIC1 / 1 取 MIC2 / 2 取平均
void AudioPipe::downmix(const int16_t* stereo, int16_t* mono, int frames, int sel) {
    for (int i = 0; i < frames; i++) {
        const int32_t l = stereo[2 * i];      // L = MIC1
        const int32_t r = stereo[2 * i + 1];  // R = MIC2
        mono[i] = static_cast<int16_t>(sel == 0 ? l : sel == 1 ? r : (l + r) / 2);
    }
}

// 24kHz → 16kHz 抽取（固定 480 入 / 320 出，talk 上行用）
void AudioPipe::resample24to16(const int16_t* in480, int16_t* out320) {
    // 3 抽头预滤波（0.25/0.5/0.25）软化混叠，再线性插值取 2/3 点
    static int16_t prevTail[2] = {0, 0};  // 跨帧连续性（粗糙但优于跳变）
    float filt[482];
    filt[0] = prevTail[0] * 0.25f + in480[0] * 0.5f + in480[1] * 0.25f;
    filt[1] = prevTail[1] * 0.25f + in480[0] * 0.25f + in480[1] * 0.5f + in480[2] * 0.25f;
    for (int i = 2; i < 479; i++) {  // 上界 479：末点无后继样本，由下行公式单独算
        filt[i] = in480[i - 1] * 0.25f + in480[i] * 0.5f + in480[i + 1] * 0.25f;
    }
    filt[479] = in480[478] * 0.25f + in480[479] * 0.75f;  // 末尾无后继
    prevTail[0] = in480[478];
    prevTail[1] = in480[479];
    for (int i = 0; i < 320; i++) {
        const float pos = i * 1.5f;
        const int   idx = static_cast<int>(pos);
        const float frac = pos - idx;
        out320[i] = static_cast<int16_t>(filt[idx] * (1.0f - frac) + filt[idx + 1] * frac);
    }
}

uint32_t AudioPipe::rms(const int16_t* pcm, int samples) {
    int64_t sumSq = 0;
    for (int i = 0; i < samples; i++) {
        const int32_t v = pcm[i];
        sumSq += v * v;
    }
    return static_cast<uint32_t>(sqrtf(static_cast<float>(sumSq) / samples));
}

}  // namespace gaga

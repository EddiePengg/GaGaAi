#include "Display.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.disp";

static esp_lcd_panel_handle_t s_panel = nullptr;  // panel 句柄：息屏命令发给它
static lv_display_t*          s_disp  = nullptr;
static bool                   s_sleep = false;    // 软件侧息屏状态（displayIsSleeping 回这个）
static esp_lcd_touch_handle_t s_touch = nullptr;  // 触摸句柄：自持 indev 用（旋转坐标变换）
static int                    s_rotation = 0;     // 当前内容旋转角（0/90/180/270）

// 自持触摸 indev（2026-09-25 自动转向）：esp_lv_adapter 的触摸没有运行时
// 旋转钩子，转屏后坐标会反 → 自己读 CST9217，按当前旋转角变换后再给 LVGL。
// 基准：bsp_touch_new 的 mirror_x/mirror_y 已把原始坐标映射到"旋转 0"的
// 屏幕坐标系；这里再做 logical = R(-rotation) · physical 的逆变换。
// 注意：read_cb 在 LVGL 服务线程里跑，不得再抢 displayLock（esp_lcd_touch
// 自带互斥，直接读安全）。
static void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t px[1] = {0}, py[1] = {0};
    uint8_t n = 0;
    if (s_touch == nullptr) return;
    esp_lcd_touch_read_data(s_touch);
    if (esp_lcd_touch_get_coordinates(s_touch, px, py, nullptr, &n, 1) && n > 0) {
        float x = px[0], y = py[0];
        const float W = 466, H = 466;   // 正方形屏：旋转前后同尺寸
        switch (s_rotation) {
        case 90:  { const float t = x; x = y;      y = H - 1 - t; } break;
        case 180: { x = W - 1 - x; y = H - 1 - y; }                break;
        case 270: { const float t = x; x = W - 1 - y; y = t;      } break;
        default: break;
        }
        data->point.x = static_cast<int32_t>(x);
        data->point.y = static_cast<int32_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// CO5300 刷新窗口偶数对齐（QSPI 约束；逐行对照 BSP 的 rounder_event_cb，
// 该回调是 BSP 的私有静态函数，这里自写等价实现）
static void rounderEventCb(lv_event_t* e) {
    lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
    // round the start of coordinate down to the nearest 2M number
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

// 拉起显示全链路：QSPI 面板 → LVGL 适配器 → 触摸 → LVGL 任务 → 起屏清 GRAM。
// 只初始化一次；区别于 bsp_display_start() 的关键是留住 s_panel 给息屏用。
lv_display_t* displayStart() {
    if (s_disp != nullptr) return s_disp;

    // 1) QSPI 面板（CO5300 初始化序列在 BSP 的 bsp_display_new 里）
    const bsp_display_config_t dispHwCfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_io_handle_t io = nullptr;
    if (bsp_display_new(&dispHwCfg, &s_panel, &io) != ESP_OK || s_panel == nullptr) {
        ESP_LOGE(TAG, "bsp_display_new 失败");
        return nullptr;
    }

    // 2) LVGL 适配器 + 注册显示（参数逐项对照 BSP bsp_display_lcd_init）
    esp_lv_adapter_config_t adapterCfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&adapterCfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_adapter_init 失败");
        return nullptr;
    }
    esp_lv_adapter_display_config_t dispCfg = {
        .panel    = s_panel,
        .panel_io = io,
        .profile  = {
            .interface             = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation              = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res               = BSP_LCD_H_RES,
            .ver_res               = BSP_LCD_V_RES,
            .buffer_height         = 10,   // 行高 50→10→4（2026-09-25 清晨）：0.5.0 的控件树
                                            // 把内部 RAM 吃到 ~7KB，10 行块 = 466×10×2≈9.3KB
                                            // 的 DMA 反弹缓冲永远分配不出来 → 每次刷屏
                                            // "Draw bitmap failed: ESP_ERR_NO_MEM"（一晚
                                            // 日志 736 条实锤），物理屏只剩旧像素+零星小块
                                            // = 用户看到的"蒙雾/残影/花盆"。4 行块 3.7KB
                                            // 塞得进去；事务数变多但每笔都成功
            .use_psram             = true,
            .enable_ppa_accel      = false,
            .require_double_buffer = true,
            .mono_layout           = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync         = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    s_disp = esp_lv_adapter_register_display(&dispCfg);
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "register_display 失败");
        return nullptr;
    }
    lv_display_add_event_cb(s_disp, rounderEventCb, LV_EVENT_INVALIDATE_AREA, nullptr);

    // 3) 触摸（CST9217）：句柄留下，indev 自建（touchReadCb，坐标随旋转变换）。
    //    不再用 esp_lv_adapter_register_touch——它不会跟着运行时旋转走。
    bsp_display_cfg_t touchCfg = {};
    touchCfg.touch_flags.swap_xy = 0;
    touchCfg.touch_flags.mirror_x = 1;   // 对照 BSP bsp_display_start 的默认触摸翻转
    touchCfg.touch_flags.mirror_y = 1;
    if (bsp_touch_new(&touchCfg, &s_touch) != ESP_OK || s_touch == nullptr) {
        ESP_LOGW(TAG, "触摸初始化失败（不影响显示，触摸功能缺席）");
        s_touch = nullptr;
    }

    // 4) LVGL 任务
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_adapter_start 失败");
        return nullptr;
    }
    // 4.5) 自持触摸 indev：read_cb 坐标按当前旋转角变换（见 touchReadCb）。
    // 必须持 LVGL 锁（lv_indev_create 是 LVGL 对象操作）。
    if (s_touch != nullptr && displayLock(2000)) {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchReadCb);
        lv_indev_set_display(indev, s_disp);
        displayUnlock();
        ESP_LOGI(TAG, "触摸 indev 就绪（自持，坐标随旋转变换）");
    }
    // 起屏即全屏刷黑一次：把面板 GRAM 里的陈旧内容（异常/历史固件留下的残块）
    // 一次性清掉，否则只有脏区被刷新、旧垃圾会"透过"新 UI 显示（真机踩实 2026-09-23）。
    // 必须持 LVGL 锁：invalidate/refr 与适配器的 LVGL 任务抢跑会触发
    // lv_refr.c 的 "Invalidate area is not allowed during rendering" 断言死锁（踩实）
    if (displayLock(2000)) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
        displayUnlock();
    } else {
        ESP_LOGW(TAG, "起屏清屏没拿到 LVGL 锁（跳过，陈旧 GRAM 可能残留）");
    }
    ESP_LOGI(TAG, "display started: %dx%d（panel 句柄已保留，支持息屏）",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    return s_disp;
}

// 息屏/亮屏（幂等：状态没变直接返回）。只发 DISPOFF/DISPON，绝不发 SLPIN
void displaySleep(bool sleep) {
    if (s_panel == nullptr || sleep == s_sleep) return;
    s_sleep = sleep;
    if (sleep) {
        // 只发 DISPOFF：AMOLED 显示驱动断电、黑屏零功耗。不发 SLPIN——
        // co5300 驱动的 SLPIN 在有 RST 脚时会进 DSTBON 深睡，唤醒要走
        // reset+完整初始化序列（含 600ms 级延时，真机按下好几秒才亮；
        // bug-analysis-0923.md 问题 1）。DISPOFF 唤醒是亚秒内的纯指令。
        esp_lcd_panel_disp_on_off(s_panel, false);
        ESP_LOGI(TAG, "screen off（DISPOFF）");
    } else {
        const uint32_t t0 = gaga::millis();
        esp_lcd_panel_disp_on_off(s_panel, true);
        ESP_LOGI(TAG, "screen on（DISPON，耗时 %lums）",
                 static_cast<unsigned long>(gaga::millis() - t0));
    }
}

// 查询是否息屏（只回内存标志，不碰硬件）
bool displayIsSleeping() {
    return s_sleep;
}

// 亮度（ADR-031）：走 BSP 的 0x51 寄存器写入——自组链路调过 bsp_display_new，
// BSP 的静态 io_handle 已登记，这里能用。息屏中调用也安全（寄存器状态在
// 面板断电时保持，DISPON 后生效）。
void displaySetBrightness(int percent) {
    const esp_err_t err = bsp_display_brightness_set(percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "亮度设置失败 %d%%：%s", percent, esp_err_to_name(err));
    }
}

// 内容 180° 反转（自动转向 2026-09-25）：MADCTL(0x36) bit6+bit7 镜像 X+Y。
// 面板层寄存器方案：GRAM 地址计数器本身被重映射——部分刷新天然兼容、
// 零 CPU 开销（LVGL9 原生旋转在部分刷新模式结构性不支持，弃用）。
// 仅支持 0/180（90/270 需行列交换 MV 位，非正方形路径会变形，不支持）。
void displaySetRotation(int deg) {
    if (s_panel == nullptr) return;
    if (deg != 0 && deg != 180) {
        ESP_LOGW(TAG, "仅支持 0/180，%d° 忽略", deg);
        return;
    }
    const bool flip = (deg == 180);
    const esp_err_t err = esp_lcd_panel_mirror(s_panel, flip, flip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "面板镜像设置失败：%s", esp_err_to_name(err));
        return;
    }
    s_rotation = flip ? 180 : 0;   // 触摸变换同步翻转（touchReadCb）
    // MADCTL 即刻重映射 GRAM 读址，旧内容立即镜像呈现；全屏失效重刷对齐
    if (displayLock(2000)) {
        lv_obj_invalidate(lv_screen_active());
        displayUnlock();
    }
    ESP_LOGI(TAG, "rotation → %d°（MADCTL 镜像）", s_rotation);
}

int displayGetRotation() {
    return s_rotation;
}

// 取 LVGL 互斥锁；false = timeoutMs 内没抢到，本次刷新应放弃
bool displayLock(uint32_t timeoutMs) {
    return esp_lv_adapter_lock(static_cast<int32_t>(timeoutMs)) == ESP_OK;
}

// 还 LVGL 互斥锁
void displayUnlock() {
    esp_lv_adapter_unlock();
}

}  // namespace gaga

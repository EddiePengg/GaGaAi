#include "Ui.h"

#include <cstdio>
#include <cstring>

#include "lvgl.h"
#include "esp_log.h"

#include "state/AppState.h"
#include "state/MsgLog.h"
#include "state/Settings.h"
#include "ui/FontFilter.h"
#include "assets/img_duck.h"
#include "input/PwrKey.h"
#include "input/Rtc.h"
#include "ui/Display.h"
#include "compat.h"
#include "version.h"

// 思源黑体子集字体（assets/fonts/ 生成，OFL；GB2312 全集 7448 字，2026-09-24）
// ——动态文本（ASR/reply）任意汉字，子集必豆腐；GBK 一级+二级全量兜底
// （注意：声明必须在全局作用域——生成物是 C 文件，符号无 C++ 命名空间修饰）
extern const lv_font_t gaga_font_cjk_16;

namespace gaga {

static const char* TAG = "gaga.ui";

#define FONT_CJK  (&gaga_font_cjk_16)
#define FONT_NUM  (&lv_font_montserrat_24)  // 时间/字标（sdkconfig 已启用）

// ---------------------------------------------------------------- 深空黑主题 ----
// ADR-031：正式视觉取代 M6 验收期黄底诊断色。纯黑底 = AMOLED 黑像素零功耗，
// 鸭黄 accent 延续吉祥物品牌色。卡片 remove_style_all 后自绘（默认主题白边根除）。
static const lv_color_t COL_ACCENT = lv_color_hex(0xFFD60A);  // 鸭黄
static const lv_color_t COL_SUB    = lv_color_hex(0x8E959E);  // 次要灰
static const lv_color_t COL_GREEN  = lv_color_hex(0x30D158);  // 连接绿
// 半透明层级（黑底上的"高度"感）：卡面 < 胶囊/控件底
static constexpr lv_opa_t   OPA_CARD   = LV_OPA_10;  // 白 10% ≈ #1A1A1A
static constexpr lv_opa_t   OPA_CTRL   = LV_OPA_20;  // 白 20% ≈ #333

// ---------------------------------------------------------------- 圆屏布局 ----
// 466 圆：屏幕坐标 y 处半宽 = sqrt(233² - |y-233|²)。
// 状态栏中心 y≈36（半宽 ≈124 → 可用 ~249px）；主体 y∈[112,402]（底缘半宽 ≈159 → 300px 全程安全）。
// 状态栏底(~50) 与主体顶(112) 间 ~62px 呼吸区（2026-09-24 用户反馈修正：上移贴顶 + 拉开间距）。
static constexpr int SAFE_W   = 300;
static constexpr int BODY_H   = 290;
static constexpr int BODY_OY  = 24;
static constexpr int STATUS_Y = 10;
static constexpr int NOTE_Y   = 195;

static Ui* s_ui = nullptr;  // LVGL 事件回调出口（单实例）

// ---------------------------------------------------------------- 布局构件 ----

// 建 label 小工：套字体和颜色
static lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    return l;
}

// 建"胶囊按钮"：文本 label + 半透明底 + 全圆角（返回键/开关/循环箭头通用形）
static lv_obj_t* makePill(lv_obj_t* parent, const char* text, lv_opa_t bgOpa) {
    lv_obj_t* l = makeLabel(parent, FONT_CJK, lv_color_white());
    lv_label_set_text(l, text);
    lv_obj_set_style_bg_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, bgOpa, LV_PART_MAIN);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(l, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(l, 6, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// 建圆形触控钮（"<" / ">"）：36px 方形触控区，视觉是圆
static lv_obj_t* makeRoundBtn(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = makeLabel(parent, FONT_CJK, lv_color_white());
    lv_label_set_text(l, text);
    lv_obj_set_size(l, 36, 36);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, OPA_CTRL, LV_PART_MAIN);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// 建设置行：全宽圆角行容器（label + 控件水平排）；需要右侧贴边的行自己加 spacer
static lv_obj_t* makeRow(lv_obj_t* parent, const char* title) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, OPA_CARD, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* t = makeLabel(row, FONT_CJK, lv_color_white());
    lv_label_set_text(t, title);
    lv_obj_set_width(t, 64);
    return row;
}

// 行内弹性空隙：把后面的控件推到行右端
static lv_obj_t* makeSpacer(lv_obj_t* parent) {
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_height(s, 1);  // 不参与可见性，1px 占位
    return s;
}

// 滚动条样式（容器 remove_style_all 后 scrollbar 样式也没了——细灰圆角条，
// 用户报障"不知道能滑"的可感知修复）
static void styleScrollbar(lv_obj_t* cont) {
    lv_obj_set_style_width(cont, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(cont, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(cont, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(cont, LV_OPA_30, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(cont, 2, LV_PART_SCROLLBAR);
}

// 开关胶囊样式（设置页/快捷面板共用）：开 = 黄底黑字，关 = 灰底灰字。
// label 非空时显示"标签·开/关"（快捷面板宽胶囊），为空只显示 开/关（设置页窄胶囊）
static void setTogglePill(lv_obj_t* pill, bool on, const char* label) {
    char buf[24];
    if (label && label[0]) snprintf(buf, sizeof(buf), "%s·%s", label, on ? "开" : "关");
    else                   snprintf(buf, sizeof(buf), "%s", on ? "开" : "关");
    lv_label_set_text(pill, buf);
    lv_obj_set_style_bg_color(pill, on ? COL_ACCENT : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, static_cast<lv_opa_t>(on ? LV_OPA_COVER : OPA_CTRL),
                            LV_PART_MAIN);
    lv_obj_set_style_text_color(pill, on ? lv_color_black() : COL_SUB, LV_PART_MAIN);
}

// 建主题 slider：黑底黄芯白钮
static lv_obj_t* makeSlider(lv_obj_t* parent, int min, int max) {
    lv_obj_t* s = lv_slider_create(parent);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_height(s, 36);                      // 触控高度
    lv_slider_set_range(s, min, max);
    lv_obj_set_style_pad_ver(s, 14, LV_PART_MAIN); // 轨道厚 = 36-28 = 8px
    lv_obj_set_style_radius(s, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, OPA_CTRL, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);  // 钮径 ≈ 12px + 描边
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 2, LV_PART_KNOB);
    lv_obj_set_style_border_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    return s;
}

// ---------------------------------------------------------------- 骨架 ----

// 搭 UI 骨架：状态栏 / 卡片列表 / 详情页 / 设置页 / 录音覆盖层 / talk / 便签，再挂状态回调
void Ui::begin(AppState* app, MsgLog* log) {
    app_ = app;
    log_ = log;
    s_ui  = this;
    if (!displayLock(2000)) {
        ESP_LOGE(TAG, "lvgl lock 超时");
        return;
    }
    lv_obj_t* scr = lv_screen_active();
    // ADR-031 深空黑主题（取代 2026-09-23 的黄底验收诊断色）
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // ---- 顶部状态栏（贴圆顶：电量·BLE | 时间 | 设置入口）----
    statusBar_ = lv_obj_create(scr);
    lv_obj_remove_style_all(statusBar_);
    lv_obj_set_flex_flow(statusBar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusBar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(statusBar_, 14, LV_PART_MAIN);
    lv_obj_set_width(statusBar_, LV_SIZE_CONTENT);
    lv_obj_align(statusBar_, LV_ALIGN_TOP_MID, 0, STATUS_Y);

    battLabel_ = makeLabel(statusBar_, FONT_CJK, lv_color_white());
    lv_label_set_text(battLabel_, "--%");

    bleDot_ = lv_obj_create(statusBar_);
    lv_obj_remove_style_all(bleDot_);
    lv_obj_set_size(bleDot_, 10, 10);
    lv_obj_set_style_radius(bleDot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bleDot_, COL_SUB, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bleDot_, LV_OPA_COVER, LV_PART_MAIN);

    timeLabel_ = makeLabel(statusBar_, FONT_NUM, lv_color_white());
    lv_label_set_text(timeLabel_, "--:--");

    // ---- 主体容器（列表/详情/设置共用的几何：300×290，居中 y+24）----

    // 消息卡列表（新在上，触摸上下滑）
    listCont_ = lv_obj_create(scr);
    lv_obj_remove_style_all(listCont_);
    lv_obj_set_size(listCont_, SAFE_W, BODY_H);
    lv_obj_align(listCont_, LV_ALIGN_CENTER, 0, BODY_OY);
    lv_obj_set_flex_flow(listCont_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont_, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(listCont_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(listCont_, LV_OBJ_FLAG_SCROLLABLE);
    styleScrollbar(listCont_);

    // 空状态（无消息时的品牌位）：gaga 字标 + 引导
    emptyCont_ = lv_obj_create(listCont_);
    lv_obj_remove_style_all(emptyCont_);
    lv_obj_set_width(emptyCont_, lv_pct(100));
    lv_obj_set_flex_flow(emptyCont_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(emptyCont_, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(emptyCont_, 40, LV_PART_MAIN);
    lv_obj_set_flex_align(emptyCont_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* logo = makeLabel(emptyCont_, FONT_NUM, COL_ACCENT);
    lv_label_set_text(logo, "gaga");
    lv_obj_t* tip1 = makeLabel(emptyCont_, FONT_CJK, lv_color_white());
    lv_label_set_text(tip1, "还没有消息");
    lv_obj_t* tip2 = makeLabel(emptyCont_, FONT_CJK, COL_SUB);
    lv_label_set_text(tip2, "长按右下角说话");

    // 卡片池一次性预建 20 张（HIDDEN 复用），避免每次消息变动都 create/delete 控件
    for (int i = 0; i < CARD_POOL; i++) {
        Card& c = cards_[i];
        c.cont = lv_obj_create(listCont_);
        lv_obj_remove_style_all(c.cont);  // 关键：默认主题的白边框在这里根除
        lv_obj_set_width(c.cont, lv_pct(100));
        lv_obj_set_height(c.cont, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(c.cont, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c.cont, OPA_CARD, LV_PART_MAIN);
        lv_obj_set_style_radius(c.cont, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_all(c.cont, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_row(c.cont, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(c.cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(c.cont, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(c.cont, LV_OBJ_FLAG_HIDDEN);

        c.ask = makeLabel(c.cont, FONT_CJK, lv_color_white());
        lv_obj_set_width(c.ask, lv_pct(100));
        lv_obj_set_height(c.ask, 40);  // 2 行封顶
        lv_label_set_long_mode(c.ask, LV_LABEL_LONG_DOT);

        c.reply = makeLabel(c.cont, FONT_CJK, COL_ACCENT);
        lv_obj_set_width(c.reply, lv_pct(100));
        lv_obj_set_height(c.reply, 40);  // 2 行封顶
        lv_label_set_long_mode(c.reply, LV_LABEL_LONG_DOT);
    }

    // ---- 详情页（点卡进入；息屏收回复自动直达这里）----
    detailCont_ = lv_obj_create(scr);
    lv_obj_remove_style_all(detailCont_);
    lv_obj_set_size(detailCont_, SAFE_W, BODY_H);
    lv_obj_align(detailCont_, LV_ALIGN_CENTER, 0, BODY_OY);
    lv_obj_set_flex_flow(detailCont_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(detailCont_, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(detailCont_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detailCont_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(detailCont_, LV_OBJ_FLAG_SCROLLABLE);
    styleScrollbar(detailCont_);

    backLabel_ = makePill(detailCont_, "← 返回", OPA_CTRL);

    detailAsk_ = makeLabel(detailCont_, FONT_CJK, lv_color_white());
    lv_obj_set_width(detailAsk_, lv_pct(100));
    lv_label_set_long_mode(detailAsk_, LV_LABEL_LONG_WRAP);

    detailReply_ = makeLabel(detailCont_, FONT_CJK, COL_ACCENT);
    lv_obj_set_width(detailReply_, lv_pct(100));
    lv_label_set_long_mode(detailReply_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(detailCont_, LV_OBJ_FLAG_HIDDEN);

    // ---- 设置页（ADR-031：触摸操作，音量/亮度/息屏/提示音/关于）----
    settingsCont_ = lv_obj_create(scr);
    lv_obj_remove_style_all(settingsCont_);
    lv_obj_set_size(settingsCont_, SAFE_W, BODY_H);
    lv_obj_align(settingsCont_, LV_ALIGN_CENTER, 0, BODY_OY);
    lv_obj_set_flex_flow(settingsCont_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(settingsCont_, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(settingsCont_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settingsCont_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(settingsCont_, LV_OBJ_FLAG_SCROLLABLE);
    styleScrollbar(settingsCont_);
    lv_obj_add_flag(settingsCont_, LV_OBJ_FLAG_HIDDEN);

    backSetBtn_ = makePill(settingsCont_, "← 返回", OPA_CTRL);

    {   // 音量行
        lv_obj_t* row = makeRow(settingsCont_, "音量");
        volSlider_ = makeSlider(row, Settings::VOL_MIN, Settings::VOL_MAX);
        volVal_ = makeLabel(row, FONT_CJK, COL_SUB);
        lv_obj_set_width(volVal_, 36);
        lv_obj_set_style_text_align(volVal_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
    {   // 亮度行
        lv_obj_t* row = makeRow(settingsCont_, "亮度");
        briSlider_ = makeSlider(row, Settings::BRIGHT_MIN, Settings::BRIGHT_MAX);
        briVal_ = makeLabel(row, FONT_CJK, COL_SUB);
        lv_obj_set_width(briVal_, 36);
        lv_obj_set_style_text_align(briVal_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
    {   // 息屏时长行（< 5s/10s/30s/60s > 循环）
        lv_obj_t* row = makeRow(settingsCont_, "息屏");
        toLeft_  = makeRoundBtn(row, "<");
        toVal_   = makeLabel(row, FONT_CJK, lv_color_white());
        lv_obj_set_flex_grow(toVal_, 1);
        lv_obj_set_style_text_align(toVal_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        toRight_ = makeRoundBtn(row, ">");
    }
    {   // WiFi 行（ADR-039 A 期：显示配置状态；B 期接直连 MQTT 的开关）
        lv_obj_t* row = makeRow(settingsCont_, "WiFi");
        makeSpacer(row);
        wifiVal_ = makeLabel(row, FONT_CJK, COL_SUB);
    }
    {   // 提示音行（开/关胶囊）
        lv_obj_t* row = makeRow(settingsCont_, "提示音");
        makeSpacer(row);
        beepToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(beepToggle_, 60, 32);
        lv_obj_set_style_text_align(beepToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(beepToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(beepToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 语音唤醒行（开/关胶囊，ADR-037）：开 = mic 常开本地听"Hi,乐鑫"
        lv_obj_t* row = makeRow(settingsCont_, "语音唤醒");
        makeSpacer(row);
        kwsToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(kwsToggle_, 60, 32);
        lv_obj_set_style_text_align(kwsToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(kwsToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(kwsToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 对话字幕行（开/关胶囊，ADR-034）：关 = talk 纯语音模式（防长文本卡顿）
        lv_obj_t* row = makeRow(settingsCont_, "对话字幕");
        makeSpacer(row);
        subsToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(subsToggle_, 60, 32);
        lv_obj_set_style_text_align(subsToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(subsToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(subsToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 摇动录音行（开/关胶囊）：摔倒/磕碰误触的用户开关（2026-09-25）
        lv_obj_t* row = makeRow(settingsCont_, "摇动录音");
        makeSpacer(row);
        shakeToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(shakeToggle_, 60, 32);
        lv_obj_set_style_text_align(shakeToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(shakeToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(shakeToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 抬手亮屏行（开/关胶囊）：挂脖拎起自动点亮，手动开关
        lv_obj_t* row = makeRow(settingsCont_, "抬手亮屏");
        makeSpacer(row);
        liftToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(liftToggle_, 60, 32);
        lv_obj_set_style_text_align(liftToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(liftToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(liftToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 自动转向行（开/关胶囊）：持握角变化内容跟着转（2026-09-25）
        lv_obj_t* row = makeRow(settingsCont_, "自动转向");
        makeSpacer(row);
        arotToggle_ = makeLabel(row, FONT_CJK, lv_color_black());
        lv_obj_set_size(arotToggle_, 60, 32);
        lv_obj_set_style_text_align(arotToggle_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_radius(arotToggle_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_flag(arotToggle_, LV_OBJ_FLAG_CLICKABLE);
    }
    {   // 屏幕方向行（< 正/反 > 手动固定；自动转向关着时生效，2026-09-25）
        lv_obj_t* row = makeRow(settingsCont_, "屏幕方向");
        flipLeft_ = makeRoundBtn(row, "<");
        flipVal_ = makeLabel(row, FONT_CJK, lv_color_white());
        lv_obj_set_flex_grow(flipVal_, 1);
        lv_obj_set_style_text_align(flipVal_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        flipRight_ = makeRoundBtn(row, ">");
    }
    {   // 对话引擎行（< 豆包/星辰 > 循环，ADR-035）：talk 用哪个实时后端
        lv_obj_t* row = makeRow(settingsCont_, "引擎");
        prvLeft_  = makeRoundBtn(row, "<");
        prvVal_   = makeLabel(row, FONT_CJK, lv_color_white());
        lv_obj_set_flex_grow(prvVal_, 1);
        lv_obj_set_style_text_align(prvVal_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        prvRight_ = makeRoundBtn(row, ">");
    }
    {   // 关于行（点击展开；整行都是点击区）
        lv_obj_t* row = makeRow(settingsCont_, "关于");
        makeSpacer(row);
        lv_obj_t* arrow = makeLabel(row, FONT_CJK, COL_SUB);
        lv_label_set_text(arrow, ">");
        lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, [](lv_event_t*) {
            if (!s_ui) return;
            s_ui->aboutOpen_ = !s_ui->aboutOpen_;
            s_ui->renderSettings();
        }, LV_EVENT_CLICKED, nullptr);
        aboutCont_ = lv_obj_create(settingsCont_);
        lv_obj_remove_style_all(aboutCont_);
        lv_obj_set_width(aboutCont_, lv_pct(100));
        lv_obj_set_style_bg_color(aboutCont_, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(aboutCont_, LV_OPA_10, LV_PART_MAIN);
        lv_obj_set_style_radius(aboutCont_, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_all(aboutCont_, 14, LV_PART_MAIN);
        lv_obj_add_flag(aboutCont_, LV_OBJ_FLAG_HIDDEN);
        aboutLabel_ = makeLabel(aboutCont_, FONT_CJK, COL_SUB);
        lv_obj_set_width(aboutLabel_, lv_pct(100));
        lv_label_set_long_mode(aboutLabel_, LV_LABEL_LONG_WRAP);
    }

    // ---- 录音覆盖层 ----
    recDot_ = lv_obj_create(scr);
    lv_obj_set_size(recDot_, 48, 48);
    lv_obj_set_style_radius(recDot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(recDot_, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(recDot_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(recDot_, 0, LV_PART_MAIN);
    lv_obj_align(recDot_, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(recDot_, LV_OBJ_FLAG_HIDDEN);

    recLabel_ = makeLabel(scr, FONT_CJK, lv_color_white());
    lv_obj_align(recLabel_, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(recLabel_, LV_OBJ_FLAG_HIDDEN);

    // ---- talk 视图（M5）----
    asrLabel_ = makeLabel(scr, FONT_CJK, lv_color_white());
    lv_obj_set_width(asrLabel_, SAFE_W);
    lv_label_set_long_mode(asrLabel_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(asrLabel_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(asrLabel_, LV_ALIGN_CENTER, 0, -60);
    lv_label_set_text(asrLabel_, "");  // LVGL 默认 "Text" 占位（用户看到两个空 text 的元凶）
    lv_obj_add_flag(asrLabel_, LV_OBJ_FLAG_HIDDEN);

    aiLabel_ = makeLabel(scr, FONT_CJK, COL_ACCENT);
    lv_obj_set_width(aiLabel_, SAFE_W);
    lv_label_set_long_mode(aiLabel_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(aiLabel_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(aiLabel_, LV_ALIGN_CENTER, 0, 60);
    lv_label_set_text(aiLabel_, "");
    lv_obj_add_flag(aiLabel_, LV_OBJ_FLAG_HIDDEN);

    talkHint_ = makeLabel(scr, FONT_CJK, COL_SUB);
    lv_label_set_text(talkHint_, "实时对话中\n直接说话\n长按右上结束");
    lv_obj_set_style_text_align(talkHint_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(talkHint_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(talkHint_, LV_OBJ_FLAG_HIDDEN);

    // ---- 底部便签（半透明胶囊，白字）----
    noteCont_ = lv_obj_create(scr);
    lv_obj_remove_style_all(noteCont_);
    lv_obj_set_style_bg_color(noteCont_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(noteCont_, OPA_CTRL, LV_PART_MAIN);
    lv_obj_set_style_radius(noteCont_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(noteCont_, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(noteCont_, 8, LV_PART_MAIN);
    // 尺寸按内容自适应（之前没设尺寸被 LVGL 默认撑成正圆、文字挤在角落——
    // 用户报"一个圆里一个已字"的真凶）；max_width 防长错误消息撑爆圆屏
    lv_obj_set_size(noteCont_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(noteCont_, 300, LV_PART_MAIN);
    lv_obj_align(noteCont_, LV_ALIGN_CENTER, 0, NOTE_Y);
    lv_obj_add_flag(noteCont_, LV_OBJ_FLAG_HIDDEN);
    noteLabel_ = makeLabel(noteCont_, FONT_CJK, lv_color_white());
    lv_label_set_long_mode(noteLabel_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(noteLabel_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);


    // ---- 鸭子页（ADR-036 陪伴层）----
    buildCompanion();
    // 滑动导航（LV_EVENT_GESTURE）：首页左滑进鸭子页；子页面右滑返回。
    // indev 级手势与列表滚动共存（慢速拖动不触发，滚动不受影响）。
    auto attachSwipe = [this](lv_obj_t* cont) {
        lv_obj_add_event_cb(cont, [](lv_event_t* e) {
            if (s_ui == nullptr) return;
            const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
            s_ui->onSwipe(dir);
        }, LV_EVENT_GESTURE, nullptr);
    };
    // GESTURE 事件挂在活动屏幕上（LVGL9 屏幕级事件；容器级真机不触发）
    attachSwipe(lv_screen_active());
    // 手势区：盖住顶部弧区（含状态栏）。下滑拖出面板；轻点右角=进设置页；
    // 轻点其余=直接开面板（新手可发现性）。录音/talk 时隐藏让路。
    pullZone_ = lv_obj_create(scr);
    lv_obj_remove_style_all(pullZone_);
    lv_obj_set_pos(pullZone_, 93, 4);   // 280×76，居中贴顶（顶弧外区域无触摸，天然无效）
    lv_obj_set_size(pullZone_, 280, 76);
    lv_obj_add_flag(pullZone_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pullZone_, [](lv_event_t* e) {
        if (s_ui == nullptr) return;
        const lv_event_code_t code = lv_event_get_code(e);
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr) return;
        if (code == LV_EVENT_PRESSED) {
            s_ui->pullDy_ = 0;
        } else if (code == LV_EVENT_PRESSING) {
            lv_point_t v;
            lv_indev_get_vect(indev, &v);
            s_ui->pullDy_ += v.y;
            if (s_ui->pullDy_ > 45 && !s_ui->settingsOpen_) {
                s_ui->openSettings();  // 下拉过阈值：呼出设置页（ADR-038 合并）
            }
        } else if (code == LV_EVENT_RELEASED) {
            const int dy = s_ui->pullDy_ < 0 ? -s_ui->pullDy_ : s_ui->pullDy_;
            if (dy < 15 && !s_ui->settingsOpen_) {
                s_ui->openSettings();  // 轻点顶部 = 同样进设置（下拉/轻点双通道）
            }
        }
    }, LV_EVENT_ALL, nullptr);

    // ---------------------------------------------------------------- 事件 ----
    // 点卡片进详情，点"← 返回"回列表
    for (int i = 0; i < CARD_POOL; i++) {
        lv_obj_add_event_cb(cards_[i].cont, [](lv_event_t* e) {
            // 卡的 id 藏在 user_data 里（渲染时写入），不靠下标防串卡
            const uint32_t id = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
            if (s_ui && id != 0) s_ui->openDetail(id);
        }, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_add_event_cb(backLabel_, [](lv_event_t*) {
        if (!s_ui) return;
        s_ui->detailId_ = 0;  // 清掉 detailId_ 就等于"不在详情页"
        s_ui->applyState();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(backSetBtn_, [](lv_event_t*) {
        if (s_ui) s_ui->closeSettings();
    }, LV_EVENT_CLICKED, nullptr);

    // 音量/亮度：拖动实时生效，松手才落盘（NVS 写频率友好）
    lv_obj_add_event_cb(volSlider_, [](lv_event_t* e) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int v = lv_slider_get_value(lv_event_get_target_obj(e));
        s_ui->applySetting(SettingKey::Volume, v, false);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(volSlider_, [](lv_event_t*) {
        if (s_ui && s_ui->settings_) s_ui->settings_->save();
    }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(briSlider_, [](lv_event_t* e) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int v = lv_slider_get_value(lv_event_get_target_obj(e));
        s_ui->applySetting(SettingKey::Brightness, v, false);
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(briSlider_, [](lv_event_t*) {
        if (s_ui && s_ui->settings_) s_ui->settings_->save();
    }, LV_EVENT_RELEASED, nullptr);

    // 息屏时长：档位循环
    lv_obj_add_event_cb(toLeft_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int n = Settings::SCREEN_TIMEOUT_COUNT;
        s_ui->applySetting(SettingKey::ScreenTimeout,
                           static_cast<int>(Settings::SCREEN_TIMEOUTS_MS[
                               (s_ui->settings_->timeoutIndex() + n - 1) % n]), true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(toRight_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int n = Settings::SCREEN_TIMEOUT_COUNT;
        s_ui->applySetting(SettingKey::ScreenTimeout,
                           static_cast<int>(Settings::SCREEN_TIMEOUTS_MS[
                               (s_ui->settings_->timeoutIndex() + 1) % n]), true);
    }, LV_EVENT_CLICKED, nullptr);

    // 提示音开关
    lv_obj_add_event_cb(beepToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::Beep, s_ui->settings_->beepEnabled() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 语音唤醒开关（"Hi,乐鑫"，开 = mic 常开本地推理）
    lv_obj_add_event_cb(kwsToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::Kws, s_ui->settings_->kwsEnabled() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 对话字幕开关（关 = talk 纯语音，文字不再上屏）
    lv_obj_add_event_cb(subsToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::Subtitles,
                           s_ui->settings_->talkSubtitles() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 摇动录音开关（"摇奶茶"触发；关 = 摔倒/磕碰不再误录）
    lv_obj_add_event_cb(shakeToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::Shake,
                           s_ui->settings_->shakeEnabled() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 抬手亮屏开关（挂脖拎起自动点亮）
    lv_obj_add_event_cb(liftToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::Lift,
                           s_ui->settings_->liftEnabled() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 自动转向开关（持握角变化内容跟着转）
    lv_obj_add_event_cb(arotToggle_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::AutoRotate,
                           s_ui->settings_->autoRotateEnabled() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 屏幕方向 < >（正/反，手动固定）
    lv_obj_add_event_cb(flipLeft_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::ScreenFlip,
                           s_ui->settings_->screenFlip() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(flipRight_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        s_ui->applySetting(SettingKey::ScreenFlip,
                           s_ui->settings_->screenFlip() ? 0 : 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    // 音效方案循环（鸭子版/叮咚版，交互逻辑一致）
    // （2026-09-25 用户定稿：方案选择删除——嘎=录开/发，叮咚=收，噗噗=错）

    // 对话引擎：档位循环（volc/step，ADR-035）
    lv_obj_add_event_cb(prvLeft_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int n = Settings::TALK_PROVIDER_COUNT;
        s_ui->applySetting(SettingKey::TalkProvider,
                           (s_ui->settings_->talkProviderIndex() + n - 1) % n, true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(prvRight_, [](lv_event_t*) {
        if (!s_ui || s_ui->settings_ == nullptr) return;
        const int n = Settings::TALK_PROVIDER_COUNT;
        s_ui->applySetting(SettingKey::TalkProvider,
                           (s_ui->settings_->talkProviderIndex() + 1) % n, true);
    }, LV_EVENT_CLICKED, nullptr);

    // 状态联动：状态机一变就重算视图；屏幕亮灭直接驱动面板 DISPOFF/DISPON
    app_->onRecStateChange([this](RecState s) {
        if (s == RecState::Recording) { settingsOpen_ = false; companionOpen_ = false; }
        applyState();
    });
    app_->onTalkStateChange([this](TalkState s) {
        if (s != TalkState::Off) { settingsOpen_ = false; companionOpen_ = false; }
        applyState();
    });
    app_->onScreenChange([this](ScreenState s) {
        displaySleep(s == ScreenState::Off);
        if (s == ScreenState::On) applyState();  // 亮屏后按最新状态重画一遍
    });

    renderStatus();
    displayUnlock();
    applyState();
    ESP_LOGI(TAG, "ui ready（深空黑主题，消息列表 %d 卡，主体 %dx%d @+%d，状态栏 y=%d）",
             CARD_POOL, SAFE_W, BODY_H, BODY_OY, STATUS_Y);
}

// 主体视图六选一，优先级：talk > 录音 > 设置 > 鸭子页 > 详情 > 列表
Ui::View Ui::currentView() const {
    if (app_->talkState() != TalkState::Off) return View::Talk;
    if (app_->recState() == RecState::Recording) return View::Recording;
    if (settingsOpen_) return View::Settings;
    if (companionOpen_) return View::Companion;
    if (detailId_ != 0) return View::Detail;
    return View::List;
}

// 按当前视图显隐各区块：同一时刻只显示一种主体（录音/talk 相当于覆盖层），其余 HIDDEN
void Ui::applyState() {
    if (!displayLock(500)) {
        ESP_LOGW(TAG, "applyState：LVGL 锁超时，本次视图刷新放弃（rec=%d）",
                 static_cast<int>(app_->recState()));
        return;
    }
    const View v = currentView();
    static int lastLogged = -1;
    if (static_cast<int>(v) != lastLogged) {  // 视图切换诊断（录音覆盖层卡死排查）
        static const char* kNames[] = {"List", "Detail", "Settings", "Companion",
                                        "Recording", "Talk"};
        ESP_LOGI(TAG, "applyState: %s（rec=%d）", kNames[static_cast<int>(v)],
                 static_cast<int>(app_->recState()));
        lastLogged = static_cast<int>(v);
    }
    const bool showList  = (v == View::List);
    const bool showDet   = (v == View::Detail);
    const bool showSet   = (v == View::Settings);
    const bool showComp  = (v == View::Companion);

    // 切到哪个视图就先把它刷成最新数据，再统一显隐
    if (showList) renderList();
    if (showDet)  renderDetail();
    if (showSet)  renderSettings();
    if (showComp) renderCompanion();

    if (showList) lv_obj_clear_flag(listCont_, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(listCont_, LV_OBJ_FLAG_HIDDEN);
    if (showDet) lv_obj_clear_flag(detailCont_, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(detailCont_, LV_OBJ_FLAG_HIDDEN);
    if (showSet) lv_obj_clear_flag(settingsCont_, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(settingsCont_, LV_OBJ_FLAG_HIDDEN);
    if (showComp) {
        lv_obj_clear_flag(companionCont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(duckImg_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(companionCont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(duckImg_, LV_OBJ_FLAG_HIDDEN);
    }

    const bool showRec  = (v == View::Recording);
    if (showRec) {
        lv_obj_clear_flag(recDot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(recLabel_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(recDot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(recLabel_, LV_OBJ_FLAG_HIDDEN);
    }
    const bool showTalk = (v == View::Talk);
    if (showTalk) {
        lv_obj_clear_flag(asrLabel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(aiLabel_, LV_OBJ_FLAG_HIDDEN);
        // 首句话到达前显示提示（ASR/回复任一有内容即收起提示）
        const bool hasContent =
            strlen(lv_label_get_text(asrLabel_)) > 0 ||
            strlen(lv_label_get_text(aiLabel_)) > 0;
        if (hasContent) lv_obj_add_flag(talkHint_, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_clear_flag(talkHint_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(asrLabel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(aiLabel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(talkHint_, LV_OBJ_FLAG_HIDDEN);
    }
    // 下拉手势区只在主体视图（列表/详情/设置）可用；录音/talk 覆盖层期间让路
    if (showRec || showTalk) lv_obj_add_flag(pullZone_, LV_OBJ_FLAG_HIDDEN);
    else                     lv_obj_clear_flag(pullZone_, LV_OBJ_FLAG_HIDDEN);
    displayUnlock();
}

// ---------------------------------------------------------------- 渲染 ----

// 刷卡片池：下标 i = 第 i 新的卡；两行文案随卡状态变（识别中/正在回复/已回复/失败）
void Ui::renderList() {
    const int n = log_->count();
    const uint32_t now = millis();
    for (int i = 0; i < CARD_POOL; i++) {
        Card& c = cards_[i];
        const Msg* m = (i < n) ? log_->at(i) : nullptr;
        if (!m) {
            lv_obj_add_flag(c.cont, LV_OBJ_FLAG_HIDDEN);  // 没这条就藏掉空卡
            continue;
        }
        lv_obj_set_user_data(c.cont, reinterpret_cast<void*>(static_cast<uintptr_t>(m->id)));
        lv_obj_clear_flag(c.cont, LV_OBJ_FLAG_HIDDEN);

        // static：全文 2048B 级，放栈上会把 8KB 的 appTask 顶爆
        static char buf[2216];
        // ask 文本：还没识别回来时先占位"识别中…"
        snprintf(buf, sizeof(buf), "你：%s",
                 m->state == MsgState::Sending ? "识别中…" : m->ask);
        lv_label_set_text(c.ask, buf);

        switch (m->state) {
        case MsgState::Sending:
            lv_label_set_text(c.reply, "GAGA：发送中…");
            lv_obj_set_style_text_color(c.reply, COL_SUB, LV_PART_MAIN);
            break;
        case MsgState::Waiting: {
            if (MsgLog::softTimedOut(*m, now)) {
                // 软超时：文案让位给"（还没回复）"，state 仍是 Waiting
                lv_label_set_text(c.reply, "GAGA：（还没回复）");
            } else {
                // "正在回复…"动效：dotsPhase_ 每 500ms 换一档（tick 里推进）
                static const char* kDots[4] = {"", ".", "..", "..."};
                snprintf(buf, sizeof(buf), "GAGA：正在回复%s", kDots[dotsPhase_ & 3]);
                lv_label_set_text(c.reply, buf);
            }
            lv_obj_set_style_text_color(c.reply, COL_SUB, LV_PART_MAIN);
            break;
        }
        case MsgState::Replied:
            snprintf(buf, sizeof(buf), "GAGA：%s", m->reply);
            lv_label_set_text(c.reply, buf);
            lv_obj_set_style_text_color(c.reply, COL_ACCENT, LV_PART_MAIN);
            break;
        case MsgState::Failed:
            snprintf(buf, sizeof(buf), "发送失败：%s", m->reply);
            lv_label_set_text(c.reply, buf);
            lv_obj_set_style_text_color(c.reply, lv_palette_main(LV_PALETTE_RED),
                                        LV_PART_MAIN);
            break;
        }
    }
    if (n == 0) lv_obj_clear_flag(emptyCont_, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(emptyCont_, LV_OBJ_FLAG_HIDDEN);
    renderedVersion_  = log_->version();
    // 记下当前是否已有软超时卡：tick() 靠它判断"（还没回复）"文案要不要重刷
    renderedTimeout_  = false;
    for (int i = 0; i < n; i++) {
        if (MsgLog::softTimedOut(*log_->at(i), now)) { renderedTimeout_ = true; break; }
    }
}

// 刷详情页：detailId_ 那张卡的问答全文（自动换行）；卡没了自动回落列表
void Ui::renderDetail() {
    const Msg* m = log_->findById(detailId_);
    if (!m) {  // 卡被 20 条上限顶掉：回落列表
        detailId_ = 0;
        return;
    }
    static char buf[2216];
    snprintf(buf, sizeof(buf), "你：%s",
             m->state == MsgState::Sending ? "识别中…" : m->ask);
    lv_label_set_text(detailAsk_, buf);

    switch (m->state) {
    case MsgState::Sending:
        lv_label_set_text(detailReply_, "GAGA：发送中…");
        break;
    case MsgState::Waiting:
        if (MsgLog::softTimedOut(*m, millis())) {
            // 软超时后安抚用户：回复到了会自己刷出来
            lv_label_set_text(detailReply_, "GAGA：（还没回复）\n回复到了会自动显示");
        } else {
            // 同列表卡："正在回复…"动效
            static const char* kDots[4] = {"", ".", "..", "..."};
            snprintf(buf, sizeof(buf), "GAGA：正在回复%s", kDots[dotsPhase_ & 3]);
            lv_label_set_text(detailReply_, buf);
        }
        break;
    case MsgState::Replied:
        snprintf(buf, sizeof(buf), "GAGA：%s", m->reply);
        lv_label_set_text(detailReply_, buf);
        break;
    case MsgState::Failed:
        snprintf(buf, sizeof(buf), "发送失败：%s", m->reply);
        lv_label_set_text(detailReply_, buf);
        break;
    }
    lv_obj_set_style_text_color(detailReply_,
                                m->state == MsgState::Failed
                                    ? lv_palette_main(LV_PALETTE_RED)
                                    : COL_ACCENT,
                                LV_PART_MAIN);
    renderedVersion_ = log_->version();
}

// ---- 转向动画：一道光沿圆形屏幕整个外沿顺时针扫一圈（用户设计
// 2026-09-25："沿着圆的外边闪一下光效，表示转了一圈"）。亮段 + 柔光尾
// 两个同心弧，500ms 扫完自删 ----
static void rimSweepAnimCb(void* var, int32_t v) {
    lv_arc_set_angles(static_cast<lv_obj_t*>(var), v % 360, (v + 45) % 360);
}
static void rimSweepDoneCb(lv_anim_t* a) {
    lv_obj_delete_async(static_cast<lv_obj_t*>(lv_anim_get_user_data(a)));
}

// 转向落地（Wake 姿态回调，appTask 上下文）：面板 180° 镜像 + 外沿光效
//（仅翻转到 180° 时播放；转回 0° 静默）
void Ui::applyRotation(int deg) {
    displaySetRotation(deg);
    if (settings_ != nullptr && !settings_->autoRotateEnabled()) return;
    if (deg == 0) return;
    if (!displayLock(800)) return;
    // 两个同心光弧：亮段（鸭黄，10px）+ 柔光尾（同色半透明，22px，滞后）
    lv_obj_t* rim[2] = {};
    for (int i = 0; i < 2; i++) {
        lv_obj_t* arc = lv_arc_create(lv_layer_top());
        lv_obj_remove_style_all(arc);
        lv_obj_set_size(arc, 466, 466);            // 全屏：弧贴圆形屏外沿
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(arc, 0);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, i == 0 ? 10 : 22, LV_PART_INDICATOR);
        if (i == 1) lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_INDICATOR);
        rim[i] = arc;
    }
    for (int i = 0; i < 2; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, rim[i]);
        lv_anim_set_exec_cb(&a, rimSweepAnimCb);
        lv_anim_set_values(&a, i == 0 ? 0 : -20, i == 0 ? 360 : 340);
        lv_anim_set_duration(&a, 500);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_user_data(&a, rim[i]);
        lv_anim_set_ready_cb(&a, rimSweepDoneCb);
        lv_anim_start(&a);
    }
    displayUnlock();
}

// 刷顶部状态栏：RTC 时间 + AXP2101 电量（BLE 圆点由连接事件单独刷）
void Ui::renderStatus() {
    RtcTime t{};
    if (rtcGetTime(&t) && t.valid) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
        lv_label_set_text(timeLabel_, buf);
    } else {
        lv_label_set_text(timeLabel_, "--:--");  // 从没对过时：显示占位
    }
    PwrBattery b{};
    if (pwrGetBattery(&b)) {
        char buf[16];
        if (b.percent >= 0) {
            snprintf(buf, sizeof(buf), "%d%%%s", b.percent, b.charging ? "充" : "");
        } else {
            snprintf(buf, sizeof(buf), "--%%%s", b.charging ? "充" : "");
        }
        lv_label_set_text(battLabel_, buf);
        // 电量三态色：充电绿（2026-09-25 用户定稿）/ 低电（≤15% 且没在充）红 / 正常白
        lv_color_t c = lv_color_white();
        if (b.charging) c = lv_palette_main(LV_PALETTE_GREEN);
        else if (b.percent >= 0 && b.percent <= 15) c = lv_palette_main(LV_PALETTE_RED);
        lv_obj_set_style_text_color(battLabel_, c, LV_PART_MAIN);
    }
}

// 刷设置页：slider/开关/关于全部对齐 settings_ 当前值（进页与每次改动后都刷）
void Ui::renderSettings() {
    if (settings_ == nullptr) return;
    lv_slider_set_value(volSlider_, settings_->volume(), LV_ANIM_OFF);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", settings_->volume());
    lv_label_set_text(volVal_, buf);

    lv_slider_set_value(briSlider_, settings_->brightness(), LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d", settings_->brightness());
    lv_label_set_text(briVal_, buf);

    snprintf(buf, sizeof(buf), "%lu秒",
             static_cast<unsigned long>(settings_->screenTimeoutMs() / 1000));
    lv_label_set_text(toVal_, buf);

    // 开关胶囊（setTogglePill 共用样式）
    setTogglePill(beepToggle_,  settings_->beepEnabled(), "");
    setTogglePill(kwsToggle_,   settings_->kwsEnabled(), "");
    setTogglePill(subsToggle_,  settings_->talkSubtitles(), "");
    setTogglePill(shakeToggle_, settings_->shakeEnabled(), "");
    setTogglePill(liftToggle_,  settings_->liftEnabled(), "");
    setTogglePill(arotToggle_,  settings_->autoRotateEnabled(), "");
    lv_label_set_text(flipVal_, settings_->screenFlip() ? "反" : "正");
    // 音效方案已定稿（嘎=录开/发，叮咚=收，噗噗=错），无设置项
    // WiFi 配置状态
    lv_label_set_text(wifiVal_, settings_->wifiConfigured() ? "已配置" : "未配置");
    // 对话引擎当前档位标签
    lv_label_set_text(prvVal_, Settings::TALK_PROVIDER_LABELS[
                                  settings_->talkProviderIndex()]);

    // 关于区：版本 / 设备 / 电量 / 运行时长（每次进设置页现查）
    if (aboutOpen_) {
        lv_obj_clear_flag(aboutCont_, LV_OBJ_FLAG_HIDDEN);
        PwrBattery b{};
        pwrGetBattery(&b);
        const uint32_t upMin = millis() / 60000;
        char about[112];
        snprintf(about, sizeof(about), "固件 fw-idf %s\n设备 %s\n电量 %d%%%s\n运行 %lu时%lu分",
                 FW_VERSION, DEVICE_ID, b.percent, b.charging ? "（充电）" : "",
                 static_cast<unsigned long>(upMin / 60),
                 static_cast<unsigned long>(upMin % 60));
        lv_label_set_text(aboutLabel_, about);
    } else {
        lv_obj_add_flag(aboutCont_, LV_OBJ_FLAG_HIDDEN);
    }
}

// 设置值三步落地：写 settings_ → 回调 applyCb_（audio/display/appState 吃）→ 按需落盘
void Ui::applySetting(SettingKey key, int value, bool save) {
    if (settings_ == nullptr) return;
    switch (key) {
    case SettingKey::Volume:      settings_->setVolume(value); break;
    case SettingKey::Brightness:  settings_->setBrightness(value); break;
    case SettingKey::ScreenTimeout: settings_->setScreenTimeoutMs(
                                      static_cast<uint32_t>(value)); break;
    case SettingKey::Beep:        settings_->setBeepEnabled(value != 0); break;
    case SettingKey::Kws:         settings_->setKwsEnabled(value != 0); break;
    case SettingKey::Subtitles:   settings_->setTalkSubtitles(value != 0); break;
    case SettingKey::Shake:       settings_->setShakeEnabled(value != 0); break;
    case SettingKey::Lift:        settings_->setLiftEnabled(value != 0); break;
    case SettingKey::AutoRotate:  settings_->setAutoRotateEnabled(value != 0); break;
    case SettingKey::ScreenFlip:  settings_->setScreenFlip(value != 0); break;
    case SettingKey::TalkProvider: settings_->setTalkProviderByIndex(value); break;
    }
    if (applyCb_) applyCb_(key, value);
    if (save) settings_->save();
    renderSettings();   // 设置页数值/开关立即跟手
}

// BLE 连/断 → 状态栏圆点（绿已连 / 灰断开）
void Ui::setBleConnected(bool connected) {
    if (!displayLock(500)) return;
    lv_obj_set_style_bg_color(bleDot_, connected ? COL_GREEN : COL_SUB, LV_PART_MAIN);
    displayUnlock();
}

// MsgLog 变了（receipt/reply/error/松手建卡）→ 重刷当前视图。
// reply 到达的两条路：亮屏中到这里就地更新、不抢屏；息屏则由上层调
// openDetail() 亮屏直达这张卡的详情。
void Ui::onLogChanged() {
    // 详情页认卡用 id：卡还在就原地刷新，被顶掉 renderDetail 自动回落列表
    applyState();
}

// 进某张卡的详情页（点卡，或息屏收回复的自动亮屏路径）；id 无效则不动作
void Ui::openDetail(uint32_t id) {
    if (!log_ || !log_->findById(id)) return;
    detailId_ = id;
    // 滚动归零：详情容器是多卡共用的，不归零的话新卡会继承上一张的
    // 滚动位置（用户报"打开新消息还停在上次的滑动位置且滑不动"）
    lv_obj_scroll_to_y(detailCont_, 0, LV_ANIM_OFF);
    app_->notifyActivity();
    app_->setScreen(ScreenState::On);  // 息屏收回复：亮屏直达详情（= 点开这张卡）
    applyState();
}

// 底部便签上屏（✓已送达 / 提示 / 错误），tick() 里约 2s 后自动收
void Ui::showNote(const char* note) {
    if (!displayLock(500)) return;
    lv_label_set_text(noteLabel_, note && note[0] ? note : "");
    lv_obj_clear_flag(noteCont_, LV_OBJ_FLAG_HIDDEN);
    noteVisible_   = true;
    noteShownAtMs_ = millis();
    displayUnlock();
}

// talk 文本上屏：上半"你说"、下半"GAGA"；ASR 未 final 前灰字（中间结果）。
// 字幕关 = 纯语音模式直接短路（长文本重排卡顿的用户侧根治，ADR-034）。
// text 空 = 新一句开始：清上一句（ASR 流式快照跨轮含旧文，设备端主动清防叠字）
void Ui::setTalkText(bool isReply, const char* text, bool final) {
    if (app_->talkState() == TalkState::Off) return;
    if (settings_ != nullptr && !settings_->talkSubtitles()) return;
    if (!displayLock(500)) return;
    lv_obj_t* label = isReply ? aiLabel_ : asrLabel_;
    if (text == nullptr || text[0] == '\0') {  // 新一句：清屏
        lv_label_set_text(label, "");
        displayUnlock();
        return;
    }
    const char* prefix = isReply ? "GAGA：" : "你说：";
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s", prefix, text ? text : "");
    lv_label_set_text(label, buf);
    if (!isReply) {
        lv_obj_set_style_text_color(label,
                                    final ? lv_color_white() : COL_SUB, LV_PART_MAIN);
    }
    displayUnlock();
}

// ---------------------------------------------------------------- 快捷面板（ADR-035）----

// ---------------------------------------------------------------- 鸭子页（ADR-036）----

// 吉祥物图（466×466 圆形遮罩 ARGB8888）：铺满整块圆屏。建在 screen 层
// （companionCont_ 只有 300×290 会裁切），z 序垫底，问候文字画在其上
static lv_obj_t* buildDuck(lv_obj_t* screen) {
    lv_obj_t* img = lv_image_create(screen);
    lv_image_set_src(img, &img_duck);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(img, 0);
    return img;
}

// 鸭子页 v3 极简版：问候 + 白鹅（气泡/概览/水纹全部移除——用户反馈杂乱）
void Ui::buildCompanion() {
    companionCont_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(companionCont_);
    lv_obj_set_size(companionCont_, SAFE_W, BODY_H);
    lv_obj_align(companionCont_, LV_ALIGN_CENTER, 0, BODY_OY);
    lv_obj_add_flag(companionCont_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(companionCont_, LV_OBJ_FLAG_CLICKABLE);  // 手势目标

    duckImg_ = buildDuck(lv_screen_active());

    greetLabel_ = makeLabel(companionCont_, FONT_CJK, lv_color_white());
    lv_obj_align(greetLabel_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t* hint = makeLabel(companionCont_, FONT_CJK, COL_SUB);
    lv_label_set_text(hint, "→ 右滑返回消息");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 22);
}

// 刷鸭子页：按时段问候
void Ui::renderCompanion() {
    RtcTime t{};
    const char* greet = "你好";
    if (rtcGetTime(&t) && t.valid) {
        if (t.hour < 5)       greet = "夜深了";
        else if (t.hour < 11) greet = "早上好";
        else if (t.hour < 13) greet = "中午好";
        else if (t.hour < 18) greet = "下午好";
        else                  greet = "晚上好";
    }
    lv_label_set_text(greetLabel_, greet);
}

void Ui::openCompanion() {
    if (app_->isRecording() || app_->isTalking()) return;
    companionOpen_ = true;
    detailId_ = 0;
    app_->notifyActivity();
    applyState();
}

void Ui::closeCompanion() {
    companionOpen_ = false;
    app_->notifyActivity();
    applyState();
}

// 滑动导航：首页左滑 = 鸭子页；子页面右滑 = 返回
void Ui::onSwipe(lv_dir_t dir) {
    if (dir == LV_DIR_LEFT) {
        if (currentView() == View::List) openCompanion();
    } else if (dir == LV_DIR_RIGHT) {
        switch (currentView()) {
        case View::Detail:    detailId_ = 0; applyState(); break;
        case View::Settings:  closeSettings(); break;
        case View::Companion: closeCompanion(); break;
        default: break;
        }
    }
}

// 右上单击 = 回主页（手表式）：关快捷面板/详情/设置/鸭子页 → 消息列表。
// 灭屏交给自动息屏，单击不背灭屏职责（用户拍板）
void Ui::goHome() {
    settingsOpen_ = false;
    companionOpen_ = false;
    detailId_ = 0;
    app_->notifyActivity();
    applyState();
}

// ---------------------------------------------------------------- 设置页进出 ----

// 状态栏"设置"入口：开页即按 settings_ 现值刷一遍（含关于区现查电量）
void Ui::openSettings() {
    if (settings_ == nullptr) return;
    settingsOpen_ = true;
    detailId_ = 0;   // 设置与详情互斥，进设置视为离开详情
    lv_obj_scroll_to_y(settingsCont_, 0, LV_ANIM_OFF);  // 每次进入从顶部开始
    app_->notifyActivity();
    applyState();
}

void Ui::closeSettings() {
    settingsOpen_ = false;
    app_->notifyActivity();
    applyState();
}

// ~10Hz 定时器：录音秒数 / 便签 2s 回落 / 状态栏 30s 刷 / "正在回复…"动效 / 软超时落地
void Ui::tick() {
    // 录音计时刷新（秒粒度）
    if (app_->recState() == RecState::Recording) {
        const uint32_t sec = app_->recordingElapsedMs() / 1000;
        if (sec != lastRecSecShown_) {
            lastRecSecShown_ = sec;
            if (displayLock(100)) {
                char buf[48];
                snprintf(buf, sizeof(buf), "录音中 %lus", static_cast<unsigned long>(sec));
                lv_label_set_text(recLabel_, buf);
                displayUnlock();
            }
        }
    }
    // 便签 2s 回落
    if (noteVisible_ && (millis() - noteShownAtMs_) >= 2000) {
        if (displayLock(100)) {
            lv_obj_add_flag(noteCont_, LV_OBJ_FLAG_HIDDEN);
            noteVisible_ = false;
            displayUnlock();
        }
    }
    // 状态栏 30s 刷一次（时间/电量；BLE 由连接事件驱动）
    if (lastStatusMs_ == 0 || (millis() - lastStatusMs_) >= 30000) {
        lastStatusMs_ = millis();
        if (displayLock(100)) {
            renderStatus();
            displayUnlock();
        }
    }
    // "正在回复…"动效（500ms 换点）+ 软超时文本落地：只在有等待卡时重刷
    bool hasWaiting = false;
    const uint32_t now = millis();
    bool timeoutNow = false;
    for (int i = 0; i < log_->count(); i++) {
        const Msg* m = log_->at(i);
        if (m->state == MsgState::Waiting) {
            hasWaiting = true;
            if (MsgLog::softTimedOut(*m, now)) timeoutNow = true;
        }
    }
    const bool dotsDue = hasWaiting && (now - lastDotsMs_) >= 500;
    if (dotsDue) {
        lastDotsMs_ = now;
        dotsPhase_ = (dotsPhase_ + 1) & 3;  // 0→1→2→3→0，对应 "" "." ".." "..."
    }
    // 重刷条件：点数该换了，或软超时刚发生（renderedTimeout_ 防止反复刷）
    if (dotsDue || (timeoutNow && !renderedTimeout_)) {
        if (displayLock(100)) {
            const View v = currentView();
            if (v == View::List) renderList();
            else if (v == View::Detail) renderDetail();
            displayUnlock();
        }
    }
}

}  // namespace gaga

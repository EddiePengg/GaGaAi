#pragma once

#include <cstdint>
#include <functional>

#include "lvgl.h"

// LVGL 9 界面（466×466 圆屏 AMOLED）——深空黑主题（ADR-031，2026-09-24）
//   正式视觉：纯黑底（AMOLED 黑像素零功耗）+ 白主文字 + 鸭黄 accent，
//   取代 M6 验收期的黄底诊断色。卡片无白边（remove_style_all 后自绘样式）。
// 圆屏布局（安全宽按高度收窄算）：
//   顶部状态栏 y=22（电量·BLE | 时间 montserrat_24 | 设置入口）——贴圆顶，
//     与主体拉开 ~70px 呼吸区（用户 2026-09-24 反馈"太近/太靠下"的修正）
//   主体区 y∈[112,402]（CENTER+24，高 290，宽 300）= 消息卡列表 / 详情页 / 设置页
//   便签胶囊 y=195（半透明胶囊底，白字）
// 视图优先级：talk > 录音 > 设置 > 详情 > 列表（同一时刻只显示一种主体）。
// 设置系统（ADR-031）：音量/亮度 slider、息屏时长循环档、提示音开关、关于页，
//   全触摸操作；新值经 onApplySetting 回调送 audio/display/appState（Ui 不碰硬件）。
// 所有方法必须在 app 主任务上下文调用（内部自取 LVGL 锁）。
namespace gaga {

class AppState;
class MsgLog;
class Settings;

class Ui {
public:
    // 设置项键（onApplySetting 的 value 语义随键变）
    enum class SettingKey : uint8_t {
        Volume,        // value = 0~100
        Brightness,    // value = 10~100
        ScreenTimeout, // value = 毫秒（5000/10000/30000/60000）
        Beep,          // value = 0 关 / 1 开
        Kws,           // value = 0 关 / 1 开（语音唤醒"Hi,乐鑫"，mic 常开）
        Subtitles,     // value = 0 关 / 1 开（talk 对话字幕；关=纯语音模式）
        Shake,         // value = 0 关 / 1 开（摇动录音"摇奶茶"触发，ADR-040）
        Lift,          // value = 0 关 / 1 开（抬手亮屏）
        AutoRotate,    // value = 0 关 / 1 开（自动转向：持握角变化内容跟着转）
        TalkProvider,  // value = Settings::TALK_PROVIDERS 下标（对话引擎，ADR-035）
    };
    using SettingCallback = std::function<void(SettingKey, int)>;

    // 搭整棵控件树并挂状态回调（只调一次；内部自取 LVGL 锁）
    void begin(AppState* app, MsgLog* log);
    void tick();  // ~10Hz：录音计时 / 便签回落 / 状态栏刷新 / 等待动效 / 软超时

    // BLE 连接态变化 → 状态栏圆点（绿连 / 灰断）
    void setBleConnected(bool connected);

    // MsgLog 变更后调用（receipt/reply/error/松手建卡）：重刷卡片或详情
    void onLogChanged();
    // 直接进某张卡的详情页（息屏收回复自动亮屏路径；id 无效则回落列表）
    void openDetail(uint32_t id);
    // 底部便签胶囊（~2s 自动清）：✓已送达 / 请打开手机App / 错误信息
    void showNote(const char* note);

    // 自动转向落地（Wake 姿态回调）：LVGL 旋转 + 左右两半顺时针箭头滚动动画
    void applyRotation(int deg);

    // talk 文本上屏（TalkSession 回调进来）：isReply=false ASR / true 模型回复；
    // text=nullptr 或空串 = 新一句开始，清上一句（ASR 分层防叠字，ADR-034）
    void setTalkText(bool isReply, const char* text, bool final);

    // ---- 设置系统（ADR-031）----
    void setSettings(Settings* s) { settings_ = s; }
    void onApplySetting(SettingCallback cb) { applyCb_ = std::move(cb); }
    void openSettings();    // 下拉/轻点顶部呼出（设置入口已合并进下拉，ADR-038）
    void closeSettings();
    bool settingsOpen() const { return settingsOpen_; }
    // 顶部下拉（ADR-038）：下拉/轻点顶部 = 打开完整设置页（与设置入口合并）
    // 鸭子页（ADR-036 陪伴层）：首页左滑进入；问候 + 今日概览 + 最近回复气泡
    void openCompanion();
    void closeCompanion();
    bool companionOpen() const { return companionOpen_; }
    // 回主页（右上单击，手表式交互 ADR-037）：关详情/设置/鸭子页/快捷面板 → 消息列表
    void goHome();

private:
    enum class View : uint8_t { List, Detail, Settings, Companion, Recording, Talk };

    void applyState();       // 按 AppState 快照决定主体视图可见性
    void renderList();       // MsgLog → 卡片池
    void renderDetail();     // detailId_ → 详情页文本
    void renderStatus();     // 时间/电量/BLE
    void renderSettings();   // 设置页数值/关于信息（进页与每次改动后刷）
    View currentView() const;

    // 设置行的值落地：改 settings_ + 回调 applyCb_；save 落盘在拖动结束时做
    void applySetting(SettingKey key, int value, bool save);

    AppState* app_ = nullptr;
    MsgLog*   log_ = nullptr;
    Settings* settings_ = nullptr;      // 可为 null（未接设置系统时设置页隐藏入口）
    SettingCallback applyCb_;           // 新值 → audio/display/appState

    // ---- 顶部状态栏 ----
    lv_obj_t* statusBar_ = nullptr;
    lv_obj_t* timeLabel_ = nullptr;     // montserrat_24，白（视觉主角）
    lv_obj_t* battLabel_ = nullptr;     // CJK16
    lv_obj_t* bleDot_    = nullptr;     // 10px 圆点：绿连 / 灰断

    // ---- 消息卡列表 ----
    static constexpr int CARD_POOL = 20;  // 与 MsgLog::MAX_MSGS 一致
    struct Card {          // 一张卡三件套：容器 / 你问一行 / GAGA 答一行
        lv_obj_t* cont;
        lv_obj_t* ask;
        lv_obj_t* reply;
    };
    lv_obj_t* listCont_  = nullptr;
    lv_obj_t* emptyCont_ = nullptr;    // 空状态：gaga 字标 + 引导文案
    Card      cards_[CARD_POOL] = {};

    // ---- 详情页 ----
    lv_obj_t* detailCont_    = nullptr;
    lv_obj_t* backLabel_     = nullptr;  // 胶囊按钮
    lv_obj_t* detailAsk_     = nullptr;
    lv_obj_t* detailReply_   = nullptr;
    uint32_t  detailId_      = 0;   // 0 = 未在详情页

    // ---- 设置页 ----
    lv_obj_t* settingsCont_ = nullptr;
    lv_obj_t* backSetBtn_   = nullptr;
    lv_obj_t* volSlider_    = nullptr;
    lv_obj_t* volVal_       = nullptr;
    lv_obj_t* briSlider_    = nullptr;
    lv_obj_t* briVal_       = nullptr;
    lv_obj_t* toLeft_       = nullptr;   // 息屏时长 < > 循环
    lv_obj_t* toVal_        = nullptr;
    lv_obj_t* toRight_      = nullptr;
    lv_obj_t* wifiVal_      = nullptr;   // WiFi 配置状态显示
    lv_obj_t* prvLeft_      = nullptr;   // 对话引擎 < > 循环（ADR-035）
    lv_obj_t* prvVal_       = nullptr;
    lv_obj_t* prvRight_     = nullptr;
    lv_obj_t* beepToggle_   = nullptr;   // 开/关胶囊
    lv_obj_t* kwsToggle_    = nullptr;   // 语音唤醒 开/关胶囊（ADR-037）
    lv_obj_t* subsToggle_   = nullptr;   // 对话字幕 开/关胶囊（ADR-034）
    lv_obj_t* shakeToggle_  = nullptr;   // 摇动录音 开/关胶囊（ADR-040）
    lv_obj_t* liftToggle_   = nullptr;   // 抬手亮屏 开/关胶囊
    lv_obj_t* arotToggle_   = nullptr;   // 自动转向 开/关胶囊
    lv_obj_t* aboutCont_    = nullptr;   // 关于展开区（版本/设备/电量/运行时长）
    lv_obj_t* aboutLabel_   = nullptr;   // 关于区文本
    bool      aboutOpen_    = false;
    bool      settingsOpen_ = false;

    // ---- 顶部下拉手势区（ADR-038）：下拉/轻点 = 设置页 ----
    lv_obj_t* pullZone_  = nullptr;
    int32_t   pullDy_    = 0;           // 本次按压累计纵向位移（手势判定）
    void buildCompanion();              // 鸭子页控件树（自绘鸭子 + 概览）
    void renderCompanion();             // 问候/概览/气泡（进页时刷）

    // 滑动手势（LV_EVENT_GESTURE，indev 级，与列表滚动共存）：
    // 手指左滑=进鸭子页（首页）/手指右滑=返回（详情/设置/鸭子页）
    void onSwipe(lv_dir_t dir);

    // ---- 录音覆盖层 / talk / 便签 ----
    lv_obj_t* recDot_    = nullptr;
    lv_obj_t* recLabel_  = nullptr;
    lv_obj_t* asrLabel_  = nullptr;  // talk：你说
    lv_obj_t* aiLabel_   = nullptr;  // talk：GAGA
    lv_obj_t* talkHint_  = nullptr;  // talk：空内容时的提示（LVGL 默认 "Text" 占位根治）
    lv_obj_t* noteCont_  = nullptr;  // 便签胶囊容器
    lv_obj_t* noteLabel_ = nullptr;  // 胶囊内文字

    // ---- 鸭子页（ADR-036）：白鹅 + 问候，极简品牌页 ----
    lv_obj_t* companionCont_ = nullptr;
    lv_obj_t* duckImg_       = nullptr;   // 吉祥物全屏图（screen 层，随鸭子页显隐）
    lv_obj_t* greetLabel_    = nullptr;   // 按时段问候
    bool      companionOpen_ = false;

    uint32_t noteShownAtMs_ = 0;
    bool     noteVisible_   = false;
    uint32_t lastRecSecShown_ = 0xFFFFFFFF;
    uint32_t renderedVersion_ = 0;   // 已渲染的 MsgLog 版本
    uint32_t lastStatusMs_    = 0;
    uint32_t lastDotsMs_      = 0;
    int      dotsPhase_       = 0;
    bool     renderedTimeout_  = false;  // 上次渲染时是否已有软超时卡
};

}  // namespace gaga

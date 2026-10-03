#pragma once

#include <functional>

// 应用侧注入的抽象输入动作集（ADR-078）：板层把物理按键映射到这些动作，
// 业务代码不感知具体按键——不同板子的激活方式（键位/数量/手势）各不相同，
// 键位分工是板级决策（1.75C 的分工见 boards/waveshare-s3-amoled-1_75c/board_input.cpp）。
namespace gaga {

struct InputActions {
    std::function<void()> wake;          // 亮屏/活动通知
    std::function<void()> recordToggle;  // 开始/结束录音（点按式，ADR-045 v2）
    std::function<void()> goHome;        // 回消息主页
    std::function<void()> talkToggle;    // 进入/退出实时对话（含链路检查，实现侧兜）
    std::function<void()> volumeUp;      // 音量+（给无触摸板预留，1.75C 不绑）
    std::function<void()> volumeDown;    // 音量-
};

}  // namespace gaga

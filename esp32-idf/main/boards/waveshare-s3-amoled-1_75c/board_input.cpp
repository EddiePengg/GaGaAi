// 1.75C 的物理输入接线：右上（AXP2101 PKEY）+ 右下（BOOT 键 GPIO0）→ 抽象动作。
// 激活方式是板级决策（ADR-078）：本文件的键位分工 = ADR-016/045 定稿，
// 换板子 = 换这份映射（甚至换触发源——ADC 阶梯键将来也能用 ButtonHandler 的
// reader 模式实现：每键一个实例，reader 判断 ADC 电压是否落在该键窗口），
// 业务代码（main.cpp）只注入 InputActions，不感知按键。
#include "board_175c.h"

#include "boards/common/board_selected.h"   // GAGA_PIN_BTN_BOOT
#include "input/ButtonHandler.h"
#include "input/PwrKey.h"

namespace gaga {

namespace {
ButtonHandler s_btnTop([] { return pwrKeyPressed(); });   // 右上：AXP2101 PKEY
ButtonHandler s_btnBottom(GAGA_PIN_BTN_BOOT);             // 右下：GPIO0 直读
InputActions  s_actions;
}  // namespace

void Board175c::wireInput(const InputActions& actions) {
    s_actions = actions;
    // PWR 键就绪才使能右上键（PMIC 缺席/初始化失败 = 禁用占位，不致命）。
    // pwrKeyInit 幂等：装配区若已按 caps.pmic 调过，这里秒回成功
    if (pwrKeyInit()) {
        s_btnTop.begin();
    }
    s_btnBottom.begin();

    // 亮屏唤醒挂"按下沿"而非"单击落定"：即按即亮（单击要等 300ms 双击窗口）
    if (s_actions.wake) {
        s_btnTop.onPressDown(s_actions.wake);
    }
    // 右下按下沿 = 立即开录（ADR-045 v2 定稿，用户拍板"马上就开始录，不允许
    // 丢内容"）：40ms 电气防抖是唯一延迟，开头的字一个不丢。松手不结束——
    // 点一下开始、说完再点一下结束，录音期间手可以离开（挂脖核心场景）。
    // 再按一下的收尾判定（<300ms 录制时长 = 静默丢弃双击误触）在 Recorder::toggle
    s_btnBottom.onPressDown([] {
        if (s_actions.wake) s_actions.wake();
        if (s_actions.recordToggle) s_actions.recordToggle();
    });

    // 右上单击：亮屏 + 回主页（手表式交互：从卡片/设置/鸭子页一键回消息列表；
    // 已在主页则无可见变化。灭屏交给自动息屏，单击灭屏职责取消——用户拍板）
    if (s_actions.goHome) {
        s_btnTop.onSingleClick(s_actions.goHome);
    }

    // 右上长按：进入/结束 realtime 对话（M5）。链路/MQTT 前置检查+提示在
    // 应用侧（main.cpp 注入的 talkToggle lambda）兜
    if (s_actions.talkToggle) {
        s_btnTop.onLongPress(s_actions.talkToggle);
    }

    // 右下不再注册单击/双击/松开回调：按下沿即录（上文），收尾在"再按一下"
    // 的按下沿。松手宽限/锁定模式已随按住说话退役（ADR-045 v2）
    // talk 期间 toggle→start() 内部拦截并提示"实时对话中，无法录音"。
}

void Board175c::inputTick() {
    s_btnTop.loop();
    s_btnBottom.loop();
}

}  // namespace gaga

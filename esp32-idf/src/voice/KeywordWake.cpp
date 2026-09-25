// 关键词唤醒（ADR-037）：esp-sr WakeNet9 "Hi,乐鑫"。
// ⚠️ 当前为占位实现（esp-sr 暂缓启用）：静态链接吃 ~20KB 内部 RAM（低点 5KB
// 引发连锁内存踩踏，真机 boot loop 实锤）+ srmodel mmap StoreProhibited 未解。
// 完整实现（AFE 单例/两段任务/线程安全关闭）在 git 历史与本文件下方的
// 注释版里；恢复 = 恢复 idf_component.yml/CMakeLists 的 esp-sr 三处 +
// sdkconfig（SR_WN_WN9_HILEXIN 等）+ 烧 srmodels.bin（README 烧录节）。
#include "voice/KeywordWake.h"

#include "esp_log.h"

namespace gaga {

static const char* TAG = "gaga.kws";

void KeywordWake::begin(AppContext* ctx) {
    ctx_ = ctx;
    ESP_LOGW(TAG, "keyword wake 未启用（esp-sr 暂缓，见 ADR-037 备注）");
}

bool KeywordWake::setEnabled(bool on) {
    ESP_LOGW(TAG, "语音唤醒不可用（固件未编入 esp-sr）");
    return false;
}

}  // namespace gaga

// AFE 的 NS 模型工厂 stub（ADR-037 链接期瘦身）
// 为什么存在：libnsnet.a 的真工厂（esp_nsn_models.c.obj）无条件引用
// esp_nsnet3/gtcrn 的 RAM 权重表（101KB DRAM：gtcrn_weights_ram 84.8KB +
// gtcrn_math 16KB），而我们只用 WebRTC NS（不调工厂）——链接即 DRAM 段溢出。
// 本文件以同名强定义顶替，链接器不再拉入 nsnet3/gtcrn 目标文件。
// ⚠️ 将来若要启用深度降噪（nsnet3/gtcrn），删除本文件重新链接即可。
#include <stddef.h>

// 真签名见 esp_nsn_models.h：const esp_nsn_iface_t *esp_nsnet_handle_from_name(char *)
// 这里只断链不复刻类型（返回 void* 会有签名漂移风险——用原类型请 include 头，
// 但头文件会拉进 nsnet2/3 模型声明，故保持最小化）
#ifdef __cplusplus
extern "C" {
#endif
void* esp_nsnet_handle_from_name(char* model_name);
#ifdef __cplusplus
}
#endif

void* esp_nsnet_handle_from_name(char* model_name) {
    (void)model_name;  // WebRTC NS 不走模型表；仅 ns_model_name 配 "nsnet3"/"gtcrn" 时才会被调
    return NULL;
}

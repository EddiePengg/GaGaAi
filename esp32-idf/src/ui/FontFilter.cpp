// 设备端文本过滤（ADR-032）：动态文本（receipt/reply/error/talk 字幕）入库/上屏前，
// 剔除字体渲染不了的字符，防止 LVGL 画出方块字（tofu）。
//
// 为什么在设备端而不是服务端共享字符集 JSON：查的就是实际烧进本机的
// gaga_font_cjk_16 字形表（lv_font_get_glyph_dsc）——运行时自证，字库升级/
// 回滚都不会与服务端的表漂移（共享表一旦和已刷固件不同步，方块字照旧）。
// 字符查表是微秒级，续航与服务端过滤无差别。
//
// 语义：
// - \n 白名单（换行是布局语义，不是字形；字体从 0x20 起步查不到它）
// - 非法/截断的 UTF-8 字节序列整体丢弃（不产生新乱码）
// - 其余字符查不到字形 → 连同其 UTF-8 字节整体删除（消失比方块体面）
#include "ui/FontFilter.h"

#include <cstring>

#include "lvgl.h"

extern const lv_font_t gaga_font_cjk_16;  // 自生子集字库（assets/fonts/）

namespace gaga {

size_t fontFilterDisplayable(char* s) {
    if (s == nullptr) return 0;
    const size_t len = strlen(s);
    size_t r = 0, i = 0;
    lv_font_glyph_dsc_t dsc;
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t cl = 0;
        if (c < 0x80) {                              // ASCII
            cp = c; cl = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len
                   && (s[i+1] & 0xC0) == 0x80) {     // 2 字节
            cp = c & 0x1F; cl = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len
                   && (s[i+1] & 0xC0) == 0x80 && (s[i+2] & 0xC0) == 0x80) {  // 3 字节（CJK）
            cp = c & 0x0F; cl = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len
                   && (s[i+1] & 0xC0) == 0x80 && (s[i+2] & 0xC0) == 0x80
                   && (s[i+3] & 0xC0) == 0x80) {     // 4 字节（emoji 常住区）
            cp = c & 0x07; cl = 4;
        } else {                                     // 非法/截断序列
            i++;
            continue;
        }
        for (size_t k = 1; k < cl; k++) {            // 续字节拼码点
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        const bool keep =
            cp == '\n' ||
            lv_font_get_glyph_dsc(&gaga_font_cjk_16, &dsc, cp, '\0');
        if (keep) {
            if (r != i) memmove(s + r, s + i, cl);
            r += cl;
        }
        i += cl;
    }
    s[r] = '\0';
    return r;
}

}  // namespace gaga

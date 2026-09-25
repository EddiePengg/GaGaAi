#pragma once

#include <cstddef>

namespace gaga {

// 原地剔除 gaga_font_cjk_16 渲染不了的字符（emoji、字库外符号），防方块字。
// 返回过滤后的字节数（不含结尾 \0）。详见 FontFilter.cpp 头注与 ADR-032。
size_t fontFilterDisplayable(char* s);

}  // namespace gaga

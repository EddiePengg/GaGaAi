# ESP-IDF 工程根 CMakeLists（PlatformIO 复用同一套：is_proper_idf_project 判定）
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gaga_idf)

# PlatformIO 的 IDF 构建器对 app 组件编译选项做 sorted()（builder/frameworks/espidf.py
# 的 get_app_flags），会把 esp_lvgl_adapter 经 PUBLIC 传播的
# `-include <abs-path>/lvgl_port_alignment.h` 二元组拆散——头文件路径被 g++ 当成第二个
# 输入文件，报 "cannot specify '-o' with '-c' ... with multiple files"。
# 该头只定义带 #ifndef 默认值的 PPA 对齐宏（LVGL_PORT_PPA_ALIGNMENT 已由 -D 单独传播；
# LVGL_PORT_PPA_MAX_PENDING_TRANS 仅适配器内部 .c 使用，走的是组件自身 PRIVATE 选项），
# 因此从 INTERFACE_COMPILE_OPTIONS 移除不影响任何组件语义。上游修复 sorted() 拆对后此段可删。
foreach(_comp espressif__esp_lvgl_adapter lvgl__lvgl)
    idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
    if(TARGET ${_lib})
        get_target_property(_opts ${_lib} INTERFACE_COMPILE_OPTIONS)
        if(_opts)
            set(_clean "")
            foreach(_o IN LISTS _opts)
                if(NOT _o STREQUAL "-include" AND NOT _o MATCHES "lvgl_port_alignment")
                    list(APPEND _clean "${_o}")
                endif()
            endforeach()
            set_target_properties(${_lib} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_clean}")
        endif()
    endif()
endforeach()

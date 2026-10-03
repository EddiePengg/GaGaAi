# gaga-idf 主组件（PlatformIO 的 src/ = IDF 的 main 组件）
# REQUIRES 里的 managed component 名 = <namespace>__<name>（双下划线），
# 依赖声明在 idf_component.yml，构建时由 IDF Component Manager 拉取。
idf_component_register(
    SRCS
        "main.cpp"
        "protocol/frame.cpp"
        "ble/GattServer.cpp"
        "net/WifiTransport.cpp"    # 家模式链路：WiFi 直连 MQTT（ADR-039 B 期落地）
        "net/LinkManager.cpp"      # 链路总机：BLE/WiFi 互斥 + 自动故障转移
        "input/ButtonHandler.cpp"
        "input/PwrKey.cpp"
        "input/Rtc.cpp"
        "state/AppState.cpp"
        "state/MsgLog.cpp"
        "state/Settings.cpp"
        "ui/Display.cpp"
        "ui/Ui.cpp"
        "ui/FontFilter.cpp"
        "ui/fonts/gaga_font_cjk_16.c"
        "ui/assets/img_duck.c"
        "ui/assets/img_duck16.c"   # 卡片答行 16px 鸭图标
        "audio/AudioPipe.cpp"
        "audio/OpusCodec.cpp"
        "audio/OggDemux.cpp"
        "audio/UplinkPump.cpp"
        "rec/Recorder.cpp"
        "talk/TalkSession.cpp"
        "notify/NotifySession.cpp"  # 服务器主动 TTS 通知：上屏 + ogg_opus 播报
        "motion/Qmi8658.cpp"
        "motion/Wake.cpp"
        "boards/waveshare-s3-amoled-1_75c/board_175c.cpp"  # Board 子类（DECLARE_BOARD 登记）
        "boards/waveshare-s3-amoled-1_75c/board_input.cpp" # 板级输入映射（物理键→InputActions）
        "power/Power.cpp"
        "voice/KeywordWake.cpp"   # 语音唤醒（esp-sr WakeNet，'W'/设置页开关拉起）
        "voice/NsnetStub.c"       # NS 模型工厂同名顶替：断 nsnet3/gtcrn 101KB RAM 权重链接（ADR-037）
        "audio/snd_quack.c"       # 鸭叫采样数据

        "debug/SerialCmd.cpp"
    INCLUDE_DIRS "." "boards/common" "boards/waveshare-s3-amoled-1_75c"
    REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75c  # BSP：显示/触摸/I2C 总线
        lvgl__lvgl                              # LVGL 9（UI）
        espressif__esp_lvgl_adapter             # BSP 的 LVGL 拉起层（Display 自组链路用）
        espressif__esp_audio_codec              # Opus 编解码（直接调 esp_opus_enc/dec_*）
        bt                                      # NimBLE host
        espressif__esp-sr                       # WakeNet 语音唤醒（ADR-037；KWS 开关拉起）
        esp_wifi                                # 家模式 STA（LinkManager/WifiTransport）
        mqtt                                    # esp-mqtt 客户端（家模式直连 broker）
        esp_netif                               # STA 网卡（WifiTransport 建/销）
        esp_event                               # WiFi/IP/MQTT 事件循环
        nvs_flash
        json                                    # cJSON（talk 信令/reply 文本解析）
        esp_timer                               # compat.h 的 millis()
        esp_driver_gpio                         # BOOT 键 GPIO0
        esp_driver_i2s
        esp_driver_i2c
        esp_lcd
        esp_hw_support
)

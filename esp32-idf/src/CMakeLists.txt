# gaga-idf 主组件（PlatformIO 的 src/ = IDF 的 main 组件）
# REQUIRES 里的 managed component 名 = <namespace>__<name>（双下划线），
# 依赖声明在 idf_component.yml，构建时由 IDF Component Manager 拉取。
idf_component_register(
    SRCS
        "main.cpp"
        "protocol/frame.cpp"
        "ble/GattServer.cpp"
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
        "audio/AudioPipe.cpp"
        "audio/OpusCodec.cpp"
        "audio/OggDemux.cpp"
        "audio/UplinkPump.cpp"
        "rec/Recorder.cpp"
        "talk/TalkSession.cpp"
        "motion/Qmi8658.cpp"
        "motion/Wake.cpp"
        "power/Power.cpp"
        "voice/KeywordWake.cpp"   # 语音唤醒存根（esp-sr 已从构建移除）
        "audio/snd_quack.c"       # 鸭叫采样数据

        "debug/SerialCmd.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75c  # BSP：显示/触摸/I2C 总线
        lvgl__lvgl                              # LVGL 9（UI）
        espressif__esp_lvgl_adapter             # BSP 的 LVGL 拉起层（Display 自组链路用）
        espressif__esp_audio_codec              # Opus 编解码（直接调 esp_opus_enc/dec_*）
        bt                                      # NimBLE host
        nvs_flash
        json                                    # cJSON（talk 信令/reply 文本解析）
        esp_timer                               # compat.h 的 millis()
        esp_driver_gpio                         # BOOT 键 GPIO0
        esp_driver_i2s
        esp_driver_i2c
        esp_lcd
        esp_hw_support
)

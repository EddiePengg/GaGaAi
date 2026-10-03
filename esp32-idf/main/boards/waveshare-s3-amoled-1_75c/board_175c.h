#pragma once

#include "boards/common/Board.h"

// 微雪 ESP32-S3-Touch-AMOLED-1.75C 的 Board 实现（当前唯一在册板子）。
// 薄实现：i2cBus 转调 BSP、bootstrap 能力清单、GPIO0 ext0 唤醒——
// 引脚数值全在 board_config.h，这里不写真值。
namespace gaga {

class Board175c : public Board {
public:
    const char* name() const override { return "waveshare-s3-amoled-1_75c"; }

    i2c_master_bus_handle_t i2cBus() override;
    const BoardCaps& caps() const override;
    void setupWakeupSources() override;
    void wireInput(const InputActions& actions) override;
    void inputTick() override;
};

}  // namespace gaga

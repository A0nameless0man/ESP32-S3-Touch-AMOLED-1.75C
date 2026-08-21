# examples/arduino — Arduino 示例与 vendored 库

7 个 sketch + 6 个 vendored 库(LVGL 8.4.0 / GFX 1.6.4 / SensorLib 0.3.3 / XPowersLib 0.2.6 / Mylibrary / lv_conf.h)。构建入口与板级硬件见根 AGENTS.md,此处只写本目录特有内容。

## WHERE TO LOOK

| 示例 | 功能 | 关键点 |
|------|------|--------|
| 01_HelloWorld | GFX 直绘 | `Arduino_ESP32QSPI` 总线 + `Arduino_CO5300` 面板的标准实例(45 行) |
| 02_GFX_AsciiTable | GFX 字符表 | — |
| 03_LVGL_AXP2101_ADC_Data | LVGL 电源遥测 | XPowersLib 用法;`adcOn/adcOff` 控制 PMU 通道 |
| 04_LVGL_QMI8658_ui | LVGL IMU 数据 | SensorLib 读 QMI8658 |
| 05_LVGL_Widgets | LVGL 组件+触摸 | 最完整 UI 参考(248 行) |
| 06_ES7210 | 双 mic 录音 | 本地驱动 es7210.cpp/.h + audio_hal.h(不经库) |
| 07_ES8311 | 音频播放 | 本地驱动 es8311.c/.h + canon.h PCM 样本 |

- 显示实例化模板:`01_HelloWorld.ino:6-13`(QSPI 引脚来自 pin_config.h)。
- LVGL v8 移植样板(`my_disp_flush`/`rounder_cb`/`lv_tick`):任一 03/04/05 sketch,三者代码近乎相同,新 UI 从 05 复制起步。

## CONVENTIONS(与父文档不同处)

- **sketch 依赖解析**:全部 `#include "pin_config.h"`(Mylibrary)+ vendored 库;编译时库目录必须指向 `examples/arduino/libraries/`。
- LVGL 配置不在库内而在 `libraries/lv_conf.h`(色深 16、LV_MEM_SIZE 48K、刷新周期 10ms)—— 改渲染行为改这里,不改库源码。
- 06/07 的音频驱动是 sketch 本地文件(与 .ino 同目录),不进 libraries。

## ANTI-PATTERNS (THIS PROJECT)

- **勿把 LVGL 升到 v9**:全部 sketch 用 v8 API(`lv_disp_drv_t`/`lv_disp_flush_t`),v9 是 esp-idf 侧的事。
- vendored 库 = 锁定版本的上游代码;必须修改时先确认上游是否有同题修复,改动处留注释标记。
- 勿重命名示例目录或 .ino 的历史编号(两者编号本就不一致,CI/文档按目录名路由,见根 NOTES)。

# examples/esp-idf — ESP-IDF 示例(5 个独立工程)

每目录一个可独立 `idf.py build` 的工程,统一走 Waveshare BSP `waveshare/esp32_s3_touch_amoled_1_75c ^3.0.0`(组件管理器拉取,本地不提交)。板级硬件与构建命令见根 AGENTS.md。

## WHERE TO LOOK

| 工程 | 功能 | 关键点 |
|------|------|--------|
| 01_AXP2101 | PMU 裸 I2C 驱动 | **唯一不用 BSP** 的工程;port_axp2101.cpp 30+ static 寄存器函数;无 sdkconfig.defaults |
| 02_lvgl_demo_v9 | LVGL 9.5 demo | 最小 BSP 用法:`bsp_display_start()` + `lv_demo_benchmark()`(main.c 共 25 行) |
| 03_esp-brookesia | 手机 UI 框架 | vendored brookesia_core 组件在 components/;自带 phone/speaker 资源 |
| 04_Immersive_block | IMU 物理小游戏 | 自有代码最大(527 行):QMI8658 + 碰撞检测;物理参数为 main.c 头部宏 |
| 05_Spec_Analyzer | 麦克风 FFT 频谱 | components/bsp_extra 提供 codec/I2S(`bsp_extra_codec_*`);esp-dsp 做 Hann 窗 FFT |

- 新建 LVGL 工程的起点:复制 02,改 idf_component.yml 依赖。
- PSRAM 一致配置(OCT/80M + XIP)在各工程 sdkconfig.defaults;LVGL 渲染单元数不一(02/05=2,04=1),改动前理解内存权衡。

## CONVENTIONS(与父文档不同处)

- 依赖只写 `main/idf_component.yml`(lvgl 9.5.0、esp-dsp、esp-audio-player 等),构建时拉 managed_components(gitignored)。
- 只提交 sdkconfig.defaults;首次构建生成 sdkconfig 后勿提交。
- 03 的 components/ 是 vendored esp-brookesia 上游拷贝,自有逻辑只放 main.cpp(130 行)。

## ANTI-PATTERNS (THIS PROJECT)

- 01_AXP2101 是裸寄存器驱动,**不要**给它加 BSP 依赖——它存在的意义就是演示无 BSP 的 PMU 操作。
- brookesia 动画资源必须 startRecordResource/stopRecordResource 包裹(见根 ANTI-PATTERNS),03 内自由 UI 开发时最易踩。
- 05 的 bsp_extra 是本仓库自有组件(非上游),可改;但接口风格(esp_err_t 返回、bsp_extra_ 前缀)保持一致。

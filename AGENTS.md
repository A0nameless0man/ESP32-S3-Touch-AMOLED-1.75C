# PROJECT KNOWLEDGE BASE

**Generated:** 2026-08-21
**Commit:** e86eff9
**Branch:** main (fork of github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)

## OVERVIEW

Waveshare ESP32-S3-Touch-AMOLED-1.75C 官方 demo 仓库的个人 fork:12 个独立可构建示例(ESP-IDF 5 + Arduino 7)+ CI 固件发布流水线。用户计划在此上游框架之上实现自有想法(见 NOTES)。

**硬件**:ESP32-S3 | 1.75" 466×466 QSPI AMOLED(CO5300 驱动)| CST9217 电容触摸(I2C)| AXP2101 PMU | QMI8658 六轴 IMU | ES7210 双 mic ADC + ES8311 codec | 功放使能 GPIO46。

## STRUCTURE

```
./
├── examples/
│   ├── arduino/          # 7 个 sketch + vendored 库(LVGL v8.4/GFX/SensorLib/XPowersLib),见其 AGENTS.md
│   └── esp-idf/          # 5 个 IDF 工程(BSP + LVGL v9),见其 AGENTS.md
├── scripts/              # CI 路由:classify_changes.py(变更分类)、discover_examples.py(构建矩阵发现)
├── tests/                # 上述 Python 脚本的 pytest(非嵌入式测试)
├── releases/             # 固件打包/校验脚本 + release notes(package_firmware.py、prepare_release_assets.py)
├── docs/                 # 双语维护文档(ci/components/firmware/repository-structure)
├── Firmware/             # 出厂恢复固件 bin(FactoryOnly)
├── Schematic/            # 原理图 PDF
└── ci-routing-config.json / markdown-audit-config.json   # CI 路由规则与文档审计规范
```

## WHERE TO LOOK

| 任务 | 位置 |
|------|------|
| 板级引脚定义(唯一定义点) | examples/arduino/libraries/Mylibrary/pin_config.h |
| Arduino LVGL 配置(16bpp/48K mem) | examples/arduino/libraries/lv_conf.h |
| ESP-IDF 依赖与 BSP 版本 | examples/esp-idf/*/main/idf_component.yml |
| 某示例为什么没被 CI 构建 | ci-routing-config.json + scripts/classify_changes.py |
| 新增示例后 CI 如何发现 | scripts/discover_examples.py(扫描目录自动生成矩阵) |
| 固件包如何打包/校验 | releases/package_firmware.py、prepare_release_assets.py |
| CI 工作流全貌 | .github/workflows/examples.yml |

## CODE MAP(自有代码;vendored 库不计)

| 符号 | 类型 | 位置 | 引用 | 角色 |
|------|------|------|------|------|
| `pin_config.h` 宏组(LCD_*/IIC_*/TP_*/PIN_ES*) | 宏 | examples/arduino/libraries/Mylibrary/ | 7/7 sketch | 板级硬件抽象唯一来源 |
| `my_disp_flush` / `example_lvgl_rounder_cb` / `example_increase_lvgl_tick` | 函数 | 03/04/05 Arduino sketch 内重复定义 | — | LVGL v8 显示移植样板(每 sketch 复制一份) |
| `axp2101_*`(30+ static 函数) | 函数 | examples/esp-idf/01_AXP2101/main/port_axp2101.cpp | 仅 01 | 裸 I2C PMU 驱动(不经 BSP) |
| `bsp_extra_codec_*` | 函数 | examples/esp-idf/05_Spec_Analyzer/components/bsp_extra | 仅 05 | 音频 codec/I2S 补充层 |
| `app_main` | 入口 | examples/esp-idf/*/main/ | 各工程 | IDF 入口 |

## CONVENTIONS

- **双语文档成对**:每个 `X.md` 必有 `X_ZH.md`;README 契约由 tests/test_homepage_contract.py 强制(徽章/hero 图/H2 图标)。
- **命名**:函数/变量 lower_snake;宏 UPPER_SNAKE;ESP-IDF 日志 TAG 为小写字符串字面量。
- **每个示例 = 独立构建单元**:IDF 工程各有 CMakeLists.txt/sdkconfig.defaults/partitions.csv;Arduino 各有 .ino。
- **Arduino 库 vendored 在 `examples/arduino/libraries/`**(非仓库根),锁定旧版本保证可复现。

## ANTI-PATTERNS (THIS PROJECT)

- **禁止混刷不同 release 的固件组件**(bootloader/partition/app 交叉混用 → 校验和失败,见 docs/firmware.md)。
- esp-brookesia 中使用动画资源必须 `startRecordResource`/`stopRecordResource` 包裹,否则崩溃/泄漏(03 示例 components/brookesia_core/docs)。
- 不编辑 vendored 库内生成文件(如 lvgl/src/lv_conf_internal.h)。
- 不提交 build/、managed_components/、sdkconfig(已 gitignore;IDF 工程只提交 sdkconfig.defaults)。
- 公开文本(截图/日志)不得泄露本地路径/用户名(CONTRIBUTING)。

## COMMANDS

```bash
# ESP-IDF 示例(先 export esp-idf 环境;CI 用 v5.5.5 与 v6.0.2 双版本)
cd examples/esp-idf/02_lvgl_demo_v9 && idf.py set-target esp32s3 && idf.py build flash monitor

# Arduino 示例(CI 用 core 3.3.11;库目录指到 examples/arduino/libraries)
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries examples/arduino/libraries examples/arduino/examples/01_HelloWorld

# CI 脚本测试
python -m pytest tests/
```

## NOTES

- **上游策略**:本仓库是 fork,当前与上游同步于 e86eff9;自有想法实现时应放在新目录或独立示例编号,避免改动上游示例文件,便于日后 rebase 上游。
- **CI 路由规则要点**:纯 `*.md` 变更 → docs_only 跳过构建;`examples/arduino/libraries/**` 变更 → Arduino 全量 7 示例;`.github/scripts/tests/releases` 变更 → 双框架全量;tag `v*` 触发发布,预期 17 个固件包(IDF 5×2 版本 + Arduino 7)。
- **GPIO2 复用**:LCD_RESET 与 TP_RST 同为 GPIO2(pin_config.h),复位时序影响两者。
- Arduino 侧 LVGL 为 v8.4(API 如 `lv_disp_drv_t`),ESP-IDF 侧为 v9.5 —— 两套 API 不兼容,移植代码时勿直接复制。
- 目录编号与 .ino 文件编号不一致(如 02_GFX_AsciiTable/03_*.ino),属上游历史命名,勿"顺手修正"(CI 与文档按目录名路由)。
- **全屏动画设计阈值(用户定)**:无分带时全屏滚动速度≤4px/帧(实测全屏帧率 14.5fps→≈58px/s);更快的动效需分带渲染或缩小刷新面积。
- **Arduino 侧双核流水线结论(08_TE_probe 最终版)**:双信号量 SPSC 双缓冲(producer 阻塞在信号量而非自旋,watchdog 友好;consumer 取引用后立即释放 back buffer 实现重叠)→ 全屏 17.2fps,瓶颈 100% 在 54ms 总线传输;GFX 库的 QSPI 写是轮询式 DMA(POLL_START 紧跟 POLL_END),单核无重叠。相位必须随被消费帧推进,否则显示跳相位(违反条纹 3 倍规则时出现时光倒流现象)。
- **4×N 分带 TE 同步架构(用户方案,实测封版 N=8)**:整帧 B=4N 条带(466=233×2 行对,偶对齐分配),每 TE 窗口按余数类 0,2,1,3 交错刷新 N 条,帧恒 4 个 TE 周期(67ms/14.8fps,与 N 无关);写入逐条带追扫描线(等扫描 27.9行/ms 越过底行再写)。四关键坑:①条带必须偶数高/偶数起点(奇数窗口→面板卡行,Waveshare rounder_cb 暗示);②N/模式切换必须在帧末,帧中 return 丢帧→周期性顿挫;③HWCDC 无主机读时每次写阻塞~2s(core 3.3.11),日志必须 `Serial.isConnected()` 门控;④顶部条带 TE 后立即写会追尾扫描线,必须逐带追扫描延迟。N 留升级口:80MHz 时钟→2 窗/帧 30fps;圆屏弦裁剪-21.5% 带宽→可压 3 窗。
- **03 工程 TE_SYNC patch 完成(2026-08-21)**:三处改动——vendored BSP(components/esp32_s3_touch_amoled_1_75c)+ 头文件加 `BSP_LCD_TE=GPIO13` + disp_cfg 加 `.te_sync{gpio_num=13, bus_freq_hz=40MHz, data_lines=4, bpp=16}`;main.cpp 改 `TEAR_AVOID_MODE_TE_SYNC`。**关键坑**:panel_io 必须设 `io_config.flags.psram_dma_direct=true`(CO5300_PANEL_IO_QSPI_CONFIG 宏不设,否则每次 flush 从内部 DMA 堆 bounce 整块传输区→ESP_ERR_NO_MEM,co5300 2.1.0 下必现)。adapter 自动识别 TE 下降沿、periods=2(全帧 21.7ms>13ms 窗口,分两个 TE 周期)。本地构建链:~/esp/esp-idf v5.5.5(Windows 用 ~/esp/build03.bat、flash03.bat,需清 MSYSTEM);IDF 组件直链下载:`https://components-file.espressif.com/components/<ns>/<name>/<ver>/<ns>__<name>-v<ver>.zip`。
- **显示撕裂/垂直同步(2026-08-21 调查+真机验证)**:CO5300 面板 TE 信号走线 **LCD_TE→GPIO13**(原理图坐标级验证 + 真机探针实测:60Hz 帧脉冲、占空 ~4%,vendored Arduino init 后即激活,无需额外发 0x35;探针 sketch 见 examples/arduino/examples/08_TE_probe)。pin_config.h 中 GPIO13 空闲,勿占用。官方 esp_lcd_co5300 驱动 init 已含 TEARON(esp_lcd_co5300_spi.c:202);esp_lvgl_adapter 0.6 自带 TE 同步模块(`te_sync.gpio_num` + `ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC`,源码 esp-iot-solution/components/display/tools/esp_lvgl_adapter/src/display/display_te_sync.c)。Waveshare BSP 3.0.0 未启用 te_sync → 出厂固件交互时撕裂。Arduino 侧 CO5300 init 的 TEARON 被注释且 QSPI 总线只写不读。QSPI 40MHz 整帧 ~87ms > 60Hz 帧周期,无同步必撕裂。出厂固件=combined 镜像:esp-brookesia UI(app@0x110000)+ xiaozhi AI(app@0xa10000);芯片实测 S3 v0.2 + 32MB GD flash + 8MB Octal PSRAM,出厂 app 用 esp_lcd_co5300 v2.0.3 + 外部 init 序列覆盖 3Ah。

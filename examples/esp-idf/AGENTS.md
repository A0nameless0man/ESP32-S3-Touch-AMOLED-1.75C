# examples/esp-idf — ESP-IDF 示例(5 个独立工程)

每目录一个可独立 `idf.py build` 的工程,统一走 Waveshare BSP `waveshare/esp32_s3_touch_amoled_1_75c ^3.0.0`(组件管理器拉取,本地不提交)。板级硬件与构建命令见根 AGENTS.md。

## WHERE TO LOOK

| 工程 | 功能 | 关键点 |
|------|------|--------|
| 01_AXP2101 | PMU 裸 I2C 驱动 | **唯一不用 BSP** 的工程;port_axp2101.cpp 30+ static 寄存器函数;无 sdkconfig.defaults |
| 02_lvgl_demo_v9 | LVGL 9.5 demo | 最小 BSP 用法:`bsp_display_start()` + `lv_demo_benchmark()`(main.c 共 25 行) |
| 03_esp-brookesia | 手机 UI 框架 | vendored brookesia_core + 自有 brookesia_app_wifi_config 组件(见下) |
| 04_Immersive_block | IMU 物理小游戏 | 自有代码最大(527 行):QMI8658 + 碰撞检测;物理参数为 main.c 头部宏 |
| 05_Spec_Analyzer | 麦克风 FFT 频谱 | components/bsp_extra 提供 codec/I2S(`bsp_extra_codec_*`);esp-dsp 做 Hann 窗 FFT |

- 新建 LVGL 工程的起点:复制 02,改 idf_component.yml 依赖。
- PSRAM 一致配置(OCT/80M + XIP)在各工程 sdkconfig.defaults;LVGL 渲染单元数不一(02/05=2,04=1),改动前理解内存权衡。

## 03 自有组件:brookesia_app_wifi_config

配网 app + 圆屏裁剪绘制模块,真机多轮迭代收敛,排障方法论有价值:

- **circular_draw(hpp/cpp)**:圆屏通用裁剪。`attach_circle_clip(容器, ρ, bg)` 挂 DRAW_POST,遍历直接子对象用各自矩形独立做几何:环带裁掉圆外全部 + 边×圆交叉处 G1 相切圆角(盘与边相切、与参考圆内切)。对象在圆内零绘制,可无差别挂任意行容器。
- **三个踩过的坑**:① `lv_draw_arc` 的 `radius` 是**外缘**非中心线(源码 `lv_DRAW_arc_get_area: rout=radius`,环带=[radius-width, radius]),按中心线传参内缘砍进 width/2;② nano newlib printf 不支持 `%f` 且**整行静默丢弃**,打小数用定点×10;③ 咬角楔块两段弧采样半径外推 2px,否则抗锯齿残边留在键色上成细枝。
- 裁剪参考圆定版 `screen_circle(7)`(R=226):物理发光圆实测≈232,切割线比黑边内收 7px 是真机观感选定。
- **排障方法论**:屏幕级 overlay 画几何标记(不受行裁剪/子对象覆盖影响)+ 串口几何真值双通道对照;日志限流用预算制(打印行数)而非帧计数——spinner 动画帧会把帧计数快速推高,错过用户真正打开的页面。
- WiFi 后端:esp_event 任务只写状态快照,LVGL 250ms 轮询消费;断开自动重试×3(reason=201 不重试)——拥挤 2.4G 下单次认证超时是常态,UI 必须当瞬态处理。

## CONVENTIONS(与父文档不同处)

- 依赖只写 `main/idf_component.yml`(lvgl 9.5.0、esp-dsp、esp-audio-player 等),构建时拉 managed_components(gitignored)。
- 只提交 sdkconfig.defaults;首次构建生成 sdkconfig 后勿提交。
- 03 的 components/ 是 vendored esp-brookesia 上游拷贝 + 自有 brookesia_app_wifi_config;改上游文件要谨慎(fork 策略),自有逻辑放自己的组件目录。

## ANTI-PATTERNS (THIS PROJECT)

- 01_AXP2101 是裸寄存器驱动,**不要**给它加 BSP 依赖——它存在的意义就是演示无 BSP 的 PMU 操作。
- brookesia 动画资源必须 startRecordResource/stopRecordResource 包裹(见根 ANTI-PATTERNS),03 内自由 UI 开发时最易踩。
- 05 的 bsp_extra 是本仓库自有组件(非上游),可改;但接口风格(esp_err_t 返回、bsp_extra_ 前缀)保持一致。

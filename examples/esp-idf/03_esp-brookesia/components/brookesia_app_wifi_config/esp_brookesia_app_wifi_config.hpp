/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief WiFi 连接配置 app:扫描列表 + LVGL 内置软键盘输入密码 + NVS 保存 + 开机自动重连
 *
 * 线程模型(刻意设计,勿改):
 *   - esp_event 任务只写 s_state / s_aps / s_ip 快照,绝不碰 LVGL 对象
 *   - LVGL 定时器是唯一读快照的地方,状态变化才刷新 UI
 *   - 扫描/连接只可能由 LVGL 上下文发起,SCAN_DONE 在拷贝完成后才置位,
 *     因此 s_aps 无需加锁(协议上保证不并发读写)
 */
class WifiConfigApp: public systems::phone::App {
public:
    static WifiConfigApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~WifiConfigApp();

    using systems::phone::App::startRecordResource;
    using systems::phone::App::endRecordResource;

protected:
    WifiConfigApp(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool init(void) override;
    bool cleanResource(void) override;
};

} // namespace esp_brookesia::apps

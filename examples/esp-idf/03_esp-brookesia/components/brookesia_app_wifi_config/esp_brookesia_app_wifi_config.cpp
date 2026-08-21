/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:WiFi"
#include "esp_lib_utils.h"
#include "esp_brookesia_app_wifi_config.hpp"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
// ip4_addrN 宏来自 lwip(经 esp_netif 也会间接包含,显式声明依赖亚稳)
#include "lwip/ip4_addr.h"

#define APP_NAME "WiFi Setup"

// 可展示的 AP 数上限;扫描结果按 RSSI 降序,取前 N 即可
#define AP_LIST_MAX 12

using namespace std;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

LV_IMG_DECLARE(esp_brookesia_app_icon_launcher_wifi_126_126);

namespace esp_brookesia::apps {

// ============================ WiFi 后端(无 LVGL 依赖) ============================

enum WifiUiState {
    WIFI_UI_IDLE, WIFI_UI_SCANNING, WIFI_UI_SCAN_DONE,
    WIFI_UI_CONNECTING, WIFI_UI_CONNECTED, WIFI_UI_FAILED, WIFI_UI_DISCONNECTED,
};

static volatile WifiUiState s_state = WIFI_UI_IDLE;
static wifi_ap_record_t s_aps[AP_LIST_MAX];
static volatile int s_ap_count = 0;
static char s_ip[16] = "";          // IP_EVENT_STA_GOT_IP 填写
static char s_conn_ssid[33] = "";   // 正在连接/已连接的 SSID
static char s_conn_pass[65] = "";   // 连接成功后由事件任务存入 NVS
static char s_pending_ssid[33] = ""; // 密码对话框选中的 SSID
static bool s_wifi_stack_inited = false;
// 连接失败原因码(WIFI_EVENT_STA_DISCONNECTED 的 reason),用于状态栏提示;串口同步打出详细日志
static volatile int s_fail_reason = 0;
// 自动重试计数:mesh/射频抖动兜底;超过 3 次才判 FAILED
static volatile int s_retry_count = 0;
#define WIFI_CONNECT_MAX_RETRY 3

// 常见 reason 的可读解释(透明化:具体原因而非泛泛的"密码错误")
static const char *fail_reason_text(int r)
{
    switch (r) {
    case 202: return "Auth failed\n(wrong password?)";
    case 15:  return "Handshake timeout\n(wrong password?)";
    case 2:   return "Auth timeout";
    case 201: return "AP not found";
    case 203: return "No handshake reply";
    case 204: return "Assoc rejected";
    case 205: return "Conn rejected";
    default:  return "See serial log";
    }
}

#define NVS_NS "wifi_cfg"

static void save_credentials_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "ssid", s_conn_ssid, strlen(s_conn_ssid));
    nvs_set_blob(h, "pass", s_conn_pass, strlen(s_conn_pass));
    nvs_commit(h);
    nvs_close(h);
}

static bool load_credentials_nvs(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ssid_len;
    bool ok = nvs_get_blob(h, "ssid", ssid, &len) == ESP_OK && len > 0;
    ssid[len] = 0;
    len = pass_len;
    nvs_get_blob(h, "pass", pass, &len);
    if (ok) {
        pass[len] = 0;
    }
    nvs_close(h);
    return ok;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        // esp_wifi 扫描默认按 RSSI 降序,直接取前 N 条
        uint16_t cnt = AP_LIST_MAX;
        esp_wifi_scan_get_ap_records(&cnt, s_aps);
        s_ap_count = cnt;
        // 每个 AP 的加密类型/信道/信号强度进串口,配网问题时一次交互拿全信息
        for (uint16_t i = 0; i < cnt; i++) {
            ESP_UTILS_LOGI("AP[%u] ssid='%s' rssi=%d authmode=%d ch=%d", i,
                           (const char *)s_aps[i].ssid, s_aps[i].rssi, s_aps[i].authmode, s_aps[i].primary);
        }
        // 先拷贝完再置位,LVGL 侧看到 SCAN_DONE 时数据已就绪
        s_state = WIFI_UI_SCAN_DONE;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        // 详细原因进串口:reason 码 + SSID,定位是密码错还是路由拒绝
        ESP_UTILS_LOGW("STA disconnected: ssid='%s' reason=%d", (const char *)disc->ssid, disc->reason);
        if (s_state == WIFI_UI_CONNECTING) {
            // reason=201(AP 找不到)重连无意义需重扫;其余重试(认证超时/握手失败常为抖动)
            if (disc->reason != 201 && s_retry_count < WIFI_CONNECT_MAX_RETRY) {
                s_retry_count++;
                ESP_UTILS_LOGI("Retry %d/%d...", s_retry_count, WIFI_CONNECT_MAX_RETRY);
                esp_wifi_connect();
                return; // 保持 CONNECTING 状态,UI 不变
            }
            s_fail_reason = disc->reason;
            s_state = WIFI_UI_FAILED;
        } else {
            s_state = WIFI_UI_DISCONNECTED;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        // ip4_addrN 宏只访问 ->addr 字段,esp_ip4_addr_t 与 ip4_addr_t 布局一致可直接用
        snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u",
                 ip4_addr1(&evt->ip_info.ip), ip4_addr2(&evt->ip_info.ip),
                 ip4_addr3(&evt->ip_info.ip), ip4_addr4(&evt->ip_info.ip));
        s_state = WIFI_UI_CONNECTED;
        save_credentials_nvs(); // 凭证验证通过才持久化
    }
}

// 幂等初始化:init() 在每次系统启动安装 app 时调用,但 WiFi 栈只能建一次
static bool wifi_backend_init(void)
{
    if (s_wifi_stack_inited) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_UTILS_CHECK_FALSE_RETURN(nvs_flash_erase() == ESP_OK, false, "NVS erase failed");
        ESP_UTILS_CHECK_FALSE_RETURN(nvs_flash_init() == ESP_OK, false, "NVS init failed");
    }
    // 以下两个调用允许"已初始化"(BSP/其他 app 可能先做过),不算失败
    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();
    ESP_UTILS_CHECK_FALSE_RETURN(esp_netif_create_default_wifi_sta() != nullptr, false, "Create STA netif failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_UTILS_CHECK_FALSE_RETURN(esp_wifi_init(&cfg) == ESP_OK, false, "WiFi init failed");
    ESP_UTILS_CHECK_FALSE_RETURN(esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK, false, "Set STA mode failed");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));
    ESP_UTILS_CHECK_FALSE_RETURN(esp_wifi_start() == ESP_OK, false, "WiFi start failed");
    s_wifi_stack_inited = true;

    // 有保存过的凭证则开机自动重连(后台进行,不阻塞 UI)
    char ssid[33], pass[65];
    if (load_credentials_nvs(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strlcpy(s_conn_ssid, ssid, sizeof(s_conn_ssid));
        strlcpy(s_conn_pass, pass, sizeof(s_conn_pass));
        wifi_config_t wc = {};
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_connect();
        s_state = WIFI_UI_CONNECTING;
        ESP_UTILS_LOGI("Auto-connecting to saved AP: %s", ssid);
    }

    return true;
}

static void wifi_backend_scan(void)
{
    wifi_scan_config_t sc = {};
    s_state = WIFI_UI_SCANNING;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        s_state = WIFI_UI_IDLE;
    }
}

static void wifi_backend_connect(const char *ssid, const char *pass)
{
    strlcpy(s_conn_ssid, ssid, sizeof(s_conn_ssid));
    strlcpy(s_conn_pass, pass, sizeof(s_conn_pass));
    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // WPA3/PMF-required 路由需要 PMF 能力位;capable 而非 required,保持对纯 WPA2 AP 的兼容
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_fail_reason = 0;
    s_retry_count = 0;
    s_state = WIFI_UI_CONNECTING;
    // 返回码也透明化:password 只打长度,不打内容
    esp_err_t err = esp_wifi_connect();
    ESP_UTILS_LOGI("Connect to '%s' (pass len %d): %s", ssid, (int)strlen(pass), esp_err_to_name(err));
}

// ============================ UI ============================

static WifiConfigApp *s_instance = nullptr;

// UI 指针集中在 reset_ui_pointers() 清空:core 关闭 app 时会回收整个 screen,
// 这些指针会悬空,重开时 run() 会全部重建
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_scan_btn = nullptr;
static lv_obj_t *s_ap_list = nullptr;
static lv_obj_t *s_pw_panel = nullptr;   // 密码输入覆盖层
static lv_obj_t *s_pw_ssid_label = nullptr;
static lv_obj_t *s_pw_textarea = nullptr;
static lv_timer_t *s_poll_timer = nullptr;
static bool s_list_dirty = false;
static WifiUiState s_last_ui_state = WIFI_UI_IDLE;

static void reset_ui_pointers(void)
{
    s_status_label = nullptr;
    s_scan_btn = nullptr;
    s_ap_list = nullptr;
    s_pw_panel = nullptr;
    s_pw_ssid_label = nullptr;
    s_pw_textarea = nullptr;
    s_poll_timer = nullptr;
    s_list_dirty = false;
    s_last_ui_state = WIFI_UI_IDLE;
}

static const char *status_text(void)
{
    switch (s_state) {
    case WIFI_UI_SCANNING:  return "Scanning...";
    case WIFI_UI_CONNECTING: return "Connecting...";
    case WIFI_UI_CONNECTED:  return nullptr; // 动态拼接 SSID+IP
    case WIFI_UI_FAILED:     return "Connect failed. Check password and retry.";
    default:                 break;
    }
    return "Not connected. Tap Scan to find networks.";
}

static void update_status_label(void)
{
    if (s_status_label == nullptr) {
        return;
    }
    if (s_state == WIFI_UI_CONNECTED) {
        lv_label_set_text_fmt(s_status_label, "Connected\n%s\nIP: %s", s_conn_ssid, s_ip);
    } else if (s_state == WIFI_UI_FAILED) {
        // 失败必须带原因码,不搞泛泛的"连接失败"
        lv_label_set_text_fmt(s_status_label, "Connect failed (%d)\n%s", s_fail_reason, fail_reason_text(s_fail_reason));
    } else {
        const char *t = status_text();
        if (s_state == WIFI_UI_SCANNING && s_conn_ssid[0] != 0) {
            lv_label_set_text_fmt(s_status_label, "Scanning...\n(saved: %s)", s_conn_ssid);
        } else {
            lv_label_set_text(s_status_label, t);
        }
    }
    lv_obj_set_style_border_color(s_status_label,
        (s_state == WIFI_UI_CONNECTED) ? lv_color_hex(0x2e7d32) : lv_color_hex(0x444444), 0);
}

static void rebuild_ap_list(void)
{
    if (s_ap_list == nullptr) {
        return;
    }
    lv_obj_clean(s_ap_list);
    int n = s_ap_count;
    if (n == 0) {
        lv_obj_t *l = lv_list_add_button(s_ap_list, nullptr, "No networks found");
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN); // 占位文本不可点
        return;
    }
    char last_ssid[33] = "";
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)s_aps[i].ssid;
        // 同名 AP( mesh/中继)只显示信号最强的一个
        if (strcmp(ssid, last_ssid) == 0) {
            continue;
        }
        strlcpy(last_ssid, ssid, sizeof(last_ssid));
        bool open = s_aps[i].authmode == WIFI_AUTH_OPEN;
        lv_obj_t *btn = lv_list_add_button(s_ap_list, nullptr, "");
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0); // txt 非空时 label 是唯一子节点
        lv_label_set_text_fmt(lbl, "%s  %ddBm %s", ssid, s_aps[i].rssi, open ? "" : "*");
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(tgt);
            strlcpy(s_pending_ssid, (const char *)s_aps[idx].ssid, sizeof(s_pending_ssid));
            bool need_pw = s_aps[idx].authmode != WIFI_AUTH_OPEN;
            if (need_pw) {
                // 弹出密码覆盖层;键盘在 run() 里已绑定输入框,无需重复绑定
                lv_obj_clear_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_pw_ssid_label, s_pending_ssid);
                lv_textarea_set_text(s_pw_textarea, "");
            } else {
                wifi_backend_connect(s_pending_ssid, "");
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
}

static void poll_timer_cb(lv_timer_t *t)
{
    if (s_status_label == nullptr) {
        return;
    }
    if (s_state != s_last_ui_state) {
        update_status_label();
        lv_obj_add_state(s_scan_btn, s_state == WIFI_UI_SCANNING ? LV_STATE_DISABLED : LV_STATE_DEFAULT);
        if (s_state == WIFI_UI_FAILED && !lv_obj_has_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN)) {
            // 密码错误:不关对话框,清空重输
            lv_textarea_set_text(s_pw_textarea, "");
        }
        s_last_ui_state = s_state;
    }
    if (s_state == WIFI_UI_SCAN_DONE && s_list_dirty) {
        rebuild_ap_list();
        s_list_dirty = false;
        s_state = WIFI_UI_IDLE; // 列表已消费,回到待机
        s_last_ui_state = WIFI_UI_IDLE;
        update_status_label();
    }
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_ap_list != nullptr) {
        lv_obj_clean(s_ap_list);
        lv_list_add_button(s_ap_list, nullptr, "Scanning...");
    }
    s_list_dirty = true;
    wifi_backend_scan();
    update_status_label();
}

static void pw_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
}

static void pw_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *pass = lv_textarea_get_text(s_pw_textarea);
    if (pass[0] == 0) {
        return; // 空密码不允许发起
    }
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    wifi_backend_connect(s_pending_ssid, pass);
}

WifiConfigApp *WifiConfigApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (s_instance == nullptr) {
        s_instance = new WifiConfigApp(use_status_bar, use_navigation_bar);
    }
    return s_instance;
}

WifiConfigApp::WifiConfigApp(bool use_status_bar, bool use_navigation_bar):
    // use_default_screen=true:core 负责创建/加载/回收 screen,run() 直接在 lv_scr_act() 上建 UI
    App(APP_NAME, &esp_brookesia_app_icon_launcher_wifi_126_126, true, use_status_bar, use_navigation_bar)
{
}

WifiConfigApp::~WifiConfigApp()
{
}

bool WifiConfigApp::init(void)
{
    // 安装期(开机)初始化 WiFi 栈并自动重连保存过的网络;不碰任何 UI
    return wifi_backend_init();
}

bool WifiConfigApp::run(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x12161c), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 28, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8e8e8), 0);

    // 状态卡:边框颜色指示连接状态
    s_status_label = lv_label_create(scr);
    lv_obj_set_width(s_status_label, 340);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_status_label, 10, 0);
    lv_obj_set_style_border_width(s_status_label, 2, 0);
    lv_obj_set_style_border_color(s_status_label, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_status_label, 10, 0);
    update_status_label();

    s_scan_btn = lv_button_create(scr);
    lv_obj_set_size(s_scan_btn, 150, 54);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_lbl = lv_label_create(s_scan_btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_lbl);

    s_ap_list = lv_list_create(scr);
    lv_obj_set_size(s_ap_list, 370, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_ap_list, 0);
    lv_obj_set_style_max_height(s_ap_list, 230, 0);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x1c222b), 0);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(0x3a4250), 0);
    lv_list_add_button(s_ap_list, nullptr, "Tap Scan to list networks");

    // ---- 密码输入覆盖层(默认隐藏):SSID 标签 + 输入框 + 按钮 + 内置软键盘 ----
    s_pw_panel = lv_obj_create(scr);
    lv_obj_set_size(s_pw_panel, 420, 434);
    lv_obj_center(s_pw_panel);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_pw_panel, lv_color_hex(0x1c222b), 0);
    lv_obj_set_style_radius(s_pw_panel, 16, 0);
    lv_obj_set_style_border_color(s_pw_panel, lv_color_hex(0x5a89c4), 0);
    lv_obj_set_style_border_width(s_pw_panel, 2, 0);
    lv_obj_set_flex_flow(s_pw_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pw_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_pw_panel, 10, 0);
    lv_obj_clear_flag(s_pw_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_ssid_label = lv_label_create(s_pw_panel);
    lv_label_set_text(s_pw_ssid_label, "");
    lv_obj_set_style_text_font(s_pw_ssid_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_pw_ssid_label, lv_color_hex(0xe8e8e8), 0);

    // 输入行:密码框 + 可见性切换(密文模式下无法核对输入,是配网失败的头号元凶)
    lv_obj_t *ta_row = lv_obj_create(s_pw_panel);
    lv_obj_set_size(ta_row, 390, 60);
    lv_obj_set_flex_flow(ta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ta_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ta_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta_row, 0, 0);
    lv_obj_set_style_pad_all(ta_row, 0, 0);
    lv_obj_clear_flag(ta_row, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_textarea = lv_textarea_create(ta_row);
    lv_obj_set_width(s_pw_textarea, 300);
    lv_obj_set_height(s_pw_textarea, 56);
    lv_textarea_set_one_line(s_pw_textarea, true);
    lv_textarea_set_password_mode(s_pw_textarea, true);
    lv_textarea_set_placeholder_text(s_pw_textarea, "Password...");
    lv_obj_set_style_text_font(s_pw_textarea, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(s_pw_textarea, lv_color_hex(0x2a323e), 0);
    lv_obj_clear_flag(s_pw_textarea, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eye_btn = lv_button_create(ta_row);
    lv_obj_set_size(eye_btn, 52, 52);
    lv_obj_add_event_cb(eye_btn, [](lv_event_t *e) {
        lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
        bool hidden = lv_textarea_get_password_mode(s_pw_textarea);
        lv_textarea_set_password_mode(s_pw_textarea, !hidden);
        // 图标反映当前状态:密文=闭眼,明文=睁眼
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        lv_label_set_text(lbl, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *eye_lbl = lv_label_create(eye_btn);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(eye_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(eye_lbl);

    lv_obj_t *btn_row = lv_obj_create(s_pw_panel);
    lv_obj_set_size(btn_row, 390, 62);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btn_row);
    lv_obj_set_size(cancel, 150, 52);
    lv_obj_add_event_cb(cancel, pw_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *connect = lv_button_create(btn_row);
    lv_obj_set_size(connect, 170, 52);
    lv_obj_add_event_cb(connect, pw_connect_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *conn_lbl = lv_label_create(connect);
    lv_label_set_text(conn_lbl, LV_SYMBOL_OK " Connect");
    lv_obj_set_style_text_font(conn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(conn_lbl);

    // LVGL 内置软键盘:map 自带小写/大写/数字/符号四套布局,输入框获得焦点即弹字符
    lv_obj_t *kb = lv_keyboard_create(s_pw_panel);
    lv_obj_set_width(kb, 400);
    lv_obj_set_height(kb, 200);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, 0);
    lv_keyboard_set_textarea(kb, s_pw_textarea);

    // 轮询 WiFi 状态机快照;用 record 包裹让 core 在 app 关闭时自动回收 timer
    startRecordResource();
    s_poll_timer = lv_timer_create(poll_timer_cb, 250, nullptr);
    endRecordResource();

    return true;
}

bool WifiConfigApp::back(void)
{
    // 覆盖层打开时 Back 只关覆盖层;再次 Back 才退出 app
    if (s_pw_panel != nullptr && !lv_obj_has_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool WifiConfigApp::cleanResource(void)
{
    // screen 被 core 回收,这里只清指针避免悬空引用
    reset_ui_pointers();
    return true;
}

// 注册进 App::Registry:系统启动时 installAppFromRegistry 遍历,桌面自动出现图标
// 注意:宏内部用 ##PluginType## 拼接符号名,必须放在 namespace 内以裸类名调用
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(
    systems::base::App, WifiConfigApp, APP_NAME,
    []() {
        return std::shared_ptr<WifiConfigApp>(WifiConfigApp::requestInstance(), [](WifiConfigApp *p) {});
    }
)

} // namespace esp_brookesia::apps

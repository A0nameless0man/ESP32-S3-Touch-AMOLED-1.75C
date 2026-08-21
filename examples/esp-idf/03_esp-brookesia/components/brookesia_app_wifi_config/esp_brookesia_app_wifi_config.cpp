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
#include "circular_draw.hpp"

#include <string.h>
#include <math.h>
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

static void erase_credentials_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
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

static void wifi_backend_forget(void)
{
    erase_credentials_nvs();
    esp_wifi_disconnect();
    s_conn_ssid[0] = 0;
    s_conn_pass[0] = 0;
    s_ip[0] = 0;
    s_state = WIFI_UI_IDLE;
    ESP_UTILS_LOGI("Saved credentials erased");
}

// ============================ 圆屏几何 ============================
// 屏幕是 466x466 的圆:越出圆边的控件由 circular_draw 的 circle_clip
// 做圆弧裁切 + G1 相切圆角(挂 DRAW_MAIN,几何自适应),不再收窄行宽。

// ============================ UI ============================

static WifiConfigApp *s_instance = nullptr;

// UI 指针集中在 reset_ui_pointers() 清空:core 关闭 app 时会回收整个 screen,
// 这些指针会悬空,重开时 run() 会全部重建
static lv_obj_t *s_hub_visual = nullptr;    // 中央状态环(spinner/arc,状态切换时重建;本身可点击=Scan/Forget)
static lv_obj_t *s_hub_label = nullptr;     // 中央文字(SSID/IP/原因)
static lv_obj_t *s_list_overlay = nullptr;  // AP 列表覆盖层
static lv_obj_t *s_ap_container = nullptr;  // 列表行容器(可滚动)
static lv_obj_t *s_pw_overlay = nullptr;    // 密码输入覆盖层
static lv_obj_t *s_pw_ssid_label = nullptr;
static lv_obj_t *s_pw_textarea = nullptr;
static lv_timer_t *s_poll_timer = nullptr;
// 自绘键盘的字符模式:0 小写 1 大写 2 符号数字(底行 aA# 键循环)
static int s_kb_mode = 0;
static lv_obj_t *s_kb_container = nullptr;   // 自绘键盘行容器(模式切换时重建)
static WifiUiState s_last_ui_state_cache = WIFI_UI_IDLE; // poll 去重用

static void reset_ui_pointers(void)
{
    s_hub_visual = nullptr;
    s_hub_label = nullptr;
    s_list_overlay = nullptr;
    s_ap_container = nullptr;
    s_kb_container = nullptr;
    s_pw_overlay = nullptr;
    s_pw_ssid_label = nullptr;
    s_pw_textarea = nullptr;
    s_poll_timer = nullptr;
    s_kb_mode = 0;
    s_last_ui_state_cache = WIFI_UI_IDLE;
}

// ---- 配色(暗色,圆屏弱环境光下可读性优先) ----
#define COL_BG      0x12161c
#define COL_PANEL   0x1c222b
#define COL_KEY_BG  0x272e3a
#define COL_TEXT    0xe8e8e8
#define COL_TEXT_DIM 0x9aa3b0
#define COL_ACCENT  0x5a89c4
#define COL_GREEN   0x4caf50
#define COL_RED     0xcf6679
#define COL_GOOD    0x2e7d32

static uint32_t rssi_color(int rssi)
{
    if (rssi >= -60) {
        return COL_GREEN;
    }
    if (rssi >= -75) {
        return 0xd9a441;
    }
    return 0x8a5a44;
}

static void hub_update(void);

static void hub_circle_cb(lv_event_t *e)
{
    (void)e;
    // 圆环即主按钮:连接中不可打断;已连点一下=忘记,其余点一下=扫描
    if (s_state == WIFI_UI_SCANNING || s_state == WIFI_UI_CONNECTING) {
        return;
    }
    if (s_state == WIFI_UI_CONNECTED) {
        wifi_backend_forget();
    } else {
        wifi_backend_scan();
    }
    hub_update();
}

// ---- hub:中央状态环重建(每次状态切换) ----
static void hub_rebuild_visual(void)
{
    if (s_hub_visual != nullptr) {
        lv_obj_delete(s_hub_visual);
        s_hub_visual = nullptr;
    }
    if (s_hub_label == nullptr) {
        return;
    }
    lv_obj_t *parent = lv_obj_get_parent(s_hub_label);

    WifiUiState st = s_state;
    if (st == WIFI_UI_SCANNING || st == WIFI_UI_CONNECTING) {
        // 瞬态:spinner 无限旋转,色区分扫描/连接
        s_hub_visual = lv_spinner_create(parent);
        lv_obj_set_size(s_hub_visual, 190, 190);
        lv_spinner_set_anim_params(s_hub_visual, 1200, 90);
        lv_obj_set_style_arc_width(s_hub_visual, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_hub_visual, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_hub_visual, lv_color_hex(0x2a323e), LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_hub_visual,
            lv_color_hex((st == WIFI_UI_SCANNING) ? COL_ACCENT : 0xd9a441), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_hub_visual, true, LV_PART_INDICATOR);
    } else {
        // 静态:进度环表达完成度(空闲 0/失败 30/已连 100)
        s_hub_visual = lv_arc_create(parent);
        lv_obj_set_size(s_hub_visual, 190, 190);
        lv_arc_set_range(s_hub_visual, 0, 100);
        lv_arc_set_bg_angles(s_hub_visual, 0, 360);
        lv_arc_set_mode(s_hub_visual, LV_ARC_MODE_NORMAL);
        lv_obj_remove_style(s_hub_visual, nullptr, LV_PART_KNOB);
        // 不清 CLICKABLE:圆环就是 Scan/Forget 主按钮,点击回调挂在下面统一处理
        lv_obj_set_style_arc_width(s_hub_visual, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_hub_visual, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_hub_visual, lv_color_hex(0x2a323e), LV_PART_MAIN);
        int val = 0;
        uint32_t col = COL_TEXT_DIM;
        if (st == WIFI_UI_CONNECTED) {
            val = 100;
            col = COL_GREEN;
        } else if (st == WIFI_UI_FAILED) {
            val = 30;
            col = COL_RED;
        }
        lv_arc_set_value(s_hub_visual, val);
        lv_obj_set_style_arc_color(s_hub_visual, lv_color_hex(col), LV_PART_INDICATOR);
    }
    // 圆环本身可点(代替被移除的底部按钮);压到最底层,免得重建后盖住列表/键盘覆盖层
    lv_obj_add_event_cb(s_hub_visual, hub_circle_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_to_index(s_hub_visual, 0);
    lv_obj_center(s_hub_visual);
}

static void hub_update_text(void)
{
    if (s_hub_label == nullptr) {
        return;
    }
    switch (s_state) {
    case WIFI_UI_SCANNING:
        lv_label_set_text(s_hub_label, "Scanning...");
        break;
    case WIFI_UI_CONNECTING:
        lv_label_set_text_fmt(s_hub_label, "Connecting\n%s", s_conn_ssid);
        break;
    case WIFI_UI_CONNECTED:
        lv_label_set_text_fmt(s_hub_label, LV_SYMBOL_WIFI " %s\n%s\nTap to forget", s_conn_ssid, s_ip);
        break;
    case WIFI_UI_FAILED:
        lv_label_set_text_fmt(s_hub_label, "Failed (%d)\n%s", s_fail_reason, fail_reason_text(s_fail_reason));
        break;
    case WIFI_UI_DISCONNECTED:
    default:
        lv_label_set_text(s_hub_label, LV_SYMBOL_WIFI "\nTap to scan");
        break;
    }
}

static void hub_update(void)
{
    hub_rebuild_visual();
    hub_update_text();
}

// ---- AP 列表 ----
static void ap_row_click_cb(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(tgt);
    strlcpy(s_pending_ssid, (const char *)s_aps[idx].ssid, sizeof(s_pending_ssid));
    if (s_aps[idx].authmode == WIFI_AUTH_OPEN) {
        // 开放网络直接连,不弹键盘
        lv_obj_add_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
        wifi_backend_connect(s_pending_ssid, "");
        return;
    }
    lv_obj_add_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pw_ssid_label, s_pending_ssid);
    lv_textarea_set_text(s_pw_textarea, "");
}

static void rebuild_ap_list(void)
{
    if (s_ap_container == nullptr) {
        return;
    }
    lv_obj_clean(s_ap_container);
    int n = s_ap_count;
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_ap_container);
        lv_label_set_text(l, "No networks found");
        lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    char last_ssid[33] = "";
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)s_aps[i].ssid;
        // 同名 AP(mesh/中继)只显示信号最强的一个
        if (strcmp(ssid, last_ssid) == 0) {
            continue;
        }
        strlcpy(last_ssid, ssid, sizeof(last_ssid));
        // 行内三段:信号图标(按强度着色) / SSID(长名截断) / dBm + 锁
        lv_obj_t *row = lv_button_create(s_ap_container);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, ap_row_click_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_PANEL), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 26, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 12, 0);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(rssi_color(s_aps[i].rssi)), 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, ssid);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_pad_hor(name, 8, 0);

        lv_obj_t *sig = lv_label_create(row);
        lv_label_set_text_fmt(sig, "%d %s", s_aps[i].rssi,
            (s_aps[i].authmode == WIFI_AUTH_OPEN) ? "" : LV_SYMBOL_EYE_CLOSE);
        lv_obj_set_style_text_font(sig, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sig, lv_color_hex(COL_TEXT_DIM), 0);
    }
}

// ---- 自绘键盘(圆屏四行,每行按弦收窄) ----

static void kb_key_style(lv_obj_t *btn, int w, int h)
{
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_KEY_BG), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(COL_TEXT), 0);
}

static lv_obj_t *kb_key(lv_obj_t *row, const char *txt, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    kb_key_style(btn, w, h);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    return btn;
}

static void kb_char_cb(lv_event_t *e)
{
    if (s_pw_textarea == nullptr) {
        return;
    }
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    lv_textarea_add_char(s_pw_textarea, lv_label_get_text(l)[0]);
}

static void kb_backspace_cb(lv_event_t *e)
{
    (void)e;
    if (s_pw_textarea != nullptr) {
        lv_textarea_delete_char(s_pw_textarea);
    }
}

static void kb_space_cb(lv_event_t *e)
{
    (void)e;
    if (s_pw_textarea != nullptr) {
        lv_textarea_add_char(s_pw_textarea, ' ');
    }
}

static void kb_ok_cb(lv_event_t *e);
static void kb_rebuild(lv_obj_t *container);

static void kb_mode_cb(lv_event_t *e)
{
    (void)e;
    s_kb_mode = (s_kb_mode + 1) % 3; // ab -> AB -> 12 -> ab
    if (s_kb_container != nullptr) {
        kb_rebuild(s_kb_container);
    }
}

static void kb_rebuild(lv_obj_t *container)
{
    lv_obj_clean(container);
    // 键盘两侧贴屏边的键挂圆弧切角:运行时按实际坐标,只有越出圆边的角才画,
    // 因此两侧键可以无差别挂裁(小屏/布局变化自动适应,也支持一键多角)
    // 三套字符排布;符号排布下第二行含 ':' 等,行宽同字母行
    static const char *rows_lower[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char *rows_upper[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const char *rows_sym[2]   = {"1234567890", "-/:;()&@\""};
    const char **letters = (s_kb_mode == 0) ? rows_lower : (s_kb_mode == 1) ? rows_upper : rows_sym;
    int letter_rows = (s_kb_mode == 2) ? 2 : 3;

    for (int r = 0; r < letter_rows; r++) {
        lv_obj_t *row = lv_obj_create(container);
        lv_obj_set_size(row, 400, 50);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_column(row, 4, 0);
        // 行级圆弧裁剪:行端越出圆边的键角由环带弧切+交叉圆角处理,
        // 弦宽足够的行自动零绘制 —— 键盘得以保持自然满宽
        circdraw::attach_circle_clip(row, 12, COL_BG, circdraw::screen_circle(4));
        int cnt = (int)strlen(letters[r]);
        // 行宽预算:字母行按位数定,弦收窄在 fit 时统一裁
        for (int i = 0; i < cnt; i++) {
            char t[2] = {letters[r][i], 0};
            kb_key(row, t, 36, 46, kb_char_cb);
        }
    }
    // 符号模式的第三行(逗号问号叹号引号)与功能行合并思路不同,单独补一行
    if (s_kb_mode == 2) {
        lv_obj_t *row = lv_obj_create(container);
        lv_obj_set_size(row, 400, 50);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_column(row, 4, 0);
        circdraw::attach_circle_clip(row, 12, COL_BG, circdraw::screen_circle(4));
        static const char *sym3 = ",?!'.";
        for (const char *p = sym3; *p; p++) {
            char t[2] = {p[0], 0};
            kb_key(row, t, 36, 46, kb_char_cb);
        }
        kb_key(row, LV_SYMBOL_BACKSPACE, 48, 46, kb_backspace_cb);
    }
    // 功能行:模式循环 / 空格 / 退格(字母模式) / OK
    lv_obj_t *row = lv_obj_create(container);
    lv_obj_set_size(row, 320, 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(row, 6, 0);
    circdraw::attach_circle_clip(row, 12, COL_BG, circdraw::screen_circle(4));

    lv_obj_t *mode_key = kb_key(row, (s_kb_mode == 0) ? "aA" : (s_kb_mode == 1) ? "12" : "ab", 52, 48, kb_mode_cb);
    lv_obj_t *mode_lbl = lv_obj_get_child(mode_key, 0);
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *space = kb_key(row, "_", 100, 48, kb_space_cb); // 下划线示意空格键
    lv_obj_t *space_lbl = lv_obj_get_child(space, 0);
    lv_obj_set_style_text_color(space_lbl, lv_color_hex(COL_TEXT_DIM), 0);

    if (s_kb_mode != 2) {
        kb_key(row, LV_SYMBOL_BACKSPACE, 52, 48, kb_backspace_cb);
    }
    lv_obj_t *ok = kb_key(row, LV_SYMBOL_OK, 60, 48, kb_ok_cb);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_GOOD), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_GREEN), LV_STATE_PRESSED);
}

// 每行按实际纵坐标收进可见弦内(键盘是自绘的,这里集中处理)
static void kb_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_pw_textarea == nullptr) {
        return;
    }
    const char *pass = lv_textarea_get_text(s_pw_textarea);
    if (pass[0] == 0) {
        return; // 空密码不允许发起
    }
    lv_obj_add_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN);
    wifi_backend_connect(s_pending_ssid, pass);
}

static void pw_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void pw_eye_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    bool hidden = lv_textarea_get_password_mode(s_pw_textarea);
    lv_textarea_set_password_mode(s_pw_textarea, !hidden);
    // 图标反映当前状态:密文=闭眼,明文=睁眼
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    lv_label_set_text(lbl, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void scan_from_overlay_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
    wifi_backend_scan();
}

// ---- 轮询:消费 WiFi 状态机快照,驱动三个场景 ----
static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hub_label == nullptr) {
        return;
    }
    WifiUiState st = s_state;
    if (st != s_last_ui_state_cache) {
        hub_update();
        s_last_ui_state_cache = st;
    }
    if (st == WIFI_UI_SCAN_DONE) {
        rebuild_ap_list();
        s_state = WIFI_UI_IDLE;
        s_last_ui_state_cache = WIFI_UI_IDLE;
        hub_update();
        // 扫描完直接展示列表,省一步点击
        if (s_list_overlay != nullptr) {
            lv_obj_clear_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);

    // ---- hub:状态环 + 中央文字;圆环本身就是主按钮 ----
    // 布局全部居中放置,越出圆边的控件由 circle_clip 几何自适应处理
    s_hub_label = lv_label_create(scr);
    lv_label_set_text(s_hub_label, "");
    lv_obj_set_style_text_font(s_hub_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hub_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(s_hub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hub_label, 300);
    lv_label_set_long_mode(s_hub_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_hub_label, LV_ALIGN_CENTER, 0, 0);
    // 主按钮就是中央圆环本身(hub_rebuild_visual 里挂回调);不再另设底部按钮

    // ---- AP 列表覆盖层 ----
    s_list_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_list_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s_list_overlay);
    lv_obj_add_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_list_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_list_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list_overlay, 0, 0);
    lv_obj_set_style_radius(s_list_overlay, 0, 0);
    lv_obj_set_flex_flow(s_list_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // pad_top=56 避开顶部系统状态栏;行内底部留白见 container 的 pad_bottom
    lv_obj_set_style_pad_all(s_list_overlay, 6, 0);
    lv_obj_set_style_pad_top(s_list_overlay, 106, 0);

    lv_obj_t *list_hdr = lv_obj_create(s_list_overlay);
    // 容器比按钮高 6px:按钮底部描边/抗锯齿像素不再被父容器裁掉
    lv_obj_set_size(list_hdr, 300, 50);
    lv_obj_add_flag(list_hdr, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(list_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(list_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_hdr, 0, 0);
    lv_obj_set_style_pad_all(list_hdr, 0, 0);
    lv_obj_clear_flag(list_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list_title = lv_label_create(list_hdr);
    lv_label_set_text(list_title, "Networks");
    lv_obj_set_style_text_font(list_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *rescan_btn = lv_button_create(list_hdr);
    lv_obj_set_size(rescan_btn, 44, 44);
    lv_obj_set_style_radius(rescan_btn, 22, 0);
    lv_obj_set_style_bg_color(rescan_btn, lv_color_hex(COL_KEY_BG), 0);
    lv_obj_set_style_bg_color(rescan_btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(rescan_btn, scan_from_overlay_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rescan_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(rescan_lbl);

    s_ap_container = lv_obj_create(s_list_overlay);
    lv_obj_set_width(s_ap_container, 288); // <= 内切于任何可见弦,行永远完整可点
    lv_obj_set_flex_grow(s_ap_container, 1);
    lv_obj_set_flex_flow(s_ap_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ap_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_ap_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ap_container, 0, 0);
    lv_obj_set_style_pad_all(s_ap_container, 0, 0);
    lv_obj_set_style_pad_row(s_ap_container, 8, 0);
    // 底部滚动留白:最后行只能滚到 y≈366(该处弦宽 382),滚到屏底弦宽只剩 143 会把行裁成月牙
    lv_obj_set_style_pad_bottom(s_ap_container, 44, 0);
    lv_obj_t *ph = lv_label_create(s_ap_container);
    lv_label_set_text(ph, "Scanning...");

    // ---- 密码输入覆盖层:SSID 行 / 密码行 / 自绘圆屏键盘 ----
    s_pw_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_pw_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s_pw_overlay);
    lv_obj_add_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_pw_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_pw_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pw_overlay, 0, 0);
    lv_obj_set_style_radius(s_pw_overlay, 0, 0);
    lv_obj_set_flex_flow(s_pw_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pw_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // pad_top=56 避开顶部系统状态栏(SSID 行原在 y≈4 被状态栏裁住)
    lv_obj_set_style_pad_all(s_pw_overlay, 4, 0);
    lv_obj_set_style_pad_top(s_pw_overlay, 106, 0);
    lv_obj_clear_flag(s_pw_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ssid_row = lv_obj_create(s_pw_overlay);
    lv_obj_set_size(ssid_row, 300, 46); // 比 close_btn(40) 高 6px,底边像素不被容器裁
    lv_obj_add_flag(ssid_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ssid_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ssid_row, 0, 0);
    lv_obj_set_style_pad_all(ssid_row, 0, 0);
    lv_obj_clear_flag(ssid_row, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_ssid_label = lv_label_create(ssid_row);
    lv_label_set_text(s_pw_ssid_label, "");
    lv_obj_set_flex_grow(s_pw_ssid_label, 1);
    lv_label_set_long_mode(s_pw_ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_pw_ssid_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_pw_ssid_label, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *close_btn = lv_button_create(ssid_row);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_set_style_radius(close_btn, 20, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COL_KEY_BG), 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(close_btn, pw_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    lv_obj_t *ta_row = lv_obj_create(s_pw_overlay);
    lv_obj_set_size(ta_row, 300, 48);
    lv_obj_set_flex_flow(ta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ta_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta_row, 0, 0);
    lv_obj_set_style_pad_all(ta_row, 0, 0);
    lv_obj_clear_flag(ta_row, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_textarea = lv_textarea_create(ta_row);
    lv_obj_set_flex_grow(s_pw_textarea, 1);
    lv_obj_set_height(s_pw_textarea, 44);
    lv_textarea_set_one_line(s_pw_textarea, true);
    lv_textarea_set_password_mode(s_pw_textarea, true);
    lv_textarea_set_placeholder_text(s_pw_textarea, "Password");
    lv_obj_set_style_text_font(s_pw_textarea, &lv_font_montserrat_18, 0);
    lv_obj_set_style_bg_color(s_pw_textarea, lv_color_hex(COL_KEY_BG), 0);
    lv_obj_set_style_border_color(s_pw_textarea, lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *eye_btn = lv_button_create(ta_row);
    lv_obj_set_size(eye_btn, 44, 44);
    lv_obj_set_style_radius(eye_btn, 12, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COL_KEY_BG), 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(eye_btn, pw_eye_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *eye_lbl = lv_label_create(eye_btn);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(eye_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(eye_lbl);

    lv_obj_t *kb = lv_obj_create(s_pw_overlay);
    lv_obj_set_width(kb, lv_pct(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_set_flex_flow(kb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(kb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_row(kb, 6, 0);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    s_kb_container = kb;
    kb_rebuild(kb);

    hub_update();

    // 轮询 WiFi 状态机快照;用 record 包裹让 core 在 app 关闭时自动回收 timer
    startRecordResource();
    s_poll_timer = lv_timer_create(poll_timer_cb, 250, nullptr);
    endRecordResource();

    // 打开即扫:待机/失败状态下免点 Scan 一步直达列表
    if (s_state == WIFI_UI_IDLE || s_state == WIFI_UI_DISCONNECTED || s_state == WIFI_UI_FAILED) {
        wifi_backend_scan();
        hub_update();
    }

    return true;
}

bool WifiConfigApp::back(void)
{
    // 覆盖层打开时 Back 逐层关;全部关掉再退出 app
    if (s_pw_overlay != nullptr && !lv_obj_has_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_pw_overlay, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    if (s_list_overlay != nullptr && !lv_obj_has_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_list_overlay, LV_OBJ_FLAG_HIDDEN);
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

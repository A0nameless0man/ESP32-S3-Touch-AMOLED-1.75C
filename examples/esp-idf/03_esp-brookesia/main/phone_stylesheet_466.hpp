/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 圆屏(466x466)专属 phone stylesheet —— 自有文件,不进 vendored 目录。
 * 基于 brookesia 480_480 Dark 表改造,begin() 前 addStylesheet 即按精确
 * 分辨率自动激活(Phone::begin 只在无表时才补 default)。
 *
 * 圆屏适配差异:
 * - app launcher icon 170 -> 每页恰好 2x2(table 360 高:ver=360/170=2),
 *   行列 pad 由 launcher 均匀分布自动居中
 * - navigation bar 300x52 胶囊短条(配合 navigation_bar.cpp 的 radius/上浮
 *   patch),替代全宽直条 —— 圆屏底部全宽条两端会被圆边吃掉
 */
#pragma once

#include "systems/phone/esp_brookesia_phone.hpp"
#include "systems/phone/assets/esp_brookesia_phone_assets.h"

namespace esp_brookesia::systems::phone {

// ---- core(壁纸/字体表复用 480 资源;default_fonts 必须给全:StyleFont::SIZE()
// 校验时查 getFontBySize 的 map,缺表会导致 addStylesheet 直接失败) ----
constexpr base::Display::Data STYLESHEET_466_466_DARK_CORE_DISPLAY_DATA = {
    .background = {
        .color = gui::StyleColor::COLOR(0x1A1A1A),
        .wallpaper_image_resource = gui::StyleImage::IMAGE(&esp_brookesia_image_middle_wallpaper_dark_480_480),
    },
    .text = {
        .default_fonts_num = 21,
        .default_fonts = {
            gui::StyleFont::CUSTOM_SIZE(8, &esp_brookesia_font_maison_neue_book_8),
            gui::StyleFont::CUSTOM_SIZE(10, &esp_brookesia_font_maison_neue_book_10),
            gui::StyleFont::CUSTOM_SIZE(12, &esp_brookesia_font_maison_neue_book_12),
            gui::StyleFont::CUSTOM_SIZE(14, &esp_brookesia_font_maison_neue_book_14),
            gui::StyleFont::CUSTOM_SIZE(16, &esp_brookesia_font_maison_neue_book_16),
            gui::StyleFont::CUSTOM_SIZE(18, &esp_brookesia_font_maison_neue_book_18),
            gui::StyleFont::CUSTOM_SIZE(20, &esp_brookesia_font_maison_neue_book_20),
            gui::StyleFont::CUSTOM_SIZE(22, &esp_brookesia_font_maison_neue_book_22),
            gui::StyleFont::CUSTOM_SIZE(24, &esp_brookesia_font_maison_neue_book_24),
            gui::StyleFont::CUSTOM_SIZE(26, &esp_brookesia_font_maison_neue_book_26),
            gui::StyleFont::CUSTOM_SIZE(28, &esp_brookesia_font_maison_neue_book_28),
            gui::StyleFont::CUSTOM_SIZE(30, &esp_brookesia_font_maison_neue_book_30),
            gui::StyleFont::CUSTOM_SIZE(32, &esp_brookesia_font_maison_neue_book_32),
            gui::StyleFont::CUSTOM_SIZE(34, &esp_brookesia_font_maison_neue_book_34),
            gui::StyleFont::CUSTOM_SIZE(36, &esp_brookesia_font_maison_neue_book_36),
            gui::StyleFont::CUSTOM_SIZE(38, &esp_brookesia_font_maison_neue_book_38),
            gui::StyleFont::CUSTOM_SIZE(40, &esp_brookesia_font_maison_neue_book_40),
            gui::StyleFont::CUSTOM_SIZE(42, &esp_brookesia_font_maison_neue_book_42),
            gui::StyleFont::CUSTOM_SIZE(44, &esp_brookesia_font_maison_neue_book_44),
            gui::StyleFont::CUSTOM_SIZE(46, &esp_brookesia_font_maison_neue_book_46),
            gui::StyleFont::CUSTOM_SIZE(48, &esp_brookesia_font_maison_neue_book_48),
        },
    },
    .container = {
        .styles = {
            { .outline_width = 1, .outline_color = gui::StyleColor::COLOR(0xeb3b5a), },
            { .outline_width = 2, .outline_color = gui::StyleColor::COLOR(0xfa8231), },
            { .outline_width = 3, .outline_color = gui::StyleColor::COLOR(0xf7b731), },
            { .outline_width = 2, .outline_color = gui::StyleColor::COLOR(0x20bf6b), },
            { .outline_width = 1, .outline_color = gui::StyleColor::COLOR(0x0fb9b1), },
            { .outline_width = 3, .outline_color = gui::StyleColor::COLOR(0x2d98da), },
        },
    },
};

constexpr base::Manager::Data STYLESHEET_466_466_DARK_CORE_MANAGER_DATA = {
    .app = {
        .max_running_num = 3,
    },
    .flags = {
        .enable_app_save_snapshot = 1,
    },
};

constexpr base::Context::Data STYLESHEET_466_466_DARK_CORE_DATA = {
    .name = "466x466 Dark Round",
    .screen_size = gui::StyleSize::RECT(466, 466),
    .display = STYLESHEET_466_466_DARK_CORE_DISPLAY_DATA,
    .manager = STYLESHEET_466_466_DARK_CORE_MANAGER_DATA,
};

// ---- status bar(同 480) ----
constexpr StatusBar::AreaData STYLESHEET_466_466_DARK_STATUS_BAR_AREA_DATA(int w_percent, StatusBar::AreaAlign align)
{
    return {
        .size = gui::StyleSize::RECT_PERCENT(w_percent, 100),
        .layout_column_align = align,
        .layout_column_start_offset = 26,
        .layout_column_pad = 4,
    };
}

constexpr StatusBar::Data STYLESHEET_466_466_DARK_STATUS_BAR_DATA = {
    .main = {
        .size = gui::StyleSize::RECT_W_PERCENT(100, 40),
        .background_color = gui::StyleColor::COLOR(0x38393A),
        .text_font = gui::StyleFont::SIZE(18),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .area = {
        .num = 2,
        .data = {
            STYLESHEET_466_466_DARK_STATUS_BAR_AREA_DATA(50, StatusBar::AreaAlign::START),
            STYLESHEET_466_466_DARK_STATUS_BAR_AREA_DATA(50, StatusBar::AreaAlign::END),
        },
    },
    .icon_common_size = gui::StyleSize::SQUARE(24),
    .battery = {
        .area_index = 1,
        .icon_data = {
            .icon = {
                .image_num = 5,
                .images = {
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level1_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level2_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level3_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_level4_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_battery_charge_24_24),
                },
            },
        },
    },
    .wifi = {
        .area_index = 1,
        .icon_data = {
            .icon = {
                .image_num = 4,
                .images = {
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_close_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level1_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level2_24_24),
                    gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_status_bar_wifi_level3_24_24),
                },
            },
        },
    },
    .clock = {
        .area_index = 0,
    },
    .flags = {
        .enable_battery_icon = 1,
        .enable_battery_icon_common_size = 1,
        .enable_battery_label = 1,
        .enable_wifi_icon = 1,
        .enable_wifi_icon_common_size = 1,
        .enable_clock = 1,
    },
};

// ---- app launcher:icon 170 -> 2x2 居中网格 ----
constexpr AppLauncherIcon::Data STYLESHEET_466_466_DARK_APP_LAUNCHER_ICON_DATA = {
    .main = {
        .size = gui::StyleSize::SQUARE(170),
        .layout_row_pad = 10,
    },
    .image = {
        .default_size = gui::StyleSize::SQUARE(112),
        .press_size = gui::StyleSize::SQUARE(100),
    },
    .label = {
        .text_font = gui::StyleFont::SIZE(22),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    }
};

constexpr AppLauncherData STYLESHEET_466_466_DARK_APP_LAUNCHER_DATA = {
    .main = {
        .y_start = 0,
        .size = gui::StyleSize::RECT_PERCENT(100, 100),
    },
    .table = {
        .default_num = 3,
        .size = gui::StyleSize::RECT_W_PERCENT(100, 360),
    },
    .indicator = {
        .main_size = gui::StyleSize::RECT_W_PERCENT(100, 80),
        .main_layout_column_pad = 10,
        // 页面指示点上浮让位弧形 nav dock(dock 占 y394-456)
        .main_layout_bottom_offset = 70,
        .spot_inactive_size = gui::StyleSize::SQUARE(12),
        .spot_active_size = gui::StyleSize::RECT(30, 12),
        .spot_inactive_background_color = gui::StyleColor::COLOR(0xC6C6C6),
        .spot_active_background_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .icon = STYLESHEET_466_466_DARK_APP_LAUNCHER_ICON_DATA,
    .flags = {
        .enable_table_scroll_anim = 0,
    },
};

// ---- navigation bar:弧形 dock —— [CIRCLE-UI] vendored patch 按 466 圆屏画弧带,
// main.size 是弧带包围盒容器(466 宽 × 110 高,底部对齐),背景色即弧带色 ----
constexpr NavigationBar::Data STYLESHEET_466_466_DARK_NAVIGATION_BAR_DATA = {
    .main = {
        .size = gui::StyleSize::RECT(466, 110),
        .background_color = gui::StyleColor::COLOR(0x38393A),
    },
    .button = {
        .icon_size = gui::StyleSize::SQUARE(32),
        .icon_images = {
            gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_navigation_bar_back_32_32),
            gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_navigation_bar_home_32_32),
            gui::StyleImage::IMAGE_RECOLOR_WHITE(&esp_brookesia_image_middle_navigation_bar_recents_screen_32_32),
        },
        .navigate_types = {
            base::Manager::NavigateType::BACK,
            base::Manager::NavigateType::HOME,
            base::Manager::NavigateType::RECENTS_SCREEN,
        },
        .active_background_color = gui::StyleColor::COLOR_WITH_OPACITY(0xFFFFFF, LV_OPA_50),
    },
    .visual_flex = {
        .show_animation = {
            .duration_ms = 200,
            .path_type = gui::StyleAnimation::ANIM_PATH_TYPE_EASE_OUT,
        },
        .hide_animation = {
            .duration_ms = 200,
            .path_type = gui::StyleAnimation::ANIM_PATH_TYPE_EASE_IN,
        },
        .hide_timer_period_ms = 2000,
    },
    .flags = {
        .enable_main_size_min = 0,
        .enable_main_size_max = 0,
    },
};

// ---- gesture(同 480) ----
constexpr Gesture::IndicatorBarData STYLESHEET_466_466_DARK_GESTURE_LEFT_RIGHT_INDICATOR_BAR_DATA = {
    .main = {
        .size_min = gui::StyleSize::RECT(10, 0),
        .size_max = gui::StyleSize::RECT_H_PERCENT(10, 50),
        .radius = 5,
        .layout_pad_all = 2,
        .color = gui::StyleColor::COLOR(0x000000),
    },
    .indicator = {
        .radius = 5,
        .color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .animation = {
        .scale_back_path_type = gui::StyleAnimation::ANIM_PATH_TYPE_BOUNCE,
        .scale_back_time_ms = 500,
    },
};

constexpr Gesture::IndicatorBarData STYLESHEET_466_466_DARK_GESTURE_BOTTOM_INDICATOR_BAR_DATA = {
    .main = {
        // [CIRCLE-UI] 52% 宽:对象宽度是弧长状态,盒子需盖住弧端圆头的包围盒
        .size_min = gui::StyleSize::RECT(0, 10),
        .size_max = gui::StyleSize::RECT_W_PERCENT(52, 10),
        .radius = 5,
        .layout_pad_all = 2,
        .color = gui::StyleColor::COLOR(0x1A1A1A),
    },
    .indicator = {
        .radius = 5,
        .color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .animation = {
        .scale_back_path_type = gui::StyleAnimation::ANIM_PATH_TYPE_BOUNCE,
        .scale_back_time_ms = 500,
    },
};

constexpr Gesture::Data STYLESHEET_466_466_DARK_GESTURE_DATA = {
    .detect_period_ms = 20,
    .threshold = {
        .direction_vertical = 50,
        .direction_horizon = 50,
        .direction_angle = 60,
        .horizontal_edge = 10,
        .vertical_edge = 20,
        .duration_short_ms = 800,
        .speed_slow_px_per_ms = 0.1,
    },
    .indicator_bars = {
        [static_cast<int>(Gesture::IndicatorBarType::LEFT)] =
            STYLESHEET_466_466_DARK_GESTURE_LEFT_RIGHT_INDICATOR_BAR_DATA,
        [static_cast<int>(Gesture::IndicatorBarType::RIGHT)] =
            STYLESHEET_466_466_DARK_GESTURE_LEFT_RIGHT_INDICATOR_BAR_DATA,
        [static_cast<int>(Gesture::IndicatorBarType::BOTTOM)] =
            STYLESHEET_466_466_DARK_GESTURE_BOTTOM_INDICATOR_BAR_DATA,
    },
    .flags = {
        .enable_indicator_bars = {
            [static_cast<int>(Gesture::IndicatorBarType::LEFT)] = 0,
            [static_cast<int>(Gesture::IndicatorBarType::RIGHT)] = 0,
            [static_cast<int>(Gesture::IndicatorBarType::BOTTOM)] = 1,
        },
    },
};

// ---- recents screen:快照高度适配 466 屏(flex 扣状态栏后可用高度 338,
// 480 表的 352 在此屏会被 calibrateCoreObjectSize 拒绝) ----
constexpr RecentsScreenSnapshot::Data STYLESHEET_466_466_DARK_RECENTS_SCREEN_SNAPSHOT_DATA = {
    .main_size = gui::StyleSize::RECT(300, 330),
    .title = {
        .main_size = gui::StyleSize::RECT(300, 52),
        .main_layout_column_pad = 10,
        .icon_size = gui::StyleSize::SQUARE(36),
        .text_font = gui::StyleFont::SIZE(22),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .image = {
        .main_size = gui::StyleSize::RECT(300, 270),
        .radius = 20,
    },
};

constexpr RecentsScreen::Data STYLESHEET_466_466_DARK_RECENTS_SCREEN_DATA = {
    .main = {
        .size = gui::StyleSize::RECT_PERCENT(100, 100),
        .layout_row_pad = 10,
        .layout_top_pad = 0,
        .layout_bottom_pad = 20,
        .background_color = gui::StyleColor::COLOR(0x1A1A1A),
    },
    .memory = {
        .main_size = gui::StyleSize::RECT_W_PERCENT(100, 20),
        .main_layout_x_right_offset = 10,
        .label_text_font = gui::StyleFont::SIZE(16),
        .label_text_color = gui::StyleColor::COLOR(0xFFFFFF),
        .label_unit_text = "KB",
    },
    .snapshot_table = {
        .main_size = gui::StyleSize::RECT_PERCENT(100, 100),
        .main_layout_column_pad = 40,
        .snapshot = STYLESHEET_466_466_DARK_RECENTS_SCREEN_SNAPSHOT_DATA,
    },
    .trash_icon = {
        .default_size = gui::StyleSize::SQUARE(48),
        .press_size = gui::StyleSize::SQUARE(43),
        .image = gui::StyleImage::IMAGE(&esp_brookesia_image_middle_recents_screen_trash_48_48),
    },
    .flags = {
        .enable_memory = 1,
        .enable_table_height_flex = 1,
        .enable_table_snapshot_use_icon_image = 0,
    },
};

// ---- display 汇总(nav 沿用 HIDE,行为与 default 表一致) ----
constexpr Display::Data STYLESHEET_466_466_DARK_DISPLAY_DATA = {
    .status_bar = {
        .data = STYLESHEET_466_466_DARK_STATUS_BAR_DATA,
        .visual_mode = StatusBar::VisualMode::SHOW_FIXED,
    },
    .navigation_bar = {
        .data = STYLESHEET_466_466_DARK_NAVIGATION_BAR_DATA,
        .visual_mode = NavigationBar::VisualMode::HIDE,
    },
    .app_launcher = {
        .data = STYLESHEET_466_466_DARK_APP_LAUNCHER_DATA,
        .default_image = gui::StyleImage::IMAGE(&esp_brookesia_image_middle_app_launcher_default_112_112),
    },
    .recents_screen = {
        .data = STYLESHEET_466_466_DARK_RECENTS_SCREEN_DATA,
        .status_bar_visual_mode = StatusBar::VisualMode::HIDE,
        .navigation_bar_visual_mode = NavigationBar::VisualMode::HIDE,
    },
    .flags = {
        .enable_status_bar = 1,
        .enable_navigation_bar = 1,
        .enable_app_launcher_flex_size = 1,
        .enable_recents_screen = 1,
        .enable_recents_screen_flex_size = 1,
    },
};

constexpr Manager::Data STYLESHEET_466_466_DARK_MANAGER_DATA = {
    .gesture = STYLESHEET_466_466_DARK_GESTURE_DATA,
    .gesture_mask_indicator_trigger_time_ms = 0,
    .recents_screen = {
        .drag_snapshot_y_step = 10,
        .drag_snapshot_y_threshold = 50,
        .drag_snapshot_angle_threshold = 60,
        .delete_snapshot_y_threshold = 50,
    },
    .flags = {
        .enable_gesture = 1,
        .enable_gesture_navigation_back = 0,
        .enable_recents_screen_snapshot_drag = 1,
        .enable_recents_screen_hide_when_no_snapshot = 1,
    },
};

constexpr Stylesheet STYLESHEET_466_466_DARK = {
    .core = STYLESHEET_466_466_DARK_CORE_DATA,
    .display = STYLESHEET_466_466_DARK_DISPLAY_DATA,
    .manager = STYLESHEET_466_466_DARK_MANAGER_DATA,
};

} // namespace esp_brookesia::systems::phone

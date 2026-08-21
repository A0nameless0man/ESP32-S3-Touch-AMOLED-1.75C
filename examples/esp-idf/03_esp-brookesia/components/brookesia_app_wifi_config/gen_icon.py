# 生成 126x126 ARGB8888 WiFi 弧形图标 C 数组,风格对齐系统 launcher 图标(灰色 0x8d)
import math

W = H = 126
CX, CY = 63, 92          # 圆心在下方,弧向上张开
GRAY = 0x8d
ARCS = [(16, 8), (38, 8), (60, 8)]   # (半径, 线宽/2)
DOT_R = 7

def smoothstep(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)

buf = bytearray()
for y in range(H):
    for x in range(W):
        dx, dy = x + 0.5 - CX, y + 0.5 - CY
        dist = math.hypot(dx, dy)
        # 上扇区:数学坐标 y 向上,角度 35°~145°
        ang = math.degrees(math.atan2(-dy, dx))
        in_sector = smoothstep(32, 38, ang) * (1 - smoothstep(142, 148, ang))
        alpha = 0.0
        for r, half in ARCS:
            edge = smoothstep(r - half - 1, r - half + 1, dist) * (1 - smoothstep(r + half - 1, r + half + 1, dist))
            alpha = max(alpha, edge * in_sector)
        # 底部圆点(全圆)
        dot = (1 - smoothstep(DOT_R - 1.5, DOT_R + 1.5, dist))
        alpha = max(alpha, dot)
        a = int(alpha * 255)
        buf += bytes((GRAY, GRAY, GRAY, a))

with open("esp_brookesia_app_icon_launcher_wifi_126_126.c", "w") as f:
    f.write("""/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef __has_include
#if __has_include("lvgl.h")
#ifndef LV_LVGL_H_INCLUDE_SIMPLE
#define LV_LVGL_H_INCLUDE_SIMPLE
#endif
#endif
#endif
#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#else
#include "../../lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_IMG_DATA
#define LV_ATTRIBUTE_IMG_DATA
#endif

static const uint8_t LV_ATTRIBUTE_IMG_DATA esp_brookesia_app_icon_launcher_wifi_126_126_map[] = {
""")
    for i in range(0, len(buf), 32):
        row = ", ".join(f"0x{b:02x}" for b in buf[i:i+32])
        f.write("    " + row + ",\n")
    f.write("""};

const lv_image_dsc_t esp_brookesia_app_icon_launcher_wifi_126_126 = {
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.w = 126,
    .header.h = 126,
    .data_size = 126 * 126 * 4,
    .data = esp_brookesia_app_icon_launcher_wifi_126_126_map,
};
""")

print("icon written, bytes:", len(buf))

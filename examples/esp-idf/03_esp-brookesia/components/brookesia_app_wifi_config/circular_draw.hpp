/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 圆屏通用裁剪绘制:把对象越出参考圆的部分整块裁掉,并在每条边与
 * 圆的交叉点处生成与两边相切的圆角。按"环带 + 边交叉"分解,与
 * 对象大小/位置无关:
 *
 *   1. 完整环形带(内半径=参考圆,外半径=覆盖对象最远角)盖掉圆外
 *      全部区域 —— 任意数量角/整条边出界/极端形态统一成立,零分支;
 *      圆外区域在物理屏幕上本就不可见,全盖无副作用。
 *   2. 每条边独立找与圆的交点,交点在边段内 → 在该处放一枚圆角盘:
 *      与该边相切(圆心内缩 fillet_r)且与参考圆内切(|F-C|=R-r)。
 *      可见边界因此是 直线边 —切—> 圆角弧 —切—> 圆弧,全程 G1 光滑。
 *
 * 绘制挂 DRAW_MAIN 事件,每帧按对象实时坐标重算;对象完全在圆内
 * 时零绘制,可无差别挂到任意对象复用。
 */
#pragma once

#include "lvgl.h"

namespace circdraw {

// 裁剪参考圆(默认屏幕内切圆缩进 inset 像素)
struct ArcGeom {
    int cx, cy, r;
};

inline ArcGeom screen_circle(int inset = 2)
{
    lv_display_t *d = lv_display_get_default();
    int w = lv_display_get_horizontal_resolution(d);
    int h = lv_display_get_vertical_resolution(d);
    return {w / 2, h / 2, (w < h ? w : h) / 2 - inset};
}

/**
 * 挂载圆屏裁剪绘制到容器对象。
 *
 * 对容器的每个直接子对象(按键/卡片)用自己的矩形独立做几何:
 * 切割线贴子对象自身边界,圆角长在子对象与圆的交叉处。
 * 挂在容器的几何会与子对象错位(容器通常比子对象大一圈),
 * 圆角落在透明容器边上不可见 —— 这是本 API 遍历子树的原因。
 *
 * fillet_r  边↔圆交叉点处的圆角半径(0 = 直切,边界在交叉点转折)
 * bg_color  裁剪填充色,取子对象背后的背景色
 * geom      裁剪参考圆(默认屏幕圆;inset=0 切割线即屏幕物理圆边)
 */
void attach_circle_clip(lv_obj_t *obj, int fillet_r, uint32_t bg_color,
                        ArcGeom geom = screen_circle(2));

} // namespace circdraw

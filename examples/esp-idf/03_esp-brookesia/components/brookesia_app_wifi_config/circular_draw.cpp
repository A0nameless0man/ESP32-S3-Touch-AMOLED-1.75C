/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "circular_draw.hpp"
#include <math.h>

namespace circdraw {

struct ClipCfg {
    ArcGeom geom;
    int fillet_r;
    uint32_t bg;
};

#define N_SAMPLES 12

/*
 * 在边与参考圆的交叉点处画"圆角咬角"。
 *
 * 咬角区域是曲边三角形,三段边界:
 *   线段 X→T1   (沿直边;X=边与圆交点,T1=圆角盘与直边切点)
 *   圆角弧 T1→T2 (圆角盘上;T2=圆角盘与参考圆的内切点)
 *   圆弧   T2→X  (参考圆上)
 * 用 X 为轴心的三角扇填充(区域对 X 星形凸,扇覆盖无损)。
 *
 * h_vert=false: 水平边 y=e,s_int=+1 顶边(对象在下)/-1 底边;边段 x∈[lo,hi]
 * h_vert=true : 竖直边 x=e,s_int=+1 左边/-1 右边;边段 y∈[lo,hi]
 */
static void fillet_edge(lv_layer_t *layer, const ClipCfg *cfg, bool is_vert,
                        int e, int s_int, int lo, int hi)
{
    const float C_X = cfg->geom.cx, C_Y = cfg->geom.cy, R = cfg->geom.r;
    const float rho = cfg->fillet_r;
    if (rho <= 0) {
        return;
    }

    for (int side = -1; side <= 1; side += 2) {
        // 交叉点 X:边与参考圆的交点(取靠 side 一侧)
        float dc = (is_vert ? (e - C_X) : (e - C_Y));
        float d2 = R * R - dc * dc;
        if (d2 <= 0) {
            continue; // 整条边在圆外,交叉不存在
        }
        float x_cross = C_X + side * sqrtf(d2); // 水平边的 x 坐标(竖直边则是 y)
        if (x_cross < lo || x_cross > hi) {
            continue;
        }

        // 圆角盘圆心 F:沿边内缩 rho 相切于边;内切于参考圆(|F-C|=R-rho)
        // 在边方向解出切点 T1 的边向坐标
        float f_in = e + s_int * rho;
        float t2 = (R - rho) * (R - rho) -
                   (is_vert ? (f_in - C_X) * (f_in - C_X) : (f_in - C_Y) * (f_in - C_Y));
        if (t2 <= 0) {
            continue;
        }
        float x_tan = C_X + side * sqrtf(t2);
        if (x_tan < lo || x_tan > hi) {
            continue;
        }
        // 切点必须落在交叉点朝对象内侧(对侧交叉点方向)的一侧:
        // 若画到外侧,咬角落在环带已盖住的圆外区域,可见边界在交叉点处
        // 留下 G0 折点(直边直接撞圆弧,没有圆角过渡)
        float x_other = C_X - side * sqrtf(d2);
        if ((x_tan - x_cross) * (x_other - x_cross) <= 0) {
            continue;
        }

        // 统一映射回坐标:水平边 F=(x_tan,f_in),竖直边 F=(f_in,x_tan)
        float fx = is_vert ? f_in : x_tan;
        float fy = is_vert ? x_tan : f_in;
        float t1x = is_vert ? e : x_tan;
        float t1y = is_vert ? x_tan : e;
        float xx = is_vert ? e : x_cross;
        float xy = is_vert ? x_cross : e;

        // T2 = F 沿 F-C 方向推到参考圆上(内切点)
        float len = hypotf(fx - C_X, fy - C_Y); // = R-rho
        float t2x = C_X + (fx - C_X) * R / len;
        float t2y = C_Y + (fy - C_Y) * R / len;

        // 边界点列:X → 圆弧(T2→X) → T2 → 圆角弧(T2→T1) → T1
        // 采样后以 X 为轴心做三角扇
        struct Pt { float x, y; };
        Pt pts[N_SAMPLES * 2 + 3];
        int n = 0;

        // 圆弧 T2→X(参考圆上):角度取劣弧方向
        float a_t2 = atan2f(t2y - C_Y, t2x - C_X);
        float a_x = atan2f(xy - C_Y, xx - C_X);
        float sweep = a_x - a_t2;
        while (sweep > M_PI) sweep -= 2 * (float)M_PI;
        while (sweep < -M_PI) sweep += 2 * (float)M_PI;
        for (int i = 0; i <= N_SAMPLES; i++) {
            float a = a_t2 + sweep * i / N_SAMPLES;
            pts[n].x = C_X + R * cosf(a);
            pts[n].y = C_Y + R * sinf(a);
            n++;
        }
        // 圆角弧 T2→T1(圆角盘上,圆心 F):同样取短弧
        float b_t2 = atan2f(t2y - fy, t2x - fx);
        float b_t1 = atan2f(t1y - fy, t1x - fx);
        float sweep2 = b_t1 - b_t2;
        while (sweep2 > M_PI) sweep2 -= 2 * (float)M_PI;
        while (sweep2 < -M_PI) sweep2 += 2 * (float)M_PI;
        for (int i = 0; i <= N_SAMPLES; i++) {
            float a = b_t2 + sweep2 * i / N_SAMPLES;
            pts[n].x = fx + rho * cosf(a);
            pts[n].y = fy + rho * sinf(a);
            n++;
        }
        if (n < 3) {
            continue;
        }

        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = lv_color_hex(cfg->bg);
        tri.opa = LV_OPA_COVER;
        for (int i = 0; i + 1 < n; i++) {
            // 轴心 X 与相邻两点组成扇面
            tri.p[0] = {(lv_value_precise_t)xx, (lv_value_precise_t)xy};
            tri.p[1] = {(lv_value_precise_t)pts[i].x, (lv_value_precise_t)pts[i].y};
            tri.p[2] = {(lv_value_precise_t)pts[i + 1].x, (lv_value_precise_t)pts[i + 1].y};
            lv_draw_triangle(layer, &tri);
        }
    }
}

static void circle_clip_draw_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ClipCfg *cfg = (ClipCfg *)lv_event_get_user_data(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(cfg);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    const float C_X = cfg->geom.cx, C_Y = cfg->geom.cy, R = cfg->geom.r;

    // 快速路径:四个角全在圆内则无事可做
    float max_r = 0;
    const int xs[2] = {co.x1, co.x2};
    const int ys[2] = {co.y1, co.y2};
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            float r = hypotf(xs[i] - C_X, ys[j] - C_Y);
            if (r > max_r) {
                max_r = r;
            }
        }
    }
    if (max_r <= R) {
        return;
    }

    // 1) 完整环形带:内缘=参考圆,外缘盖过最远角。圆外一律变背景色,
    //    覆盖范围与对象形状无关,不需要任何角/边组合枚举
    int outer = (int)max_r + 4;
    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.center = {(lv_value_precise_t)(int)C_X, (lv_value_precise_t)(int)C_Y};
    ring.radius = (int)((R + outer) / 2.0f);
    ring.width = outer - (int)R;
    ring.start_angle = 0;
    ring.end_angle = 360;
    ring.color = lv_color_hex(cfg->bg);
    ring.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &ring);

    // 2) 四条边逐条配圆角:咬角楔块 = 直边段 + 圆角弧 + 圆弧,
    //    圆角盘与边相切、与参考圆内切,边界全程 G1 光滑
    fillet_edge(layer, cfg, false, co.y1, +1, co.x1, co.x2); // 顶边
    fillet_edge(layer, cfg, false, co.y2, -1, co.x1, co.x2); // 底边
    fillet_edge(layer, cfg, true,  co.x1, +1, co.y1, co.y2); // 左边
    fillet_edge(layer, cfg, true,  co.x2, -1, co.y1, co.y2); // 右边
}

void attach_circle_clip(lv_obj_t *obj, int fillet_r, uint32_t bg_color, ArcGeom geom)
{
    ClipCfg *cfg = (ClipCfg *)lv_malloc(sizeof(ClipCfg));
    cfg->geom = geom;
    cfg->fillet_r = fillet_r;
    cfg->bg = bg_color;
    // DRAW_MAIN 在控件本体绘制之后、子对象之前触发 → 环带能盖住越界部分,
    // 而标签(子对象)画在其上不受影响;DELETE 时释放配置
    lv_obj_add_event_cb(obj, circle_clip_draw_cb, LV_EVENT_DRAW_MAIN, cfg);
    lv_obj_add_event_cb(obj, circle_clip_draw_cb, LV_EVENT_DELETE, cfg);
}

} // namespace circdraw

#!/usr/bin/env python3
"""VelaPet 前盖板 STL 生成器（v3，基于 2026-08-18 实测数据）.

v3 相对 v2 的关键修正
--------------------
v2 假设盖板架在螺柱上、与所有元件之间有间隙，于是眼窗只按 AA 可视区
开 Φ19.5 圆孔。**这个假设是错的** —— 实测屏玻璃面与摄像头顶面都
高出盖板底面 2.5mm，也就是说它们必须从盖板里穿出去，否则要么扣不上，
要么深陷在孔里很难看。

v3 因此改为：
  盖板厚度 2.5mm      → 玻璃面 / 摄像头顶面与盖板前表面基本齐平
  眼窗改水滴形通孔    → 让玻璃连底部 4mm 的 FPC/IC 凸起一起穿出
  摄像头改方孔通孔    → 8x8 方壳要穿出，圆孔过不去（对角线 11.3mm）
  咪头改单个圆孔      → 咪头是立起来的圆柱、也要穿出；比三个小孔
                        声学上更好（无管道效应）

为什么眼窗露出底部凸起不影响观感：玻璃是黑的、FPC 绑定区也是黑的，
露出来就是一整块黑色水滴形，中间一个发光的 AA 圆 —— 与实物一致，
不发光的部分不构成视觉焦点。若要遮住它就得留前唇边，而那会把玻璃
压住或迫使屏幕凹陷，得不偿失。

实测数据（2026-08-18）
--------------------
PCB                    60.0 x 50.0 mm
安装孔中心距           54.0 (X) x 44.0 (Y) mm   （非位号图的 57x47）
屏玻璃直径             20.0 mm
屏模块高               24.0 mm（含底部 4mm FPC/IC 凸起）
两眼 AA 中心距         32.0 mm
上边界到 AA 横线       26.0 mm   → 玻璃圆心 Y=26、模块包围盒心 Y=28
玻璃面 / 摄像头顶      高出盖板底面 2.5mm
摄像头模块             8 x 8 mm，抵上边界，距左边界 11mm
咪头                   Φ7.5 圆柱，焊在 PCB 上不可挪动，略低于玻璃面

坐标系
------
规格坐标：原点在盖板左上角，X 向右，Y 向下（与测量时一致）。
模型坐标：Y 轴翻转向上（y_model = H - y_spec），使切片软件里额头在上、
          嘴在下，符合直觉。
"""

import math
import struct

import mapbox_earcut
import numpy as np

# ---------------------------------------------------------------- 参数

W, H = 62.0, 52.0     # 盖板外形（比 PCB 60x50 每边外伸 1mm，给螺钉头留材料）
T = 2.5               # 厚度：与凸起 2.5mm 齐平
CORNER_R = 2.0

# --- 眼窗 ---
# 模块实际轮廓 = 上面 Φ20 玻璃圆 + 下面一段较窄的 FPC/IC 凸起，
# 包围盒在规格坐标 Y=16~40（高 24mm）。
#
# EYE_SHAPE 三选一：
#   'tangent'  平滑水滴形：槽侧边从圆上**相切**引出 → 曲率连续、无转折点，
#              与屏幕实际外形一致。**推荐**
#              早先误判为"做不到"，是因为当时要求切线同时相切于底部小圆
#              （过强约束，只剩 0.02mm 间隙）。改成直线只通过底部角点即可，
#              实算处处 ≥1mm 间隙。见 fit_check() 的自动校验。
#   'teardrop' 圆 + 梯形槽 + 交界凹圆角。能装，但交界处仍有可见转折
#   'stadium'  胶囊形：上下等宽 21mm。不需要知道凸起宽度，一定装得进去，
#              但下半部分白挖了材料，看起来上下一样宽
#   'circle'   纯圆 Φ21：仅当底部凸起在 Z 方向明显低于玻璃面、
#              可以整个藏在盖板下面时才可用。观感最干净
EYE_SHAPE = 'tangent'

EYE_GLASS_D = 20.0     # 玻璃直径（实测）
EYE_CLEAR = 1.0        # 直径方向总间隙（单边 0.5mm）
EYE_D = EYE_GLASS_D + EYE_CLEAR          # 圆部分开口直径 = 21.0

# 底部凸起（蓝色绑定条 + 棕色 FPC）宽度，2026-08-18 实测：
#   上边 10.0mm、下边 6.5mm（梯形），高约 2.5mm，起点正好在玻璃圆下沿
#   （距圆眼上边 20mm = 玻璃直径，即紧接圆的最低点）
# 取最宽处 10.0；再往下的棕色 FPC 更窄（约 6.5），一并被覆盖。
EYE_TAB_W = 10.0

# 凸起顶面比玻璃面低 0.5mm（实测）。
# 注意：这不足以把它藏在盖板下面 —— 玻璃面高出盖板底面 2.5mm，
# 凸起因此仍高出 2.0mm，要在 2.5mm 厚的板上挖 2.0mm 深的让位坑
# 只剩 0.5mm 壁，太薄。故 'circle' 模式不可用，必须开通槽。
EYE_TAB_BELOW_GLASS = 0.5
# 窄槽下端宽度。凸起实测是梯形（上 10.0、下 6.5），所以槽也可以跟着收窄：
#   EYE_TAB_W_BOT = 0     直槽，上下等宽 = EYE_TAB_W。最保守，一定装得进
#   EYE_TAB_W_BOT = 6.5   梯形槽，跟随凸起实际收窄。更像水滴、多留材料 **推荐**
# 注意：完全平滑的水滴（大圆到小圆的外切线）做不到 —— 算过，切线在
# 玻璃圆下沿高度处的半宽只有 5.02mm，而凸起在那里要 5.0mm，
# 仅剩 0.02mm 间隙，必然装不进去。只能在直槽/梯形槽之间选。
EYE_TAB_W_BOT = 6.5

EYE_TAB_CLEAR = 1.0    # 凸起宽度方向总间隙（单边 0.5mm）
EYE_TAB_BOT_R = 1.5    # 窄槽底部圆角（梯形时收窄后半宽仅 3.75，R 要小一点）

# 圆与窄槽交界处的**凹圆角**半径。
#
# ⚠️ 这个不是装饰，是装配必需的。实物照片显示模块的玻璃圆与底部凸起之间
# 是平滑过渡（有过渡圆弧），也就是模块在那个夹角处**多出一块材料**；
# 而"圆 ∪ 梯形"的开口在那里是一个**尖的材料凸刺**伸进去。
# 尖刺塞不进圆角 —— 凸刺尖端会顶在模块的过渡圆弧上，导致装不进去或硬压。
#
# 倒圆只会让开口变大，所以**宁大勿小**：取 3.0mm 已远超模块可能的过渡半径。
# 设为 0 则退回尖角（不推荐）。
EYE_JOINT_R = 3.0

# --- 'tangent' 模式专用 ---
# 槽底开口全宽。切线从圆上相切引出、下行至底部两角点，
# 所以底宽越大切线越"外扩"、间隙越大，但挖掉的材料也越多。
# 8.5 = 凸起下端实测 6.5 + 2.0（切线会向内收，底部要多留）。
EYE_BOT_W = 8.5

# --- 模块实测轮廓，用于 fit_check() 自动校验开口是否装得进 ---
# (Y, 全宽)：玻璃圆段由 MOD_GLASS_* 生成，这里只列圆以下的凸起段。
MOD_GLASS_D = 20.0     # 玻璃直径
MOD_GLASS_CY = 26.0    # 玻璃圆心 Y（= AA 圆心）
MOD_TAB_PROFILE = [
    (36.0, 10.0),      # 蓝色绑定条上沿，正好接玻璃圆下沿
    (38.5, 6.5),       # 蓝色绑定条下沿
    (40.0, 6.5),       # 棕色 FPC，到模块包围盒下沿
]
FIT_MIN_CLEAR = 0.4    # 每侧最小可接受间隙 (mm)

EYE_CIRC_CY = 26.0     # 玻璃圆心 Y（规格坐标）= AA 圆心 Y，实测 16+10
EYE_BBOX_BOT = 40.0    # 模块包围盒下沿 Y（规格坐标）= 16+24
EYE_BOT_CLEAR = 0.5    # 下沿间隙

# stadium 模式用的等效尺寸（保持与 v3 首版一致）
EYE_W, EYE_H = EYE_D, 25.5

EYES_X = [15.0, 47.0]  # 两眼 X，中心距 32.0（实测 AA 中心距）

# --- 安装孔 ---
MOUNT_D = 3.2
MOUNTS_SPEC = [(4.0, 4.0), (58.0, 4.0), (4.0, 48.0), (58.0, 48.0)]

# --- 摄像头：8x8 方壳要穿出 → 方孔（圆孔需 Φ11.3 才过得去，太大）---
CAM_SIDE = 8.5        # 单边 0.25mm 间隙
CAM_FILLET = 1.0
CAM_SPEC = (16.0, 8.0)

# --- 咪头：Φ7.5 圆柱穿出 ---
MIC_D = 8.0           # 单边 0.25mm 间隙
MIC_SPEC = (31.0, 45.25)

SEG_FINE = 180
SEG_COARSE = 64


def to_model(p):
    return (p[0], H - p[1])


# ---------------------------------------------------------------- 轮廓构造

def arc(cx, cy, r, a0, a1, n):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def dedup(pts):
    out = [pts[0]]
    for p in pts[1:]:
        if math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) > 1e-9:
            out.append(p)
    if math.hypot(out[0][0] - out[-1][0], out[0][1] - out[-1][1]) < 1e-9:
        out.pop()
    return out


def rounded_rect(cx, cy, w, h, r, n=16):
    """圆角矩形，逆时针。r 可等于 min(w,h)/2（退化为胶囊/水滴形）."""
    hw, hh = w / 2.0 - r, h / 2.0 - r
    pts = []
    pts += arc(cx + hw, cy + hh, r, 0, 90, n)       # 右上
    pts += arc(cx - hw, cy + hh, r, 90, 180, n)     # 左上
    pts += arc(cx - hw, cy - hh, r, 180, 270, n)    # 左下
    pts += arc(cx + hw, cy - hh, r, 270, 360, n)    # 右下
    return dedup(pts)


def circle(cx, cy, r, seg):
    return [(cx + r * math.cos(2 * math.pi * i / seg),
             cy + r * math.sin(2 * math.pi * i / seg))
            for i in range(seg)]


def fillet_corner(a, c, b, r, n=8):
    """在角点 c（前一点 a、后一点 b）处生成圆角弧点列.

    退化情形（半径过大 / 夹角接近平角）直接返回尖角，保证不会生成
    自交或翻面的轮廓 —— 这个脚本没有交互式预览，宁可少个圆角也不能出坏网格。
    """
    ax, ay = a[0] - c[0], a[1] - c[1]
    bx, by = b[0] - c[0], b[1] - c[1]
    la = math.hypot(ax, ay)
    lb = math.hypot(bx, by)
    if la < 1e-9 or lb < 1e-9:
        return [c]
    ax, ay = ax / la, ay / la
    bx, by = bx / lb, by / lb

    cosang = max(-1.0, min(1.0, ax * bx + ay * by))
    half = math.acos(cosang) / 2.0
    if half < 1e-3 or half > math.pi / 2 - 1e-3:
        return [c]

    t = r / math.tan(half)
    if t > la * 0.95 or t > lb * 0.95:
        t = min(la, lb) * 0.95
        r = t * math.tan(half)

    t1 = (c[0] + ax * t, c[1] + ay * t)
    t2 = (c[0] + bx * t, c[1] + by * t)

    mx, my = ax + bx, ay + by
    lm = math.hypot(mx, my)
    if lm < 1e-9:
        return [c]
    dist = r / math.sin(half)
    ctr = (c[0] + mx / lm * dist, c[1] + my / lm * dist)

    a0 = math.degrees(math.atan2(t1[1] - ctr[1], t1[0] - ctr[0]))
    a1 = math.degrees(math.atan2(t2[1] - ctr[1], t2[0] - ctr[0]))
    while a1 - a0 > 180.0:
        a1 -= 360.0
    while a1 - a0 < -180.0:
        a1 += 360.0
    return arc(ctr[0], ctr[1], r, a0, a1, n)


def fillet_circle_line(oc, r, a, b, rf, nrm, n=14):
    """外切于圆(oc, r)、内切于线段 a->b 的**凹圆角**.

        oc, r   圆心、半径
        a, b    线段端点，a 为靠交界的一端
        rf      圆角半径
        nrm     线段的外法线（指向远离槽内部的一侧）

    返回 (弧点列, 圆上切点, 线上切点)；几何退化时返回 None，
    调用方退回尖角。这个脚本没有交互式预览，宁可少个圆角也不能出坏轮廓。
    """
    ux, uy = b[0] - a[0], b[1] - a[1]
    lu = math.hypot(ux, uy)
    if lu < 1e-9 or rf <= 1e-6:
        return None
    ux, uy = ux / lu, uy / lu

    # 圆角圆心在「线段外移 rf」这条平行线上，且距圆心 r+rf
    ax = a[0] + nrm[0] * rf
    ay = a[1] + nrm[1] * rf
    dx, dy = ax - oc[0], ay - oc[1]
    du = dx * ux + dy * uy
    disc = du * du - (dx * dx + dy * dy) + (r + rf) ** 2
    if disc < 0.0:
        return None
    sq = math.sqrt(disc)

    best = None
    for s in (-du - sq, -du + sq):
        if -1e-6 <= s <= lu * 0.98:
            if best is None or abs(s) < abs(best):
                best = s
    if best is None:
        return None

    cxx = ax + ux * best
    cyy = ay + uy * best
    dist = math.hypot(cxx - oc[0], cyy - oc[1])
    if dist < 1e-9:
        return None

    t_circ = (oc[0] + (cxx - oc[0]) * r / dist,
              oc[1] + (cyy - oc[1]) * r / dist)
    t_line = (cxx - nrm[0] * rf, cyy - nrm[1] * rf)

    a0 = math.degrees(math.atan2(t_circ[1] - cyy, t_circ[0] - cxx))
    a1 = math.degrees(math.atan2(t_line[1] - cyy, t_line[0] - cxx))
    while a1 - a0 > 180.0:
        a1 -= 360.0
    while a1 - a0 < -180.0:
        a1 += 360.0
    return arc(cxx, cyy, rf, a0, a1, n), t_circ, t_line


def tangent_drop(cx, cy_circ, r, bot_w, y_bot, bot_r, n=90):
    """平滑水滴形：槽侧边从圆上**相切**引出，下行至底部两角点.

    切线与圆相切 => 交界处切线方向连续（G1），没有可见转折点，
    与屏幕模块的实际外形一致。

    从底部角点 Q 向圆作外切线：|OQ| = L，半角 α = acos(r/L)，
    OQ 方位角 φ，则右侧切点在 φ+α、左侧在 φ-α。
    """
    wb = bot_w / 2.0
    qr = (cx + wb, y_bot)
    ql = (cx - wb, y_bot)

    lr = math.hypot(qr[0] - cx, qr[1] - cy_circ)
    if lr <= r + 1e-6:
        return None                      # 底角落在圆内，无外切线

    alpha = math.degrees(math.acos(max(-1.0, min(1.0, r / lr))))
    phi_r = math.degrees(math.atan2(qr[1] - cy_circ, qr[0] - cx))
    phi_l = math.degrees(math.atan2(ql[1] - cy_circ, ql[0] - cx))

    a_r = phi_r + alpha                  # 右切点
    a_l = phi_l - alpha                  # 左切点

    tr = (cx + r * math.cos(math.radians(a_r)),
          cy_circ + r * math.sin(math.radians(a_r)))
    tl = (cx + r * math.cos(math.radians(a_l)),
          cy_circ + r * math.sin(math.radians(a_l)))

    # 圆弧：右切点 -> 逆时针经圆顶 -> 左切点
    s, e = a_r, a_l
    while e < s:
        e += 360.0
    pts = arc(cx, cy_circ, r, s, e, n)

    pts += fillet_corner(tl, ql, qr, bot_r)
    pts += fillet_corner(ql, qr, tr, bot_r)

    return dedup(pts)


def teardrop(cx, cy_circ, r, tab_w, y_bot, bot_r, n=60, tab_w_bot=0.0,
             joint_r=0.0):
    """水滴形：上部圆 + 下部窄槽，逆时针（模型坐标 Y 向上，槽朝下）.

        cx, cy_circ  圆心
        r            圆半径
        tab_w        窄槽全宽
        y_bot        槽底 Y（小于 cy_circ）
        bot_r        槽底圆角半径

    圆与槽两侧直边的交点在 y = cy_circ - sqrt(r^2 - w^2)，w = tab_w/2。
    从右交点出发沿圆逆时针绕到左交点，再下行、过底、右行回到起点。
    """
    w = tab_w / 2.0
    if w >= r:                     # 槽比圆还宽 -> 退化为胶囊
        return rounded_rect(cx, (cy_circ + y_bot) / 2.0,
                            2 * r, (cy_circ + r) - y_bot, r, n=45)

    d = math.sqrt(r * r - w * w)
    y_int = cy_circ - d

    # 槽下端半宽：0 表示直槽（上下等宽）
    wb = w if tab_w_bot <= 0.0 else tab_w_bot / 2.0
    wb = min(wb, w)

    oc = (cx, cy_circ)
    pl_top = (cx - w, y_int)
    pl_bot = (cx - wb, y_bot)
    pr_bot = (cx + wb, y_bot)
    pr_top = (cx + w, y_int)

    # 槽两侧边的外法线（指向远离槽中心线的一侧）
    def outward(p_top, p_bot, sign):
        vx, vy = p_bot[0] - p_top[0], p_bot[1] - p_top[1]
        lv = math.hypot(vx, vy)
        if lv < 1e-9:
            return (sign, 0.0)
        # 垂直于边，取 x 分量与 sign 同号的那个
        nx, ny = vy / lv, -vx / lv
        if nx * sign < 0:
            nx, ny = -nx, -ny
        return (nx, ny)

    fl = fillet_circle_line(oc, r, pl_top, pl_bot, joint_r,
                            outward(pl_top, pl_bot, -1.0)) \
        if joint_r > 1e-6 else None
    fr = fillet_circle_line(oc, r, pr_top, pr_bot, joint_r,
                            outward(pr_top, pr_bot, +1.0)) \
        if joint_r > 1e-6 else None

    if fl is None or fr is None:
        # 退回尖角交界
        a_r = math.degrees(math.atan2(-d, w))
        a_l = 180.0 - a_r
        pts = arc(cx, cy_circ, r, a_r, a_l, n)
        pts.append(pl_top)
        pts += fillet_corner(pl_top, pl_bot, pr_bot, bot_r)
        pts += fillet_corner(pl_bot, pr_bot, pr_top, bot_r)
        pts.append(pr_top)
        return dedup(pts)

    arc_l, tc_l, tl_l = fl
    arc_r, tc_r, tl_r = fr

    a_start = math.degrees(math.atan2(tc_r[1] - cy_circ, tc_r[0] - cx))
    a_end = math.degrees(math.atan2(tc_l[1] - cy_circ, tc_l[0] - cx))
    while a_end < a_start:
        a_end += 360.0

    pts = arc(cx, cy_circ, r, a_start, a_end, n)   # 圆顶：右切点 -> 左切点
    pts += arc_l                                   # 左凹圆角：圆 -> 槽边
    pts += fillet_corner(tl_l, pl_bot, pr_bot, bot_r)
    pts += fillet_corner(pl_bot, pr_bot, tl_r, bot_r)
    pts += list(reversed(arc_r))                   # 右凹圆角：槽边 -> 圆

    return dedup(pts)


def eye_loop(cx_spec):
    """按 EYE_SHAPE 生成一只眼窗的轮廓（模型坐标）."""
    cx = cx_spec
    if EYE_SHAPE == 'circle':
        return circle(cx, H - EYE_CIRC_CY, EYE_D / 2.0, SEG_FINE)

    if EYE_SHAPE == 'tangent':
        loop = tangent_drop(cx,
                            H - EYE_CIRC_CY,
                            EYE_D / 2.0,
                            EYE_BOT_W,
                            H - (EYE_BBOX_BOT + EYE_BOT_CLEAR),
                            EYE_TAB_BOT_R)
        if loop is not None:
            return loop
        print('!! tangent 模式几何退化，退回 teardrop')

    if EYE_SHAPE in ('teardrop', 'tangent'):
        wb = (EYE_TAB_W_BOT + EYE_TAB_CLEAR) if EYE_TAB_W_BOT > 0 else 0.0
        return teardrop(cx,
                        H - EYE_CIRC_CY,
                        EYE_D / 2.0,
                        EYE_TAB_W + EYE_TAB_CLEAR,
                        H - (EYE_BBOX_BOT + EYE_BOT_CLEAR),
                        EYE_TAB_BOT_R,
                        tab_w_bot=wb,
                        joint_r=EYE_JOINT_R)

    # stadium：上下等宽，中心取模块包围盒中心
    cy_spec = (EYE_CIRC_CY - EYE_D / 2.0
               + EYE_BBOX_BOT + EYE_BOT_CLEAR) / 2.0
    return rounded_rect(cx, H - cy_spec, EYE_W, EYE_H, EYE_W / 2.0, n=45)


# 外形
OUTER = rounded_rect(W / 2.0, H / 2.0, W, H, CORNER_R)

# 各开口（模型坐标）
HOLE_LOOPS = []
HOLE_INFO = []

_EYE_DESC = {
    'tangent': f'平滑水滴形 圆Φ{EYE_D} 相切引出 + 槽底宽{EYE_BOT_W}',
    'teardrop': (f'水滴形 圆Φ{EYE_D} + 槽{EYE_TAB_W + EYE_TAB_CLEAR:.1f}'
                 + (f'→{EYE_TAB_W_BOT + EYE_TAB_CLEAR:.1f} 梯形'
                    if EYE_TAB_W_BOT > 0 else ' 直槽')
                 + (f' + 交界凹圆角R{EYE_JOINT_R}'
                    if EYE_JOINT_R > 0 else ' 尖角交界')),
    'stadium': f'胶囊形 {EYE_W} x {EYE_H}',
    'circle': f'纯圆 Φ{EYE_D}',
}[EYE_SHAPE]

for _cx in EYES_X:
    HOLE_LOOPS.append(eye_loop(_cx))
    HOLE_INFO.append(('眼窗', _EYE_DESC, (_cx, H - EYE_CIRC_CY)))

for c in MOUNTS_SPEC:
    m = to_model(c)
    HOLE_LOOPS.append(circle(m[0], m[1], MOUNT_D / 2.0, SEG_COARSE))
    HOLE_INFO.append(('安装孔', f'Φ{MOUNT_D}', m))

_m = to_model(CAM_SPEC)
HOLE_LOOPS.append(rounded_rect(_m[0], _m[1], CAM_SIDE, CAM_SIDE,
                               CAM_FILLET, n=12))
HOLE_INFO.append(('摄像头孔', f'{CAM_SIDE} x {CAM_SIDE} 方孔 R{CAM_FILLET}', _m))

_m = to_model(MIC_SPEC)
HOLE_LOOPS.append(circle(_m[0], _m[1], MIC_D / 2.0, SEG_FINE))
HOLE_INFO.append(('咪头孔', f'Φ{MIC_D}', _m))


# ---------------------------------------------------------------- 三角化

def triangulate():
    """带孔多边形三角化（earcut）：外环逆时针，孔环顺时针."""
    verts = list(OUTER)
    rings = [len(verts)]
    for loop in HOLE_LOOPS:
        verts.extend(reversed(loop))
        rings.append(len(verts))

    P = np.asarray(verts, dtype=np.float64)
    idx = mapbox_earcut.triangulate_float64(
        P, np.asarray(rings, dtype=np.uint32))
    faces = [tuple(int(v) for v in idx[i:i + 3])
             for i in range(0, len(idx), 3)]
    return P, faces


def build_mesh():
    P, faces = triangulate()
    tris = []

    def ccw(p0, p1, p2):
        return (p1[0] - p0[0]) * (p2[1] - p0[1]) - \
               (p1[1] - p0[1]) * (p2[0] - p0[0]) > 0

    for a, b, c in faces:
        p0, p1, p2 = P[a], P[b], P[c]
        if not ccw(p0, p1, p2):
            p1, p2 = p2, p1
        tris.append(((p0[0], p0[1], T), (p1[0], p1[1], T), (p2[0], p2[1], T)))
        tris.append(((p0[0], p0[1], 0.0), (p2[0], p2[1], 0.0),
                     (p1[0], p1[1], 0.0)))

    def wall(loop, outward):
        n = len(loop)
        for i in range(n):
            x0, y0 = loop[i]
            x1, y1 = loop[(i + 1) % n]
            a0, a1 = (x0, y0, 0.0), (x1, y1, 0.0)
            b0, b1 = (x0, y0, T), (x1, y1, T)
            if outward:
                tris.append((a0, a1, b1))
                tris.append((a0, b1, b0))
            else:
                tris.append((a0, b1, a1))
                tris.append((a0, b0, b1))

    wall(OUTER, outward=True)
    for loop in HOLE_LOOPS:
        wall(loop, outward=False)

    return tris


def write_stl(tris, path):
    with open(path, 'wb') as f:
        f.write(b'VelaPet front plate v3 (measured 2026-08-18)'
                .ljust(80, b'\0'))
        f.write(struct.pack('<I', len(tris)))
        for t in tris:
            p0, p1, p2 = (np.asarray(p, dtype=float) for p in t)
            n = np.cross(p1 - p0, p2 - p0)
            ln = np.linalg.norm(n)
            n = n / ln if ln > 1e-12 else np.zeros(3)
            f.write(struct.pack('<3f', *n))
            for p in (p0, p1, p2):
                f.write(struct.pack('<3f', *p))
            f.write(struct.pack('<H', 0))


def write_scad(path):
    lines = [
        '// VelaPet 前盖板 v3 — 2026-08-18 实测数据',
        '// 眼窗/摄像头/咪头均为通孔：三处凸起都要穿出盖板（各高出底面 2.5mm）',
        '$fn = 180;',
        f'W = {W}; H = {H}; T = {T}; R = {CORNER_R};',
        '',
        'module rrect(w, h, r) {',
        '  hull() for (sx = [-1, 1], sy = [-1, 1])',
        '    translate([sx * (w / 2 - r), sy * (h / 2 - r)]) circle(r = r);',
        '}',
        '',
        'difference() {',
        '  linear_extrude(height = T) rrect(W, H, R);',
    ]
    for cx in EYES_X:
        cyc = H - EYE_CIRC_CY
        r = EYE_D / 2.0
        if EYE_SHAPE == 'circle':
            lines.append(f'  translate([{cx:.4g}, {cyc:.4g}, -1]) '
                         f'cylinder(h = T + 2, r = {r:.4g});')
        elif EYE_SHAPE == 'teardrop':
            tw = EYE_TAB_W + EYE_TAB_CLEAR
            ybot = H - (EYE_BBOX_BOT + EYE_BOT_CLEAR)
            lines.append(f'  translate([0, 0, -1]) linear_extrude(T + 2) '
                         f'union() {{')
            lines.append(f'    translate([{cx:.4g}, {cyc:.4g}]) '
                         f'circle(r = {r:.4g});')
            lines.append(f'    translate([{cx:.4g}, '
                         f'{(cyc + ybot) / 2:.4g}]) '
                         f'rrect({tw:.4g}, {cyc - ybot:.4g}, '
                         f'{EYE_TAB_BOT_R:.4g});')
            lines.append('  }')
        else:
            cy_spec = (EYE_CIRC_CY - r + EYE_BBOX_BOT + EYE_BOT_CLEAR) / 2.0
            lines.append(f'  translate([{cx:.4g}, {H - cy_spec:.4g}, -1]) '
                         f'linear_extrude(T + 2) '
                         f'rrect({EYE_W}, {EYE_H}, {r:.4g});')
    for c in MOUNTS_SPEC:
        m = to_model(c)
        lines.append(f'  translate([{m[0]:.4g}, {m[1]:.4g}, -1]) '
                     f'cylinder(h = T + 2, r = {MOUNT_D / 2:.4g});')
    m = to_model(CAM_SPEC)
    lines.append(f'  translate([{m[0]:.4g}, {m[1]:.4g}, -1]) '
                 f'linear_extrude(T + 2) '
                 f'rrect({CAM_SIDE}, {CAM_SIDE}, {CAM_FILLET});')
    m = to_model(MIC_SPEC)
    lines.append(f'  translate([{m[0]:.4g}, {m[1]:.4g}, -1]) '
                 f'cylinder(h = T + 2, r = {MIC_D / 2:.4g});')
    lines += ['}', '']
    open(path, 'w', encoding='utf-8').write('\n'.join(lines))


def validate(tris):
    """网格自检：水密性、法线一致性、体积。

    3D 打印前必须确认：
      非流形边 = 0    每条边恰好被两个三角形共用（水密）
      朝向不一致 = 0  每条有向边的反向边存在且计数相同（法线一致）
      体积 > 0        散度定理算出的体积为正 -> 法线朝外
    """
    import collections

    key = lambda p: tuple(round(c, 4) for c in p)
    edge = collections.Counter()
    directed = collections.Counter()
    vol = 0.0
    lo = [1e9] * 3
    hi = [-1e9] * 3

    for t in tris:
        a, b, c = (key(p) for p in t)
        for u, v in ((a, b), (b, c), (c, a)):
            edge[frozenset((u, v))] += 1
            directed[(u, v)] += 1
        na, nb, nc = (np.asarray(p, dtype=float) for p in t)
        vol += float(np.dot(na, np.cross(nb, nc))) / 6.0
        for p in (a, b, c):
            for i in range(3):
                lo[i] = min(lo[i], p[i])
                hi[i] = max(hi[i], p[i])

    non_manifold = sum(1 for v in edge.values() if v != 2)
    flipped = sum(1 for k, v in directed.items()
                  if directed.get((k[1], k[0]), 0) != v)

    # 交叉验证体积：直接对实际轮廓用鞋带公式求面积。
    # 早先版本按形状硬编码面积公式（胶囊形），换成水滴形后就失效了 ——
    # 用轮廓本身算则三种 EYE_SHAPE 通用且精确，不会因改形状而误报。
    def poly_area(loop):
        s = 0.0
        n = len(loop)
        for i in range(n):
            x0, y0 = loop[i]
            x1, y1 = loop[(i + 1) % n]
            s += x0 * y1 - x1 * y0
        return abs(s) / 2.0

    area = poly_area(OUTER) - sum(poly_area(l) for l in HOLE_LOOPS)
    expect = area * T

    print()
    print('网格自检')
    print(f'  面片        {len(tris)}')
    print(f'  包围盒      X {lo[0]:.2f}~{hi[0]:.2f}  '
          f'Y {lo[1]:.2f}~{hi[1]:.2f}  Z {lo[2]:.2f}~{hi[2]:.2f}')
    print(f'  非流形边    {non_manifold}   '
          f'{"OK 水密" if non_manifold == 0 else "!! 不水密"}')
    print(f'  朝向不一致  {flipped}   '
          f'{"OK 法线一致" if flipped == 0 else "!! 法线错乱"}')
    print(f'  体积        {vol:.1f} mm^3   轮廓面积×厚度 {expect:.1f} mm^3   '
          f'偏差 {abs(vol - expect) / expect * 100:.3f}%')

    # 用轮廓算的期望值是精确的，容差可以收紧到 0.5%
    ok = (non_manifold == 0 and flipped == 0 and vol > 0
          and abs(vol - expect) / expect < 0.005)
    print(f'  结论        {"可直接下单打印" if ok else "有问题，不要下单"}')
    return ok


def module_half_width(y_spec):
    """模块实测轮廓在给定高度的半宽（规格坐标）。超出范围返回 None."""
    r = MOD_GLASS_D / 2.0
    top = MOD_GLASS_CY - r
    if y_spec < top - 1e-9:
        return None
    if y_spec <= MOD_GLASS_CY + r:                     # 玻璃圆段
        dy = y_spec - MOD_GLASS_CY
        return math.sqrt(max(r * r - dy * dy, 0.0))
    prev_y, prev_w = MOD_GLASS_CY + r, 0.0
    for y, w in MOD_TAB_PROFILE:
        if y_spec <= y + 1e-9:
            if y - prev_y < 1e-9:
                return w / 2.0
            t = (y_spec - prev_y) / (y - prev_y)
            return (prev_w + (w - prev_w) * t) / 2.0
        prev_y, prev_w = y, w
    return None


def opening_half_width(loop, cx, y_spec):
    """由实际轮廓多边形求开口在给定高度的半宽（模型坐标里做扫描线）.

    直接用生成好的多边形求交，因此对所有 EYE_SHAPE 都通用 ——
    不需要为每种形状各写一套解析公式，也就不会因为改形状而失效。
    """
    y = H - y_spec
    xs = []
    n = len(loop)
    for i in range(n):
        x0, y0 = loop[i]
        x1, y1 = loop[(i + 1) % n]
        if abs(y1 - y0) < 1e-12:
            continue
        if (y0 - y) * (y1 - y) <= 0.0:
            t = (y - y0) / (y1 - y0)
            xs.append(x0 + (x1 - x0) * t)
    if not xs:
        return None
    return max(abs(x - cx) for x in xs)


def fit_check():
    """逐高度对比「模块实测轮廓」与「开口轮廓」，确认装得进去.

    这是这块盖板最关键的校验 —— 前两版报废都是因为开口与模块几何不匹配，
    而那种错误光看网格是否水密是查不出来的。
    """
    loop = HOLE_LOOPS[0]           # 左眼
    cx = EYES_X[0]
    r = MOD_GLASS_D / 2.0
    y0 = MOD_GLASS_CY - r
    y1 = MOD_TAB_PROFILE[-1][0]

    worst = (1e9, None)
    steps = int((y1 - y0) / 0.25) + 1
    for i in range(steps + 1):
        y = y0 + (y1 - y0) * i / steps
        mw = module_half_width(y)
        ow = opening_half_width(loop, cx, y)
        if mw is None or ow is None:
            continue
        gap = ow - mw
        if gap < worst[0]:
            worst = (gap, y)

    print()
    print('装配校验（模块实测轮廓 vs 开口轮廓，逐 0.25mm 扫描）')
    for y in (y0, MOD_GLASS_CY, MOD_GLASS_CY + r, *[p[0] for p in
                                                    MOD_TAB_PROFILE]):
        mw = module_half_width(y)
        ow = opening_half_width(loop, cx, y)
        if mw is None or ow is None:
            continue
        print(f'  Y={y:5.2f}  模块半宽 {mw:5.2f}  开口半宽 {ow:5.2f}  '
              f'间隙 {ow - mw:+5.2f} mm')
    ok = worst[0] >= FIT_MIN_CLEAR
    print(f'  最小间隙 {worst[0]:+.2f} mm @ Y={worst[1]:.2f}   '
          f'{"OK 装得进" if ok else "!! 太紧，装不进或需硬压"}')
    return ok


def min_clearances():
    """关键最小材料厚度（按真实轮廓算，不是包围盒）。

    包围盒会严重低估圆形/胶囊形之间的间距 —— 曾据此误判
    "咪头孔与眼窗只剩 1.5mm"，真实值是 7.4mm。
    """
    ex = EYES_X[0]
    r = EYE_D / 2.0
    mic = MIC_SPEC
    mic_r = MIC_D / 2.0

    # 眼窗下部离咪头最近的那个特征，随形状而变
    if EYE_SHAPE == 'teardrop':
        # 槽右侧边是一条线段（梯形时为斜边），取咪头圆到该线段的最短距离
        w = (EYE_TAB_W + EYE_TAB_CLEAR) / 2.0
        wb = w if EYE_TAB_W_BOT <= 0 else (EYE_TAB_W_BOT
                                           + EYE_TAB_CLEAR) / 2.0
        d = math.sqrt(max((EYE_D / 2.0) ** 2 - w * w, 0.0))
        p0 = (ex + w, EYE_CIRC_CY + d)                     # 槽上端（接圆处）
        p1 = (ex + wb, EYE_BBOX_BOT + EYE_BOT_CLEAR)       # 槽下端
        vx, vy = p1[0] - p0[0], p1[1] - p0[1]
        wx, wy = mic[0] - p0[0], mic[1] - p0[1]
        ll = vx * vx + vy * vy
        t = 0.0 if ll < 1e-12 else max(0.0, min(1.0, (wx * vx + wy * vy) / ll))
        foot = (p0[0] + vx * t, p0[1] + vy * t)
        eye_mic = min(math.dist(foot, mic) - mic_r,
                      math.dist((ex, EYE_CIRC_CY), mic)
                      - EYE_D / 2.0 - mic_r)
    elif EYE_SHAPE == 'circle':
        eye_mic = math.dist((ex, EYE_CIRC_CY), mic) - r - mic_r
    else:
        straight = (EYE_H - EYE_D) / 2.0
        cy_spec = (EYE_CIRC_CY - r + EYE_BBOX_BOT + EYE_BOT_CLEAR) / 2.0
        eye_mic = math.dist((ex, cy_spec + straight), mic) - r - mic_r

    items = [
        ('眼窗 <-> 咪头孔', eye_mic),
        ('两眼窗之间(鼻梁)', (EYES_X[1] - r) - (EYES_X[0] + r)),
        ('眼窗 <-> 摄像头方孔',
         (EYE_CIRC_CY - r) - (CAM_SPEC[1] + CAM_SIDE / 2.0)),
        ('眼窗 <-> 盖板侧边', ex - r),
        ('摄像头孔 <-> 盖板上边', CAM_SPEC[1] - CAM_SIDE / 2.0),
        ('咪头孔 <-> 盖板下边', H - (mic[1] + mic_r)),
    ]
    print()
    print('最小材料厚度（真实轮廓）')
    for name, v in items:
        flag = 'OK' if v >= 1.5 else '!! 偏薄'
        print(f'  {name:24s} {v:6.2f} mm   {flag}')


def main():
    tris = build_mesh()
    base = '/home/zhangyan68/miwear-main/vendor/velapet/3d/VelaPet_front_plate_v3'
    write_stl(tris, base + '.stl')
    write_scad(base + '.scad')

    print(f'三角面片 : {len(tris)}')
    print(f'STL      : {base}.stl')
    print(f'SCAD     : {base}.scad')
    print()
    print(f'外形 {W} x {H} x {T} mm，四角 R{CORNER_R}')
    print(f'眼窗形状 EYE_SHAPE = {EYE_SHAPE!r}')
    for name, size, c in HOLE_INFO:
        print(f'  {name:8s} {size:34s} 中心 ({c[0]:.2f}, {c[1]:.2f})')
    print()
    print(f'眼窗圆心距 {EYES_X[1] - EYES_X[0]:.2f} mm '
          f'(= AA 中心距实测 32.0)')
    if EYE_SHAPE == 'teardrop':
        w = (EYE_TAB_W + EYE_TAB_CLEAR) / 2.0
        r = EYE_D / 2.0
        d = math.sqrt(max(r * r - w * w, 0.0))
        print(f'圆与窄槽交点在圆心下方 {d:.2f} mm '
              f'(规格坐标 Y={EYE_CIRC_CY + d:.2f})')
        print(f'窄槽 宽{EYE_TAB_W + EYE_TAB_CLEAR:.1f} x '
              f'长{(EYE_BBOX_BOT + EYE_BOT_CLEAR) - (EYE_CIRC_CY + d):.2f} mm')
        print('⚠️ EYE_TAB_W 必须是底部凸起的实测宽度，填小了装不进去')
    elif EYE_SHAPE == 'circle':
        print('⚠️ 仅当底部凸起在 Z 向明显低于玻璃面、可藏在盖板下时才可用')

    fit_ok = fit_check()
    min_clearances()
    mesh_ok = validate(tris)

    print()
    if fit_ok and mesh_ok:
        print('>>> 全部通过：可以下单打印')
    else:
        print('>>> 有问题，不要下单：'
              + ('装配校验未通过 ' if not fit_ok else '')
              + ('网格校验未通过' if not mesh_ok else ''))


if __name__ == '__main__':
    main()

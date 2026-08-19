#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VelaPet 验证壳（前板 / 对位量具）STL 生成器
==========================================

用途
----
生成可直接上传嘉立创 3D 打印下单的 STL（二进制）。
第一版只打「前板」，作用是三件事：
  1. 验证 4 个安装孔能否套上主板（孔距 57.00 × 47.00 mm）
  2. 通过 Φ21.5 放大开窗，量出两屏真实中心距 / 屏心相对安装孔的 X-Y 偏移
  3. 验证板框余量与外形比例

依赖
----
纯标准库（math / struct）。不需要 numpy / scipy / openscad / trimesh。
算法：多孔多边形 -> 桥接成单一简单多边形 -> ear clipping 三角化 -> 拉伸成实体。

尺寸来源
--------
velapet/VelaPet_3D打印外壳说明书.md 第 2.1 / 2.2 节（位号图矢量提取 + 实测）。

用法
----
    python3 gen_shell_stl.py [输出目录]
"""

import math
import os
import struct
import sys

# ============================================================
# 参数区（要改尺寸只改这里）
# ============================================================

PLATE_W = 66.0      # 前板宽 (X)  = 板框 62.9 + 双边约 1.5 余量
PLATE_H = 56.0      # 前板高 (Y)  = 板框 53.0 + 双边约 1.5 余量
PLATE_T = 2.0       # 板厚，等壁厚 2mm，满足嘉立创壁厚 >1.2mm

CORNER_R = 3.0      # 三个圆角半径
CHAMFER = 4.0       # 左下角 45° 切角，标记主板原点角（丝印 (2.90,3.04) 孔那侧）

SCREW_DX = 57.00    # 安装孔孔心间距 X（位号图提取，两层一致）
SCREW_DY = 47.00    # 安装孔孔心间距 Y
SCREW_D = 3.2       # 螺丝孔径：板上 Φ3.0 + 0.2 装配间隙

EYE_D = 21.5        # 眼窗直径：比玻璃 Φ20 放大 1.5mm，屏位偏 1mm 也不切边
EYE_PITCH = 29.5    # 两眼中心距（推定值，靠放大开窗吸收误差）

SEG_EYE = 96        # 眼窗圆周分段（弦高 ≈ 0.009mm，远优于打印精度）
SEG_SCREW = 48
SEG_ARC = 12        # 每个 90° 圆角的分段

EPS = 1e-9

# ============================================================
# 2D 轮廓
# ============================================================


def rounded_outline():
    """外轮廓，逆时针(CCW)。左下角为 45° 切角，其余三角为 R 圆角。"""
    w, h, r, c = PLATE_W, PLATE_H, CORNER_R, CHAMFER
    pts = [(c, 0.0)]

    def arc(cx, cy, a0, a1):
        for i in range(SEG_ARC + 1):
            a = math.radians(a0 + (a1 - a0) * i / SEG_ARC)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))

    pts.append((w - r, 0.0))
    arc(w - r, r, -90, 0)          # 右下
    pts.append((w, h - r))
    arc(w - r, h - r, 0, 90)       # 右上
    pts.append((r, h))
    arc(r, h - r, 90, 180)         # 左上
    pts.append((0.0, c))           # 切角起点 -> 自动闭合回 (c,0)
    return dedup(pts)


def circle(cx, cy, d, seg):
    """孔轮廓，顺时针(CW)。"""
    r = d / 2.0
    return [(cx + r * math.cos(-2 * math.pi * i / seg),
             cy + r * math.sin(-2 * math.pi * i / seg)) for i in range(seg)]


def dedup(pts):
    out = []
    for p in pts:
        if not out or (abs(p[0] - out[-1][0]) > 1e-7 or abs(p[1] - out[-1][1]) > 1e-7):
            out.append(p)
    if len(out) > 1 and abs(out[0][0] - out[-1][0]) < 1e-7 and abs(out[0][1] - out[-1][1]) < 1e-7:
        out.pop()
    return out


def signed_area(poly):
    s = 0.0
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        s += x0 * y1 - x1 * y0
    return s / 2.0


# ============================================================
# 桥接：把每个孔环接进外环，得到单一简单多边形
# ============================================================


def seg_cross(a, b, c, d):
    """判断线段 ab 与 cd 是否真交叉（共享端点不算）。"""
    for p in (a, b):
        for q in (c, d):
            if abs(p[0] - q[0]) < 1e-9 and abs(p[1] - q[1]) < 1e-9:
                return False

    def cr(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])

    d1, d2 = cr(c, d, a), cr(c, d, b)
    d3, d4 = cr(a, b, c), cr(a, b, d)
    if ((d1 > EPS and d2 < -EPS) or (d1 < -EPS and d2 > EPS)) and \
       ((d3 > EPS and d4 < -EPS) or (d3 < -EPS and d4 > EPS)):
        return True
    # 端点落在另一条线段上 -> 视为相交（保守）
    for (dv, p, s, e) in ((d1, a, c, d), (d2, b, c, d), (d3, c, a, b), (d4, d, a, b)):
        if abs(dv) < EPS and on_seg(s, e, p):
            return True
    return False


def on_seg(s, e, p):
    return (min(s[0], e[0]) - 1e-9 <= p[0] <= max(s[0], e[0]) + 1e-9 and
            min(s[1], e[1]) - 1e-9 <= p[1] <= max(s[1], e[1]) + 1e-9)


def ring_edges(ring):
    n = len(ring)
    return [(ring[i], ring[(i + 1) % n]) for i in range(n)]


def bridge_holes(outer, holes):
    """逐个把孔环桥接进主环。"""
    merged = list(outer)
    pending = [list(h) for h in holes]

    while pending:
        hole = pending.pop(0)
        # 取孔的最右点作为桥的一端
        m = max(range(len(hole)), key=lambda i: hole[i][0])
        src = hole[m]

        obstacles = ring_edges(merged) + [e for h in pending for e in ring_edges(h)] \
            + ring_edges(hole)

        order = sorted(range(len(merged)),
                       key=lambda i: (merged[i][0] - src[0]) ** 2 + (merged[i][1] - src[1]) ** 2)
        target = None
        for i in order:
            dst = merged[i]
            if any(seg_cross(src, dst, e[0], e[1]) for e in obstacles):
                continue
            target = i
            break
        if target is None:
            raise RuntimeError("桥接失败：找不到可见顶点")

        rot = hole[m:] + hole[:m] + [hole[m]]
        merged = merged[:target + 1] + rot + merged[target:]
    return merged


# ============================================================
# Ear clipping
# ============================================================


def point_in_tri(p, a, b, c):
    def cr(o, u, v):
        return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0])
    d1, d2, d3 = cr(a, b, p), cr(b, c, p), cr(c, a, p)
    return d1 >= -1e-12 and d2 >= -1e-12 and d3 >= -1e-12


def same(p, q):
    return abs(p[0] - q[0]) < 1e-9 and abs(p[1] - q[1]) < 1e-9


def earclip(poly):
    """poly 必须是 CCW 的简单多边形（可含桥接产生的重复顶点）。"""
    idx = list(range(len(poly)))
    tris = []
    guard = 0
    while len(idx) > 3:
        guard += 1
        if guard > 20 * len(poly) + 1000:
            raise RuntimeError("ear clipping 未收敛")
        found = False
        n = len(idx)
        for k in range(n):
            ia, ib, ic = idx[(k - 1) % n], idx[k], idx[(k + 1) % n]
            a, b, c = poly[ia], poly[ib], poly[ic]
            cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if cross <= 1e-10:           # 凹角或退化
                continue
            bad = False
            for j in idx:
                if j in (ia, ib, ic):
                    continue
                p = poly[j]
                if same(p, a) or same(p, b) or same(p, c):
                    continue
                if point_in_tri(p, a, b, c):
                    bad = True
                    break
            if bad:
                continue
            tris.append((a, b, c))
            idx.pop(k)
            found = True
            break
        if not found:
            # 退化兜底：切掉最"凸"的一个角
            best, bk = -1.0, None
            n = len(idx)
            for k in range(n):
                ia, ib, ic = idx[(k - 1) % n], idx[k], idx[(k + 1) % n]
                a, b, c = poly[ia], poly[ib], poly[ic]
                cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
                if cross > best:
                    best, bk = cross, k
            if bk is None or best <= 0:
                raise RuntimeError("ear clipping 卡死")
            ia, ib, ic = idx[(bk - 1) % n], idx[bk], idx[(bk + 1) % n]
            tris.append((poly[ia], poly[ib], poly[ic]))
            idx.pop(bk)
    tris.append((poly[idx[0]], poly[idx[1]], poly[idx[2]]))
    return tris


# ============================================================
# 拉伸成实体
# ============================================================


def extrude(outer, holes, t):
    faces = []
    merged = bridge_holes(outer, holes)
    if signed_area(merged) < 0:
        merged.reverse()
    tris = earclip(merged)

    for (a, b, c) in tris:                     # 顶面 +Z
        faces.append(((a[0], a[1], t), (b[0], b[1], t), (c[0], c[1], t)))
    for (a, b, c) in tris:                     # 底面 -Z（反向绕序）
        faces.append(((c[0], c[1], 0.0), (b[0], b[1], 0.0), (a[0], a[1], 0.0)))

    for ring in [outer] + list(holes):         # 侧壁
        n = len(ring)
        for i in range(n):
            p, q = ring[i], ring[(i + 1) % n]
            p0, p1 = (p[0], p[1], 0.0), (p[0], p[1], t)
            q0, q1 = (q[0], q[1], 0.0), (q[0], q[1], t)
            faces.append((p0, q0, q1))
            faces.append((p0, q1, p1))
    return faces


# ============================================================
# 校验 + 写文件
# ============================================================


def check(faces):
    """水密性检查：每条有向边恰好出现一次，且其反向边也恰好出现一次。"""
    edges = {}
    for tri in faces:
        for i in range(3):
            e = (rk(tri[i]), rk(tri[(i + 1) % 3]))
            edges[e] = edges.get(e, 0) + 1
    dup = [e for e, c in edges.items() if c != 1]
    open_e = [e for e in edges if (e[1], e[0]) not in edges]
    vol = 0.0
    for (a, b, c) in faces:
        vol += (a[0] * (b[1] * c[2] - c[1] * b[2])
                - a[1] * (b[0] * c[2] - c[0] * b[2])
                + a[2] * (b[0] * c[1] - c[0] * b[1])) / 6.0
    return dup, open_e, vol


def rk(p):
    return (round(p[0], 6), round(p[1], 6), round(p[2], 6))


def normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    return (0.0, 0.0, 0.0) if ln < 1e-12 else (nx / ln, ny / ln, nz / ln)


def write_svg(path, outer, holes, labels):
    """1:1 俯视预览图，下单前可用浏览器打开肉眼核对孔位。"""
    pad = 6.0
    w, h = PLATE_W + 2 * pad, PLATE_H + 2 * pad

    def d(ring):
        return "M " + " L ".join("%.4f,%.4f" % (x + pad, PLATE_H - y + pad)
                                 for x, y in ring) + " Z"

    parts = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<svg xmlns="http://www.w3.org/2000/svg" width="%.2fmm" height="%.2fmm" '
             'viewBox="0 0 %.4f %.4f">' % (w, h, w, h),
             '<rect width="100%%" height="100%%" fill="#fff"/>',
             '<path d="%s" fill="#cfe3f7" stroke="#1a4a7a" stroke-width="0.25"/>' % d(outer)]
    for ho in holes:
        parts.append('<path d="%s" fill="#fff" stroke="#c0392b" stroke-width="0.25"/>' % d(ho))
    for (x, y, txt) in labels:
        parts.append('<text x="%.3f" y="%.3f" font-size="2.2" text-anchor="middle" '
                     'fill="#333" font-family="sans-serif">%s</text>'
                     % (x + pad, PLATE_H - y + pad, txt))
    parts.append('</svg>')
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))


def write_stl(path, faces, title):
    with open(path, "wb") as f:
        f.write(title.encode("ascii", "replace")[:80].ljust(80, b"\0"))
        f.write(struct.pack("<I", len(faces)))
        for (a, b, c) in faces:
            f.write(struct.pack("<3f", *normal(a, b, c)))
            for p in (a, b, c):
                f.write(struct.pack("<3f", *p))
            f.write(struct.pack("<H", 0))


# ============================================================
# main
# ============================================================


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "3d")
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)

    cx, cy = PLATE_W / 2.0, PLATE_H / 2.0
    outer = rounded_outline()
    holes = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            holes.append(circle(cx + sx * SCREW_DX / 2.0, cy + sy * SCREW_DY / 2.0,
                                SCREW_D, SEG_SCREW))
    for sx in (-1, 1):
        holes.append(circle(cx + sx * EYE_PITCH / 2.0, cy, EYE_D, SEG_EYE))

    assert signed_area(outer) > 0, "外轮廓应为 CCW"
    for h in holes:
        assert signed_area(h) < 0, "孔轮廓应为 CW"

    faces = extrude(outer, holes, PLATE_T)
    dup, open_e, vol = check(faces)

    name = "VelaPet_验证壳_前板_v1.stl"
    path = os.path.join(outdir, name)
    write_stl(path, faces, "VelaPet validation front plate v1 - contest2026_098")

    svg = os.path.join(outdir, "VelaPet_验证壳_前板_v1_俯视图.svg")
    write_svg(svg, outer, holes, [
        (cx - EYE_PITCH / 2.0, cy, "L Φ%.1f" % EYE_D),
        (cx + EYE_PITCH / 2.0, cy, "R Φ%.1f" % EYE_D),
        (cx, cy + 8.0, "pitch %.1f" % EYE_PITCH),
        (cx, cy - 8.0, "%.0fx%.0fx%.0f" % (PLATE_W, PLATE_H, PLATE_T)),
        (cx, cy + PLATE_H / 2.0 - 26.0, "screw %.2f x %.2f  Φ%.1f" % (SCREW_DX, SCREW_DY, SCREW_D)),
    ])

    print("=" * 60)
    print("三角面数 : %d" % len(faces))
    print("重复有向边: %d   开放边: %d" % (len(dup), len(open_e)))
    print("水密     : %s" % ("OK (manifold)" if not dup and not open_e else "FAIL"))
    print("体积     : %.3f cm^3 (%.1f mm^3)" % (vol / 1000.0, vol))
    print("外形     : %.1f x %.1f x %.1f mm" % (PLATE_W, PLATE_H, PLATE_T))
    print("螺丝孔   : 4 x Φ%.1f @ %.2f x %.2f" % (SCREW_D, SCREW_DX, SCREW_DY))
    print("眼窗     : 2 x Φ%.1f, 中心距 %.1f" % (EYE_D, EYE_PITCH))
    print("切角     : 左下 %.1fmm 45° (对应主板原点角)" % CHAMFER)
    print("输出     : %s  (%d bytes)" % (path, os.path.getsize(path)))
    print("=" * 60)
    if dup or open_e:
        sys.exit(1)


if __name__ == "__main__":
    main()

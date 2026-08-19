#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Independent re-check of the generated STL by re-parsing the file itself."""
import struct, math, sys

path = sys.argv[1]
with open(path, 'rb') as f:
    header = f.read(80)
    (n,) = struct.unpack('<I', f.read(4))
    tris = []
    norms = []
    for _ in range(n):
        nx, ny, nz = struct.unpack('<3f', f.read(12))
        v = [struct.unpack('<3f', f.read(12)) for _ in range(3)]
        struct.unpack('<H', f.read(2))
        norms.append((nx, ny, nz))
        tris.append(tuple(v))
    rest = f.read()

print("header      :", header.rstrip(b'\0').decode('ascii', 'replace'))
print("facet count :", n, " parsed:", len(tris), " trailing bytes:", len(rest))

# bbox
xs = [p[0] for t in tris for p in t]
ys = [p[1] for t in tris for p in t]
zs = [p[2] for t in tris for p in t]
print("bbox        : X %.3f..%.3f (%.3f)  Y %.3f..%.3f (%.3f)  Z %.3f..%.3f (%.3f)" % (
    min(xs), max(xs), max(xs)-min(xs), min(ys), max(ys), max(ys)-min(ys),
    min(zs), max(zs), max(zs)-min(zs)))

# manifold check on rounded coords
def k(p): return (round(p[0], 4), round(p[1], 4), round(p[2], 4))
edges = {}
for t in tris:
    for i in range(3):
        e = (k(t[i]), k(t[(i+1) % 3]))
        edges[e] = edges.get(e, 0) + 1
bad_dup = sum(1 for c in edges.values() if c != 1)
bad_open = sum(1 for e in edges if (e[1], e[0]) not in edges)
print("manifold    : dup=%d open=%d -> %s" % (bad_dup, bad_open,
      "WATERTIGHT" if bad_dup == 0 and bad_open == 0 else "BROKEN"))

# degenerate / zero-area facets
deg = 0
for t in tris:
    ax, ay, az = t[0]; bx, by, bz = t[1]; cx, cy, cz = t[2]
    ux, uy, uz = bx-ax, by-ay, bz-az
    vx, vy, vz = cx-ax, cy-ay, cz-az
    nx, ny, nz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
    if math.sqrt(nx*nx+ny*ny+nz*nz)/2.0 < 1e-9:
        deg += 1
print("degenerate  :", deg)

# stored normal vs geometric normal agreement
worst = 0.0
for t, sn in zip(tris, norms):
    ax, ay, az = t[0]; bx, by, bz = t[1]; cx, cy, cz = t[2]
    ux, uy, uz = bx-ax, by-ay, bz-az
    vx, vy, vz = cx-ax, cy-ay, cz-az
    nx, ny, nz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
    L = math.sqrt(nx*nx+ny*ny+nz*nz)
    if L < 1e-12:
        continue
    dot = (nx/L)*sn[0] + (ny/L)*sn[1] + (nz/L)*sn[2]
    worst = max(worst, abs(1.0-dot))
print("normal error: max |1-dot| = %.2e" % worst)

# signed volume (positive => outward-facing winding)
vol = 0.0
for (a, b, c) in tris:
    vol += (a[0]*(b[1]*c[2]-c[1]*b[2]) - a[1]*(b[0]*c[2]-c[0]*b[2])
            + a[2]*(b[0]*c[1]-c[0]*b[1]))/6.0
print("volume      : %.3f cm^3 (sign %s)" % (vol/1000.0, "outward OK" if vol > 0 else "INVERTED"))

# --- geometry spot checks using the top face (z == max) triangles ---
T = max(zs)
top = [t for t in tris if all(abs(p[2]-T) < 1e-6 for p in t)]
print("top facets  :", len(top))

def inside_material(x, y):
    for (a, b, c) in top:
        d1 = (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0])
        d2 = (c[0]-b[0])*(y-b[1]) - (c[1]-b[1])*(x-b[0])
        d3 = (a[0]-c[0])*(y-c[1]) - (a[1]-c[1])*(x-c[0])
        if (d1 >= -1e-9 and d2 >= -1e-9 and d3 >= -1e-9) or \
           (d1 <= 1e-9 and d2 <= 1e-9 and d3 <= 1e-9):
            return True
    return False

W, H = max(xs)-min(xs), max(ys)-min(ys)
cx, cy = min(xs)+W/2, min(ys)+H/2
checks = []
# eye holes: centers must be void, ring just outside must be material
for sx in (-1, 1):
    ex, ey = cx + sx*29.5/2, cy
    checks.append(("eye %+d center void" % sx, not inside_material(ex, ey)))
    checks.append(("eye %+d r=10.0 void" % sx, not inside_material(ex+10.0, ey)))
    checks.append(("eye %+d r=11.5 solid" % sx, inside_material(ex+11.5, ey)))
# screw holes
for sx in (-1, 1):
    for sy in (-1, 1):
        hx, hy = cx + sx*57.0/2, cy + sy*47.0/2
        checks.append(("screw(%+d,%+d) void" % (sx, sy), not inside_material(hx, hy)))
        checks.append(("screw(%+d,%+d) r=1.3 void" % (sx, sy), not inside_material(hx+1.3, hy)))
        checks.append(("screw(%+d,%+d) r=2.2 solid" % (sx, sy), inside_material(hx+2.2, hy)))
# plate centre (between eyes) must be solid
checks.append(("plate centre solid", inside_material(cx, cy)))
# corner chamfer: 4mm cut at min corner -> point (0.5,0.5) must be void
checks.append(("chamfer corner void", not inside_material(min(xs)+0.5, min(ys)+0.5)))
checks.append(("opposite corner solid", inside_material(max(xs)-1.2, max(ys)-1.2)))

ok = True
for name, res in checks:
    print("  [%s] %s" % ("PASS" if res else "FAIL", name))
    ok = ok and res
print("spot checks :", "ALL PASS" if ok else "FAILURES PRESENT")
sys.exit(0 if (ok and bad_dup == 0 and bad_open == 0 and deg == 0 and vol > 0) else 1)

// VelaPet 前盖板 v3 — 2026-08-18 实测数据
// 眼窗/摄像头/咪头均为通孔：三处凸起都要穿出盖板（各高出底面 2.5mm）
$fn = 180;
W = 62.0; H = 52.0; T = 2.5; R = 2.0;

module rrect(w, h, r) {
  hull() for (sx = [-1, 1], sy = [-1, 1])
    translate([sx * (w / 2 - r), sy * (h / 2 - r)]) circle(r = r);
}

difference() {
  linear_extrude(height = T) rrect(W, H, R);
  translate([15, 24, -1]) linear_extrude(T + 2) rrect(21.0, 25.5, 10.5);
  translate([47, 24, -1]) linear_extrude(T + 2) rrect(21.0, 25.5, 10.5);
  translate([4, 48, -1]) cylinder(h = T + 2, r = 1.6);
  translate([58, 48, -1]) cylinder(h = T + 2, r = 1.6);
  translate([4, 4, -1]) cylinder(h = T + 2, r = 1.6);
  translate([58, 4, -1]) cylinder(h = T + 2, r = 1.6);
  translate([16, 44, -1]) linear_extrude(T + 2) rrect(8.5, 8.5, 1.0);
  translate([31, 6.75, -1]) cylinder(h = T + 2, r = 4);
}

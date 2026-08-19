// VelaPet 前盖板 v2 — 基于 2026-08-18 实测数据
// 规格坐标原点在左上角、Y 向下；本文件已翻转为 Y 向上。
$fn = 180;
W = 62.0; H = 52.0; T = 3.5; R = 2.0;

module plate() {
  hull() for (p = [[R,R],[W-R,R],[R,H-R],[W-R,H-R]])
    translate(p) circle(r = R);
}

difference() {
  linear_extrude(height = T) plate();
  translate([15, 25, -1]) cylinder(h = T + 2, r = 9.75);
  translate([47, 25, -1]) cylinder(h = T + 2, r = 9.75);
  translate([4, 48, -1]) cylinder(h = T + 2, r = 1.6);
  translate([58, 48, -1]) cylinder(h = T + 2, r = 1.6);
  translate([4, 4, -1]) cylinder(h = T + 2, r = 1.6);
  translate([58, 4, -1]) cylinder(h = T + 2, r = 1.6);
  translate([16, 44, -1]) cylinder(h = T + 2, r = 4.5);
  translate([31, 8.75, -1]) cylinder(h = T + 2, r = 1);
  translate([29.27, 5.75, -1]) cylinder(h = T + 2, r = 1);
  translate([32.73, 5.75, -1]) cylinder(h = T + 2, r = 1);
}

#!/usr/bin/env python3
# Strip BK flash 32+2 CRC layout: keep 32 data bytes, drop 2 CRC bytes, repeat.
import sys

src = sys.argv[1]
dst = sys.argv[2]
with open(src, "rb") as f:
    raw = f.read()

out = bytearray()
i = 0
n = len(raw)
while i < n:
    out += raw[i:i+32]
    i += 34  # skip the 2 CRC bytes
with open(dst, "wb") as f:
    f.write(out)

print(f"in={n} bytes ({n/1024/1024:.2f} MB) -> out={len(out)} bytes ({len(out)/1024/1024:.2f} MB)")
print(f"ratio={len(out)/n:.4f} (expect ~0.941 = 32/34)")

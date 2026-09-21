import sys
from collections import Counter

lbl, W, H, CW, CH = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
try:
    rec = open("bc250_hevc_debug_recon_i420.raw", "rb").read()
    dec = open(lbl + ".yuv", "rb").read()
except Exception as e:
    print("  %-22s no output (%s)" % (lbl, e)); sys.exit(0)

ry, dy = rec[:CW*CH], dec[:W*H]
bad = [(x, y, ry[y*CW+x], dy[y*W+x])
       for y in range(H) for x in range(W) if ry[y*CW+x] != dy[y*W+x]]

print("  %-22s luma %d/%d differ" % (lbl, len(bad), W*H), end="")
if not bad:
    print(); sys.exit(0)

xs = [p[0] for p in bad]; ys = [p[1] for p in bad]
mx = max(abs(p[2]-p[3]) for p in bad)
blocks = sorted({(p[0]//4*4, p[1]//4*4) for p in bad})
print("  max|d|=%d  x:%d-%d y:%d-%d  %d distinct 4x4 blocks"
      % (mx, min(xs), max(xs), min(ys), max(ys), len(blocks)))
print("      first blocks (x,y):", blocks[:8])
sub = Counter(((bx % 8)//4, (by % 8)//4) for bx, by in blocks)
cu  = Counter(((bx % 16)//8, (by % 16)//8) for bx, by in blocks)
print("      4x4 position within its 8x8 CU (x,y):", dict(sub))
print("      8x8 CU position within its CTU (x,y):", dict(cu))

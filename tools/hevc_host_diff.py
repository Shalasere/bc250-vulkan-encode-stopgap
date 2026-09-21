# Exit status is the whole point of this script: hevc_host_drift.sh keys
# `fail=1` off it, and a CI gate keys off that. It used to exit 0 on every
# path - including after printing a full drift report - so the oracle
# printed "2026871/2073600 differ" and then "HOST DRIFT PASS". That is the
# same shape as the bare `ffmpeg ... -f null -` + unconditional "ZERO
# errors!" echo that kept .github/workflows/build.yml green over an
# undecodable H.264 stream. Every failure path below must exit non-zero.
import sys
from collections import Counter

lbl, W, H, CW, CH = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
try:
    rec = open("bc250_hevc_debug_recon_i420.raw", "rb").read()
    dec = open(lbl + ".yuv", "rb").read()
except Exception as e:
    # Missing recon or missing decode is a failure, not a skip. A decoder
    # that refused the bitstream outright produces no .yuv, which is the
    # worst drift there is, and this path used to report it as a pass.
    print("  %-22s no output (%s)" % (lbl, e)); sys.exit(1)

# The decoded frame must be the size that was ASKED for. Without this the
# oracle is blind to exactly the bug b2b7fef fixed in H.264, where 1920x1080
# was emitted with a crop window that decoded as 1920x1088: the comparison
# below slices dec[:W*H], which for a too-TALL decode is still the correct
# top W*H bytes, so a wrong conformance window scores zero drift. Compare
# the whole file - one frame, planar 4:2:0, at the requested size.
want = W*H + 2*((W//2)*(H//2))
if len(dec) != want:
    print("  %-22s decoded %d bytes, expected %d (%dx%d): the stream does not "
          "decode at the size it was asked for" % (lbl, len(dec), want, W, H))
    sys.exit(1)
if len(rec) < CW*CH + 2*((CW//2)*(CH//2)):
    print("  %-22s recon dump short (%d bytes)" % (lbl, len(rec))); sys.exit(1)

# Crop a plane out of the padded recon dump, one row-slice at a time. The
# per-pixel comparison below is ~40x slower than this in CPython and the
# oracle passes far more often than it fails, so the equality test is done
# in bulk and the pixel-by-pixel walk only runs to explain a failure. That
# is worth about 2 s of the CI job at 1080p.
def crop(buf, base, stride, w, h):
    return b"".join(buf[base+y*stride:base+y*stride+w] for y in range(h))

ry, dy = crop(rec, 0, CW, W, H), dec[:W*H]

# Chroma too. Padding and crop arithmetic is done a second time, in chroma
# units and at half resolution, so a rounding error can miss luma entirely -
# and a luma-only oracle would never see it. (It is also where the Table
# 8-10 chroma-QP defect lived.)
cw2, ch2, w2, h2 = CW//2, CH//2, W//2, H//2
cplanes = [(crop(rec, rbase, cw2, w2, h2), dec[dbase:dbase+w2*h2])
           for rbase, dbase in ((CW*CH, W*H), (CW*CH + cw2*ch2, W*H + w2*h2))]
cbad = 0 if all(rp == dp for rp, dp in cplanes) else \
       sum(a != b for rp, dp in cplanes for a, b in zip(rp, dp))

if ry == dy and not cbad:
    print("  %-22s luma 0/%d differ" % (lbl, W*H)); sys.exit(0)

bad = [(i % W, i // W, ry[i], dy[i]) for i in range(W*H) if ry[i] != dy[i]]
print("  %-22s luma %d/%d differ" % (lbl, len(bad), W*H), end="")
if cbad:
    print("  chroma %d/%d differ" % (cbad, 2*w2*h2), end="")
if not bad:
    print(); sys.exit(1)

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
sys.exit(1)

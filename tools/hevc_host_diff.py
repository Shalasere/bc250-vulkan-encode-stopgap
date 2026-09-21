# Exit status is the whole point of this script: hevc_host_drift.sh keys
# `fail=1` off it, and a CI gate keys off that. It used to exit 0 on every
# path - including after printing a full drift report - so the oracle
# printed "2026871/2073600 differ" and then "HOST DRIFT PASS". That is the
# same shape as the bare `ffmpeg ... -f null -` + unconditional "ZERO
# errors!" echo that kept .github/workflows/build.yml green over an
# undecodable H.264 stream. Every failure path below must exit non-zero.
#
# usage: hevc_host_diff.py <label> <w> <h> <coded_w> <coded_h> [frames]
#
# The encoder's recon dump (bc250_hevc_debug_recon_i420.raw) is planar I420
# at CODED dimensions, one frame appended after another. The decoder's .yuv
# is planar I420 at DISPLAY dimensions, same frame order. With `frames` > 1
# every frame is compared, and the FIRST differing frame is reported in
# full - that is the one that localises an inter bug, because everything
# after it is downstream of a poisoned reference picture.
import sys
from collections import Counter

lbl, W, H, CW, CH = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
NF = int(sys.argv[6]) if len(sys.argv) > 6 else 1

try:
    rec = open("bc250_hevc_debug_recon_i420.raw", "rb").read()
    dec = open(lbl + ".yuv", "rb").read()
except Exception as e:
    # Missing recon or missing decode is a failure, not a skip. A decoder
    # that refused the bitstream outright produces no .yuv, which is the
    # worst drift there is, and this path used to report it as a pass.
    print("  %-26s no output (%s)" % (lbl, e)); sys.exit(1)

rec_frame = CW * CH * 3 // 2
dec_frame = W * H * 3 // 2

# A short file is drift too: the decoder dropped frames the encoder emitted.
if len(rec) < rec_frame * NF or len(dec) < dec_frame * NF:
    print("  %-26s truncated: recon %d/%d frames, decoded %d/%d frames"
          % (lbl, len(rec) // rec_frame, NF, len(dec) // dec_frame, NF))
    sys.exit(1)

# The decode must be EXACTLY the size that was asked for. Without this the
# oracle is blind to exactly the bug b2b7fef fixed in H.264, where 1920x1080
# was emitted with a crop window that decoded as 1920x1088: every comparison
# below indexes the decoded frame at the requested stride, and for a
# too-TALL decode the first frame's bytes are still the correct top rows, so
# a wrong conformance window scores zero drift. A too-tall decode also
# silently shifts frame k>0, which would then read as an inter bug.
if len(dec) != dec_frame * NF:
    print("  %-26s decoded %d bytes, expected %d (%d x %dx%d): the stream does "
          "not decode at the size it was asked for"
          % (lbl, len(dec), dec_frame * NF, NF, W, H))
    sys.exit(1)


def plane_diff(rbuf, roff, rstride, dbuf, doff, dstride, w, h):
    # Bulk-compare first. The per-pixel walk below is ~40x slower in CPython
    # and this oracle passes far more often than it fails, so the pixel
    # coordinates are only reconstructed to explain a failure. Worth about
    # 2 s of the CI job at 1080p, and more once NF > 1.
    if all(rbuf[roff + y * rstride:roff + y * rstride + w] ==
           dbuf[doff + y * dstride:doff + y * dstride + w] for y in range(h)):
        return []
    return [(x, y, rbuf[roff + y * rstride + x], dbuf[doff + y * dstride + x])
            for y in range(h) for x in range(w)
            if rbuf[roff + y * rstride + x] != dbuf[doff + y * dstride + x]]


first_bad = None
summary = []
for k in range(NF):
    rb, db = k * rec_frame, k * dec_frame
    bad = plane_diff(rec, rb, CW, dec, db, W, W, H)
    # Chroma: Cb then Cr, both at half dimensions. Inter SKIP copies chroma
    # from the reference at dx/2, so a chroma-only inter defect is a real
    # possibility and luma alone would not see it.
    cw, ch, dcw, dch = CW // 2, CH // 2, W // 2, H // 2
    rcb, rcr = rb + CW * CH, rb + CW * CH + cw * ch
    dcb, dcr = db + W * H, db + W * H + dcw * dch
    bcb = plane_diff(rec, rcb, cw, dec, dcb, dcw, dcw, dch)
    bcr = plane_diff(rec, rcr, cw, dec, dcr, dcw, dcw, dch)
    summary.append((k, len(bad), len(bcb) + len(bcr)))
    if (bad or bcb or bcr) and first_bad is None:
        first_bad = (k, bad, bcb, bcr)

tot_l = sum(s[1] for s in summary)
tot_c = sum(s[2] for s in summary)
print("  %-26s luma %d/%d differ, chroma %d/%d differ"
      % (lbl, tot_l, W * H * NF, tot_c, (W // 2) * (H // 2) * 2 * NF), end="")
if first_bad is None:
    print()
    sys.exit(0)
print()

k, bad, bcb, bcr = first_bad
print("      first bad frame: %d of %d  (luma %d, chroma %d)" % (k, NF, len(bad), len(bcb) + len(bcr)))
if NF > 1:
    print("      per-frame luma:", ", ".join("f%d=%d" % (s[0], s[1]) for s in summary))
if bad:
    xs = [p[0] for p in bad]; ys = [p[1] for p in bad]
    mx = max(abs(p[2] - p[3]) for p in bad)
    blocks = sorted({(p[0] // 4 * 4, p[1] // 4 * 4) for p in bad})
    print("      luma max|d|=%d  x:%d-%d y:%d-%d  %d distinct 4x4 blocks"
          % (mx, min(xs), max(xs), min(ys), max(ys), len(blocks)))
    print("      first blocks (x,y):", blocks[:8])
    sub = Counter(((bx % 8) // 4, (by % 8) // 4) for bx, by in blocks)
    cu = Counter(((bx % 16) // 8, (by % 16) // 8) for bx, by in blocks)
    print("      4x4 position within its 8x8 CU (x,y):", dict(sub))
    print("      8x8 CU position within its CTU (x,y):", dict(cu))
for name, b in (("cb", bcb), ("cr", bcr)):
    if b:
        mx = max(abs(p[2] - p[3]) for p in b)
        print("      %s max|d|=%d  first: %s" % (name, mx, b[:6]))
sys.exit(1)

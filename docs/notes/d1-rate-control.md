# D1: rate control does not spend a higher bitrate

Status: **root-caused and fixed off-board, board re-measure still required.**

## The claim being investigated

`docs/backlog.md` D1, board run 2026-09-21, 2560x1440 `testsrc2`, 15M vs 31M:

| encoder | 15M | 31M | delta |
|---|---|---|---|
| ours, H.264 | 42.61 dB (qp_avg 27.9) | 43.09 dB (qp_avg 27.5) | +0.48 |
| libx264 | 41.60 dB | 47.91 dB | +6.31 |
| ours, HEVC CPU | 44.56 dB | 44.59 dB | +0.03 |
| ours, HEVC GPU | 42.81 dB | 43.27 dB | +0.46 |

D1 explicitly rules out `qp_min=12` (qp_avg is ~27-28, nowhere near the
floor) and asks why the controller doesn't drive QP down when the bucket
has room.

## Mechanism, from the code

`rate_control.c` has two entry points that touch `rc.base_qp`/`rc.current_qp`:

- `rc_init()` (reached via `h264_encoder_set_bitrate()` /
  `hevc_encoder_set_bitrate()`) - computes `base_qp` from the requested
  bitrate via `rc_estimate_base_qp()`'s log-bpp model, and **guards against
  reinitializing when the bitrate hasn't actually changed**
  (`h264_encoder_set_bitrate()`'s own comment already flagged this exact
  class of risk: "never confirmed whether a real VA-API caller resends one
  of those buffers with an unchanged value every frame").
- `h264_encoder_set_qp()` / `hevc_encoder_set_qp()` (reached from
  `va_backend.c`'s `bc250_RenderPicture()`, wired to
  `VAEncPictureParameterBufferH264/HEVC.pic_init_qp` and
  `VAEncMiscParameterRateControl.initial_qp`) - **had no such guard**. Every
  call unconditionally overwrote *both* `rc.base_qp` and `rc.current_qp`
  with the caller's hint, full stop.

`VAEncPictureParameterBufferH264/HEVC` is a mandatory **per-frame** VA-API
buffer (it carries `frame_num`/`poc`/reference lists, so it cannot be
reused across frames). If a real caller sends a nonzero `pic_init_qp` (or
`VAEncMiscParameterRateControl.initial_qp`) - which `docs/DEVLOG.md`'s own
§10.7 already documents Sunshine's real negotiation doing - that call
happens on **every single frame** of the session, with a value that
`rc_estimate_base_qp()` never computed and that does not track the
requested bitrate the way our own log-bpp model does.

Effect on `rc_get_frame_qp()`: `base_qp` (the value the frame walks toward)
gets snapped back to the caller's fixed hint before the P/I feedback loop
ever accumulates more than one frame's `+-2`/`+-3` step. The buffer-error
terms (`p_term`, `i_term`) still run every frame, but they're now walking
around a *fixed, bitrate-insensitive* anchor instead of the anchor
`rc_estimate_base_qp()` picked for the real target - so the mean QP tracks
the caller's hint almost exactly (see the numbers below: `stomp=27`
produces `qp_mean` within ~2 of 27 at *both* 15M and 31M) and barely moves
between bitrates, regardless of what the buffer-fullness math wants to do.

This is the asymmetry the fix closes: give `*_encoder_set_qp()` the same
"did this actually change" guard `*_encoder_set_bitrate()` already has. A
resent, unchanged hint (the ordinary, spec-sanctioned meaning of "here is
the seed, still") becomes a no-op, and the P/I loop is left alone to keep
walking frame to frame - exactly like a resent, unchanged bitrate already
was. A genuinely new hint (a real CQP QP change, or a session that
actually renegotiates) still applies immediately.

## Reproducing off-board, before touching anything

`hevc_encoder_encode_raw()` is a real, full CPU intra/CABAC path with no
GPU dependency - `tools/hevc_host_drift.sh` and the rest of this project
already rely on that. **`h264_encoder_encode_raw()` is not the H.264
equivalent of that claim**: it is documented (`docs/performance-
measurement.md`, "Measuring H.264 CAVLC without a board") and confirmed by
reading it (`encoder_h264.c`, `h264_encoder_encode_raw()`) to code **no
residual at all** - every IDR macroblock is an Intra16x16 DC-only block
with all-zero coefficients, every P macroblock is SKIP or a bare P16x16
header, regardless of QP. That means H.264's raw path can validate the
*rate-control/QP-walk mechanism* (which is codec-agnostic - the same
`va_backend.c` call pattern and the same `rate_control.c` state machine
back both codecs) but **cannot** produce a meaningful PSNR/bits-per-frame
quality number: bytes/frame from this path is ~1896 regardless of QP or
bitrate, because there is no content-dependent payload to spend bits on.
HEVC's raw path has no such limitation and is the one used for the
quality claims below.

Built `tools/rc_bench.c` (new CMake target `rc_bench`, real `-O3
-march=znver2` codegen, links the real `rate_control.c`/`encoder_h264.c`/
`encoder_h265.c` - see `approach1-compute-encoder/CMakeLists.txt`). It
drives `h264_encoder_encode_raw()`/`hevc_encoder_encode_raw()` directly (no
VA-API, no ffmpeg, no GPU, no board) against **real** `testsrc2` content -
generated once via `ffmpeg -f lavfi -i testsrc2=size=2560x1440:rate=60
-frames:v 150 -pix_fmt nv12 -f rawvideo` to match D1's board content
exactly - fed in via `--input=`. `BC250_RC_NOMINAL_DRAIN=1` is set
internally so the leaky bucket drains at the fixed per-frame quota that a
real, perfectly-scheduled 60fps session would produce, rather than by this
harness's own (much faster, and therefore misleading) loop speed - see
`docs/performance-measurement.md`'s "second, CPU-only way to lose
byte-exactness" and `tests/test_encode.c`'s identical use of the same
env var.

Two modes: `sweep` (create at bitrate, `set_rc_mode(RC_VBR)`, encode - no
external QP override at all) and `--stomp=N` (additionally calls
`*_encoder_set_qp(N)` before every frame, simulating exactly what
`va_backend.c` does when a real caller's per-frame `pic_init_qp`/
`initial_qp` is nonzero and constant).

### HEVC, 2560x1440, real testsrc2, 150 frames, gop=120

Mean QP and mean bytes/frame, `rc_bench hevc --input=<testsrc2.nv12>
--bitrates=15000000,31000000`:

| scenario | 15M qp\_mean | 31M qp\_mean | d\_qp | 15M bytes/frame | 31M bytes/frame |
|---|---|---|---|---|---|
| `sweep` (no stomp) | 43.43 | 31.96 | **-11.47** | 30497 | 64276 |
| `--stomp=27` **before fix** | 28.97 | 28.87 | **-0.11** | 76328 | 76878 |
| `--stomp=27` **after fix** | 43.43 | 31.96 | **-11.47** | 30497 | 64276 |

`sweep` alone proves `rate_control.c`'s own P/I feedback + `rc_estimate_
base_qp()`, driven with nothing else touching it, responds *strongly* to a
2x bitrate change - an 11.5 QP swing, not a flat line. The **pre-fix**
`--stomp=27` run reproduces D1's flat pattern almost exactly by itself,
with nothing else changed. The **post-fix** `--stomp=27` run is now
byte-for-byte identical to `sweep`: once the guard is in place, a resent,
unchanged QP hint every frame is a no-op, so the simulated VA-API traffic
no longer disturbs the real rate controller after frame 0.

### PSNR (the actual claim), HEVC CPU path, same content/bitrates

Encoded via `rc_bench hevc --input=<testsrc2.nv12> --stomp=27 --out=...`
(the realistic scenario - a per-frame QP hint present, as a real client's
negotiation produces), decoded with ffmpeg to raw NV12, compared against
the **same already-raw** source file with forced `-f rawvideo -s 2560x1440
-r 60` framing on both sides (never comparing against a freshly regenerated
`-f lavfi` source - see `CLAUDE.md`'s PSNR rule):

| | 15M | 31M | delta |
|---|---|---|---|
| **before fix** | 44.39 dB | 44.49 dB | **+0.10 dB** |
| **after fix** | 35.39 dB | 42.40 dB | **+7.00 dB** |

The "before fix" row is a near-exact match to D1's own board figures for
this path (44.56 -> 44.59 dB, +0.03 dB) - both the absolute level and the
flatness reproduce off-board, from nothing but the QP-hint-stomping
mechanism above, on totally different hardware. The "after fix" row shows
a real, substantial, libx264-shaped response (D1's libx264 comparison
point for the same 2x change was +6.31 dB) - once the stomping stops, the
rate controller answers a higher budget by actually lowering QP and
spending more bits on real detail, which is what the buffer-fullness math
was already trying to do the whole time.

### H.264 (mechanism-only evidence, quality evidence needs the board/GPU path)

Same harness, `rc_bench h264 ... `. Bytes/frame is ~1896 in every cell
below regardless of scenario or bitrate - the expected, uninteresting
consequence of `encode_raw()` coding no residual (see above), not a rate-
control result.

| scenario | 15M qp\_mean | 31M qp\_mean | d\_qp |
|---|---|---|---|
| `sweep` (no stomp) | 13.03 | 12.07 | -0.96 |
| `--stomp=27` before fix | 25.07 | 25.07 | -0.01 |
| `--stomp=27` after fix | 14.59 | 14.51 | -0.07 |

Two things this does show, honestly:

1. **The same stomping mechanism reproduces for H.264 too** - pre-fix
   `--stomp=27` pins qp_mean to ~25 at both bitrates (matching D1's ~27-28
   qp_avg far better than `sweep`'s own ~12-13 does), and the fix visibly
   loosens it (qp_mean moves toward `sweep`'s floor-pinned values, though
   not all the way to them within one frame - see the note below).
2. `sweep`'s own qp_mean (~12-13, essentially at `qp_min`) does **not**
   match D1's observed ~27-28 qp_avg at all. That mismatch is itself
   evidence that the real board session was never running "pure" rate
   control in the first place - something was holding QP up near a fixed
   ~26-28 regardless of the bitrate-driven target, consistent with the
   stomping mechanism and inconsistent with `rate_control.c`'s own
   bitrate-driven math (which, left alone on this content, wants to run
   near the floor - see `CLAUDE.md`'s qp_min=12 note, §18, which documents
   exactly this "real desktop content pins at the floor with bitrate to
   spare" behavior as pre-existing and deliberate).

Why `--stomp=27` after the fix doesn't land exactly on `sweep`'s numbers
for H.264 the way it does for HEVC: `qp_hint_applied` starts at the `-1`
sentinel, so frame 0's stomp call still applies (nothing to compare
against yet) - one frame's difference out of 150 measurably nudges a
15/31-frame-average qp_mean when the per-frame walk is `+-2`, but has no
effect on the *shape* of the fix (from frame 1 onward the guard is
identical in both codecs).

**What still needs the board (or a working off-board GPU/VA-API stack) to
confirm for H.264**: a real PSNR delta at 15M vs 31M through
`h264_encoder_encode_frame()`'s GPU residual path (the only H.264 path that
actually codes content-dependent bits), or through Sunshine/`lab qsweep`
itself with this fix applied. The mechanism fix is code-identical in shape
between the two codecs and directly demonstrated on HEVC; there is no
reason to expect it behaves differently for H.264's real (GPU) path, but
that is an expectation, not a measurement - `encode_raw()` cannot supply
the measurement for this codec.

## The fix

`approach1-compute-encoder/src/encoder_h264.c` (`h264_encoder_set_qp()`)
and `approach1-compute-encoder/src/encoder_h265.c`
(`hevc_encoder_set_qp()`): added a `qp_hint_applied` field to each encoder
struct (distinct from the RC's own live QP, which both `pick_frame_qp()`
and `rc_get_frame_qp()` overwrite every frame - reusing that field as the
"was this explicit" flag would have made the guard a no-op almost every
frame). `rc.base_qp`/`rc.current_qp` are now only reset when the incoming
QP hint differs from the last one applied, exactly mirroring
`*_encoder_set_bitrate()`'s existing "did this actually change" guard.
Every other field these functions touch (`pps.pic_init_qp`, `encoder->qp`)
is still updated unconditionally, because those need to reflect the
caller's latest request regardless (SPS/PPS's `pic_init_qp` reference
value, and CQP mode's actual per-frame QP for HEVC, which reads
`encoder->qp` directly rather than `rc.current_qp`).

`qp_min = 12` was **not** touched, per the task's standing instruction -
this mechanism has nothing to do with the floor; qp_avg in both D1's board
run and this investigation's reproductions sits nowhere near it.

## Correctness

- `tools/hevc_host_drift.sh`: **HOST DRIFT PASS (38 cases byte-exact)**,
  including the three VBR cases (`n8g8_vbr400`, `n8g8_mv2_0_vbr2000`,
  `n8g8_vbr4000`) that exercise `RC_VBR` end-to-end. Unaffected, as
  expected: that harness calls `*_encoder_set_qp()` at most once per
  encoder instance (never per-frame), so the guard is a no-op there either
  way - same value in, same value out, whether it's re-applied or skipped.
- `ctest -R 'BitstreamTest|EncodeBitstreamTest|HevcEncodeBitstreamTest'`:
  **3/3 pass** (includes `test_rate_control_cqp_and_vbr()`, which is part
  of `EncodeBitstreamTest`).
- `VaApiDriverTest` fails in this environment (llvmpipe/WSL software
  Vulkan, `VK_ERROR_OUT_OF_HOST_MEMORY` on a real image allocation before
  any encoder code runs) - this is a pre-existing off-board environment
  limitation unrelated to this change (confirmed: the failure is inside
  `gpu_compute.c`'s image allocation, nothing this fix touches), not a
  regression, and not one of the checks this task required.
- This is a genuine compression/operating-point change, not a byte-
  identical one, by design (D1 is exactly the complaint that the operating
  point was *wrong* before). The byte-exactness oracle above is the
  correct one to run precisely because it is scoped to the CQP paths this
  change does not touch - see `docs/backlog.md`'s standing rule
  "Byte-identical means byte-identical. A performance change that alters
  output is a correctness change and needs a different review," which is
  why the evidence for the actual claim is the PSNR/bits/QP data above,
  not a diff.

## What the board still needs to confirm

1. **Re-run `docs/backlog.md` D1's own scoreboard**
   (`tools/lab qsweep --codec=h264` and `--codec=hevc`, 15M/31M/etc,
   2560x1440 `testsrc2`) with this fix, on real hardware, through the real
   VA-API/ffmpeg path this investigation could not run off-board. The
   off-board numbers above predict the flat-to-responsive shift should
   reproduce for both HEVC paths (CPU and GPU, since GPU intra shares the
   identical `va_backend.c` call pattern and `rate_control.c` state
   machine, just a different encode core) and for H.264's rate-control
   *mechanism*, but only the board can supply H.264's real PSNR number
   (see "What still needs the board" above).
2. **Confirm the real caller actually sends a per-frame, roughly-constant
   `pic_init_qp`/`initial_qp`** - this investigation inferred that from
   (a) `docs/DEVLOG.md` §10.7 documenting Sunshine's real negotiation
   setting a nonzero `pic_init_qp`, (b) the board's own qp_avg (~27-28)
   matching a fixed-hint simulation far better than it matches this
   file's `sweep` numbers, and (c) the pre-fix `--stomp=27` PSNR
   reproduction landing almost exactly on D1's board figures - but no
   off-board environment here could actually run ffmpeg through this
   driver's real VA-API surface to watch `BC250_DEBUG_RC=1`'s
   `RateControl:`/`MiscParam:` lines fire frame by frame and confirm it
   directly. That log is cheap to capture on the next real board/Sunshine
   session and would close the one remaining inferential step in this
   diagnosis.
3. HEVC GPU's own D1 numbers (42.81 -> 43.27 dB, +0.46) were not
   separately reproduced here - the GPU intra encode path
   (`hevc_encoder_encode_frame()`) needs a real GPU surface and cannot run
   off-board, and there is no reason from the code to expect its
   `va_backend.c`/`rate_control.c` plumbing (identical to the CPU path's)
   behaves differently, but that is again an expectation, not this
   investigation's own measurement.

## Tooling added

- `tools/rc_bench.c` + `rc_bench` CMake target
  (`approach1-compute-encoder/CMakeLists.txt`): off-board rate-control
  response harness, `sweep`/`--stomp=N` modes, `--input=<nv12>` for real
  captured content, `--out=<prefix>` to dump full bitstreams for an
  external PSNR check. See its own top comment for the full rationale.

## Drive-by cleanup

Several `.c`/`.h` comments still cited `docs/rate_control_audit.md`, which
no longer exists in this tree (its content was folded into `rate_control.c`'s
own comments and into `docs/DEVLOG.md`, per this task's brief). Repointed
those references at the relevant `docs/DEVLOG.md` sections (or removed the
dead section numbering where the surrounding comment already restates the
content) in `encoder_h264.h`, `encoder_h264.c`, `rate_control.c`,
`va_backend.h`, `va_backend.c`. Also corrected one comment in
`encoder_h264.h` (`h264_encoder_set_cbr_intent()`'s doc comment) that
claimed "this driver still hardcodes rate_control_t.mode to RC_CBR
everywhere and never actually negotiates VA_RC_VBR from the VAConfig" -
that was true when written but is contradicted by the current
`bc250_CreateContext()` (`va_backend.c`), which has since been wired to
route `VAConfigAttribRateControl` to `RC_CQP`/`RC_VBR`/`RC_LOW_LATENCY`
(`docs/DEVLOG.md` §26.7, §28). `docs/DEVLOG.md` itself is left untouched
(three references there) per this task's instruction not to edit it.

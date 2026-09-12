# BC-250 VCN Driver — Rate Control Accuracy Audit

**Scope**: does the encoder's actual output bitrate track the bitrate an
application requests (`-b:v X`)? Measurement/diagnosis only — no source
files were changed. Correctness (image quality) and throughput (fps) are
covered elsewhere in `docs/DEVLOG.md`; this document is specifically about
bitrate targeting, which had never been measured before this audit.

**Hardware under test**: physical AMD BC-250 (`user@10.0.0.104`), same
board as every other measurement in this repo. Driver built fresh from this
worktree (`audit/rate-control-accuracy`, forked from `main` at `ae05125`)
in an isolated build directory on the board (`~/bc250-ratecontrol-audit`,
untouched by other concurrent worktrees). ffmpeg 7.1.3 / libva 1.23,
`h264_vaapi` encoder, `testsrc`/`testsrc2`/`noise`/`smptebars` lavfi
sources, real encoded `.mp4` output measured with `wc -c` + `ffprobe
-show_entries stream=duration`.

**Bottom line**: rate control does **not** reliably hit a requested
bitrate. It is a real, wired-up proportional feedback loop — not a
"fixed QP, never adjusted" stub — but its QP adjustment range is
mathematically clamped to a narrow band (~±6-8 QP steps) around a
**hardcoded** QP of 26, regardless of what bitrate is requested. Since
QP-to-bitrate is roughly exponential, ±6-8 QP only covers roughly a 2-4×
range of achievable bitrate for any given content — nowhere near enough to
span "1 Mbps vs 20 Mbps" requests. Outside that narrow reachable band, the
requested bitrate has essentially no effect: measured output bitrate for
the same content varies by <5% across a 20× spread of requested bitrates,
while switching content complexity at a *fixed* requested bitrate swings
output bitrate by >6×. There is also a second, independent, smaller bug:
`VAEncMiscParameterRateControl.target_percentage` is never read, so under
ffmpeg's actual default VAAPI rate-control mode (VBR, not CBR) the driver
is handed the wrong absolute target to begin with.

---

## 1. How the algorithm actually works (it is real, not a stub)

`approach1-compute-encoder/src/rate_control.c` implements a genuine
per-frame proportional feedback loop around a leaky-bucket buffer model —
this is *not* the naive "derive one QP from the bitrate at init and never
touch it again" pattern the task brief raised as a possibility:

- `rc_init()` (rate_control.c:13-39) sets up `target_bitrate`,
  `target_bits_per_frame = target_bitrate / framerate`, and a buffer
  (`buffer_size` = 1 second of `target_bitrate` bits for CBR/VBR, or 2
  frames for `RC_LOW_LATENCY`), initialized half-full. It also sets
  `base_qp = 26` and `current_qp = 26` — **unconditionally, regardless of
  what `bitrate` was passed in.**
- `rc_get_frame_qp()` (rate_control.c:41-74), called once per frame from
  `encoder_h264.c:1033` and `:1522`, computes how far `buffer_fullness` has
  drifted from its 50%-full target level, turns that into a proportional
  QP adjustment (`p_term = error/target_level * 6.0`), steps `current_qp`
  toward `base_qp + qp_adjust` by at most `max_step` (2 for CBR/VBR, 3 for
  `RC_LOW_LATENCY`) per frame, and clamps to `[qp_min=12, qp_max=51]`.
- `rc_update_stats()` (rate_control.c:76-87), called after each frame is
  actually encoded (`encoder_h264.c:1485`, `:1733`) with the real coded
  size in bits, updates `buffer_fullness` by `+= bits_used -
  target_bits_per_frame`, clamped to `[0, buffer_size]`.

This is architecturally correct: it is proportional (not constant), it
sees real coded output size (`bits_used`), and it converges — the bug is
in what it converges *to*, not whether it does anything at all.

### The dynamic range is capped to a fixed ±6 QP band, independent of the target bitrate

`error` in `rc_get_frame_qp()` is bounded to `[-target_level,
target_level]` because `buffer_fullness` is clamped to `[0, buffer_size]`
and `target_level = buffer_size/2`. `p_term = error/target_level * 6.0` is
therefore a **normalized, dimensionless ratio** — its magnitude is bounded
to exactly `6.0` no matter what `target_bitrate`, `buffer_size`, or
resolution is in play. `qp_adjust = round(p_term)` is thus bounded to
`[-6, 6]` in `RC_CBR` (the only mode ever actually used — see §2), so
`current_qp` converges to `base_qp ± 6 = 26 ± 6`, i.e. **QP ∈ [20, 32]
forever, for any requested bitrate.** The full codec range advertised by
the struct (`qp_min=12`, `qp_max=51`, rate_control.h:23-24) is
structurally unreachable through this feedback path; only ~24% of it
(20/39 steps) is ever used, and it is always the same 24% centered on a
constant that has nothing to do with the request.

`base_qp` is the crux: it is the one number that should encode "what QP
roughly hits this target bitrate for this resolution," and it is a literal
constant (`26`, rate_control.c:35) set once in `rc_init()` and never
touched again — not derived from `target_bitrate`, `target_bits_per_frame`,
width×height, or anything else. The proportional term can only nudge
*around* that fixed point; it cannot relocate the fixed point itself.

This exact limitation is already flagged, narrowly, in an existing code
comment that this audit's measurements now connect to bitrate accuracy
specifically — `encoder_h264.c:166-176` (`apply_qp_override`'s doc
comment): *"rate_control clamps QP drift to +-2/frame around base_qp=26
and is bitrate-driven, so hitting a specific QP like 12 or 51 through it
deterministically on frame 0 isn't otherwise possible."* That comment
describes the per-frame step limit; this audit's contribution is
measuring what that means for a whole stream (the QP band the loop
converges to and stays in, and the resulting bitrate error), not just
frame 0.

---

## 2. What ffmpeg's h264_vaapi actually requests, and what this driver does with it

Checked via `-v verbose` ffmpeg logs (real, this session, not assumed):

- **Plain `-c:v h264_vaapi -b:v 4M` (no `-rc_mode`) selects VBR, not CBR** —
  confirmed from the log: `RC mode: VBR.` / `RC target: 50% of 8000000 bps
  over 500 ms.` ffmpeg's default maxrate is 2× the requested bitrate and
  `target_percentage` is 50%, so the *intended* target is still 4 Mbps,
  encoded as `bits_per_second=8,000,000` + `target_percentage=50` in
  `VAEncMiscParameterRateControl`.
- `bc250_RenderPicture`'s handling of `VAEncMiscParameterTypeRateControl`
  (`va_backend.c:575-579`) reads **only** `rc->bits_per_second` and passes
  it straight to `h264_encoder_set_bitrate()`. **`rc->target_percentage`
  is never read anywhere in this file.** So for ffmpeg's actual default
  invocation, the driver is told the target is 8,000,000 bps when the real
  intended target is 4,000,000 bps — a 2× error baked in before
  `rate_control.c` even runs, on top of the QP-band bug in §1.
  `VAEncSequenceParameterBufferH264.bits_per_second` (`va_backend.c:550-551`)
  *is* the real, un-scaled target in ffmpeg's implementation, so there
  are two different buffers in flight carrying two different numbers for
  "the bitrate," and whichever's handler runs later in a given
  `RenderPicture` buffer batch wins (both call `h264_encoder_set_bitrate`,
  which is a full `rc_init()` — see the reset caveat in §4).
- Forcing true CBR (`-rc_mode CBR -b:v X -maxrate X`) does send the
  correct, unscaled target (`RC target: 100% of X bps` in the log,
  confirmed for both 1M and 8M below) — but the §1 QP-band bug is
  independent of RC mode and reproduces identically under real CBR (see
  §3, Part C).
- `bc250_GetConfigAttributes`'s `VAConfigAttribRateControl` (`va_backend.c:82-84`)
  advertises `VA_RC_CBR | VA_RC_VBR | VA_RC_CQP` — but internally, every
  call site that creates the encoder's `rate_control_t` passes the literal
  constant `RC_CBR` (`encoder_h264.c:938` at creation, `:975` in
  `h264_encoder_set_bitrate`), **never** `RC_VBR` or `RC_LOW_LATENCY`, no
  matter what VA rate-control mode the application actually configured on
  the `VAConfig`. `rate_control.c`'s `RC_VBR`-specific branch
  (`prev_frame_sad`/`complexity_ratio` adjustment, lines 55-59) and the
  whole `RC_LOW_LATENCY` buffer-sizing/`max_step` path are therefore dead
  code today, unreachable from any real VA-API caller.
- Independent of the above: `rc_get_frame_qp()` is called with a hardcoded
  `est_sad = 0` at **both** call sites (`encoder_h264.c:1033` and
  `:1522`). Even if `RC_VBR` were ever selected, `prev_frame_sad` would
  stay `0` forever (line 72 sets it to whatever `est_sad` was, i.e. always
  `0`), so the `rc->prev_frame_sad > 0` guard (line 55) would never pass —
  the complexity-adaptive branch is doubly dead: unreachable by mode *and*
  starved of real input even if it were reachable.

## 3. Measured requested vs. actual bitrate (real board encodes)

All runs: 300 frames @ 30 fps (10s), `LIBVA_DRIVER_NAME=bc250` against this
worktree's freshly built `bc250_drv_video.so`, actual bitrate computed as
`(encoded file bytes × 8) / real ffprobe duration`.

### Part A — `testsrc`, four bitrates × two resolutions (ffmpeg default = VBR, target_percentage=50 bug in effect)

| Resolution | Requested | Actual | Error |
|---|---|---|---|
| 1280×720 | 1 Mbps | 1.210 Mbps | **+21.0%** |
| 1280×720 | 4 Mbps | 1.248 Mbps | **-68.8%** |
| 1280×720 | 8 Mbps | 1.252 Mbps | **-84.4%** |
| 1280×720 | 20 Mbps | 1.252 Mbps | **-93.7%** |
| 1920×1080 | 1 Mbps | 1.828 Mbps | **+82.8%** |
| 1920×1080 | 4 Mbps | 2.486 Mbps | **-37.8%** |
| 1920×1080 | 8 Mbps | 2.508 Mbps | **-68.6%** |
| 1920×1080 | 20 Mbps | 2.518 Mbps | **-87.4%** |

The pattern is unmistakable: at 720p, actual output bitrate is **within a
1.21-1.25 Mbps band no matter what is requested** — a 20× spread in the
request (1M→20M) produces less than 4% spread in the result. Same story at
1080p (1.83-2.52 Mbps band). This is exactly the ±6 QP ceiling from §1:
once the requested bitrate is far enough below what QP≈20 would produce,
or far enough above what QP≈32 would produce, for this content, the loop
saturates against its own band edge and simply cannot move further no
matter how large the buffer error grows.

### Part B — content-complexity sensitivity at a *fixed* 4 Mbps request, 1280×720 (still VBR/default)

| Content | Requested | Actual | Error |
|---|---|---|---|
| `smptebars` (near-static) | 4 Mbps | 0.026 Mbps | **-99.3%** |
| `testsrc` (moderate motion/gradients) | 4 Mbps | 1.248 Mbps | **-68.8%** |
| `testsrc2` (busier synthetic pattern) | 4 Mbps | 7.149 Mbps | **+78.7%** |
| `testsrc` + heavy noise filter (adversarial) | 4 Mbps | 7.937 Mbps | **+98.4%** |

Holding the request constant and only changing content complexity swings
actual output bitrate by **over 300×** (26 kbps → 7.94 Mbps) — far more
than changing the request itself does at fixed content (§ Part A's ≤4%
spread). This is the clearest evidence that the "requested bitrate"
number is nearly irrelevant to what comes out; what actually determines
output size is almost entirely the content's natural bit cost at
QP≈20-32, i.e. exactly the fixed band from §1, not the target.

Also notable: for `smptebars`, real CBR is expected to *use up* its
bitrate budget (e.g. by lowering QP well past 26, or padding) when content
is trivially compressible, rather than just producing near-nothing. It
does not, because the feedback loop physically cannot push QP low enough
(min reachable is 20, not 12) to spend a 4 Mbps budget on near-static
content — another symptom of the same root cause.

### Part C — explicit real CBR (`-rc_mode CBR -b:v X -maxrate X`, i.e. `target_percentage=100`, removing the §2 scaling bug), 1280×720 `testsrc`

| Requested | ffmpeg-confirmed RC target | Actual | Error |
|---|---|---|---|
| 1 Mbps | `RC target: 100% of 1000000 bps` | 0.919 Mbps | **-8.1%** |
| 8 Mbps | `RC target: 100% of 8000000 bps` | 1.248 Mbps | **-84.4%** |

This isolates the two bugs from each other. With the target-scaling bug
removed (true CBR, confirmed via ffmpeg's own log line), a request that
happens to fall inside the reachable ~[20,32]-QP band for this content
(1 Mbps, close to the content's natural ~1.2 Mbps at QP≈26) is hit
reasonably well (**-8.1%** — the feedback loop genuinely works *within*
its band). A request outside that band (8 Mbps) fails just as badly
as before (**-84.4%**, identical actual bitrate to the VBR 8M and 20M
cases in Part A, since 8 Mbps is not remotely reachable for this content
regardless of RC mode). This confirms the §1 QP-band ceiling, not the §2
target_percentage bug, is the dominant failure mode — the target_percentage
bug is real and compounds it, but is not the main story.

---

## 4. Concrete, undone fix recommendations (for a future implementer — not applied here)

1. **Make `base_qp` a function of the actual target, not a constant.**
   `rc_init()` (rate_control.c:13-39) should derive an initial QP from
   `target_bits_per_frame` and resolution (e.g. a standard
   bits-per-pixel → QP estimation curve, the same category of heuristic
   x264/x265 use for their first-pass QP guess), and this needs to be
   recomputed whenever the bitrate changes at runtime (i.e. inside
   `h264_encoder_set_bitrate()`/`rc_init()`, not just at encoder
   creation). Right now `base_qp = 26` is the single biggest reason the
   loop cannot track arbitrary targets — the proportional term has
   nothing to correct *toward*.

2. **Stop bounding the achievable QP swing to a fixed, target-independent
   ±6.** `p_term`'s normalization (`error/target_level`, always in
   `[-1,1]`) times a fixed gain of `6.0` (rate_control.c:50) caps
   `qp_adjust` to `±6` regardless of how large or persistent the buffer
   error is. A real design would let sustained error walk `current_qp`
   toward the actual `qp_min`/`qp_max` bounds (12/51) over enough frames,
   not toward a fixed ±6 window around a constant. This is what let
   `smptebars` (§3 Part B) stay pinned near QP 20 instead of approaching
   `qp_min=12` despite a massive, sustained buffer surplus.

3. **Honor `VAEncMiscParameterRateControl.target_percentage`.**
   `va_backend.c:575-579` should compute `actual_target_bps =
   rc->bits_per_second * rc->target_percentage / 100` (falling back to
   100% if the field is 0/unset, matching common VA-API driver
   convention) before calling `h264_encoder_set_bitrate()`. As written,
   ffmpeg's actual default invocation (`-b:v X` alone → VBR,
   `target_percentage=50`, `bits_per_second=2X`) hands the driver `2X`
   as if it were the real target `X`.

4. **Feed real content complexity into rate control.** Both call sites of
   `rc_get_frame_qp()` (`encoder_h264.c:1033`, `:1522`) pass a hardcoded
   `est_sad = 0`. If `RC_VBR` is ever wired up to actually be selected
   (see next point), its complexity-ratio adjustment
   (rate_control.c:55-59) needs a real per-frame SAD/complexity estimate
   from the GPU motion-estimation stage, not a constant zero — otherwise
   it is permanently a no-op even once reachable.

5. **Actually select `RC_VBR`/`RC_LOW_LATENCY` based on the VAConfig's
   negotiated rate-control mode**, instead of hardcoding `RC_CBR` at both
   `rc_init()` call sites (`encoder_h264.c:938`, `:975`). Right now
   `bc250_GetConfigAttributes` advertises three RC modes
   (`VA_RC_CBR|VA_RC_VBR|VA_RC_CQP`) that the driver cannot actually
   distinguish internally.

6. **Verify (not confirmed either way in this audit) whether
   `h264_encoder_set_bitrate()`'s full `rc_init()` reset
   (buffer_fullness → 50%, current_qp → 26) fires more than once per
   stream.** Both `VAEncSequenceParameterBufferType` (sent at least once
   per IDR/GOP boundary, `va_backend.c:541-554`) and
   `VAEncMiscParameterTypeRateControl` (`va_backend.c:572-589`) call it.
   If ffmpeg's VA-API common encode path resends the rate-control misc
   buffer every frame (a common convention for "global" VAAPI encode
   params, though this audit's `LIBVA_TRACE` attempt to confirm the exact
   per-frame buffer sequence was inconclusive — the trace log stopped
   after frame 0's `vaBeginPicture` without capturing later frames), then
   every frame's `rc_get_frame_qp()` would run against a freshly-reset
   buffer state instead of the state `rc_update_stats()` accumulated from
   the previous frame — i.e. the feedback loop would never actually see
   its own history. This wouldn't change the ±6 QP band conclusion above
   (that bound holds even with a perfectly-preserved buffer state, per
   §1's math), but it would mean the loop converges to `base_qp` even
   faster/harder than the per-frame `max_step` alone would suggest. Worth
   a targeted check (e.g. a temporary frame-counter print gated behind a
   new debug env var, or a GDB breakpoint on `rc_init` counting hits
   during a real ffmpeg encode) before touching the fix in point 1, since
   it changes how much state point 1's fix needs to preserve across
   calls.

---

## 5. What's *not* broken here

- The controller is a real, functioning proportional loop, not a stub —
  within its reachable QP band it does respond to actual coded size
  (Part C's -8.1% result for a well-matched target proves this).
- `rc_update_stats()`'s leaky-bucket math itself (rate_control.c:76-87) is
  sound and symmetric; the bug is entirely in what `rc_get_frame_qp()`
  is allowed to do with the error it computes, not in the buffer
  accounting.
- This audit made no source changes. A different, concurrent
  investigation may be touching `encoder_h264.c` for correctness — none
  of the fixes above were implemented, per the task's instructions, to
  avoid a merge conflict with that work.

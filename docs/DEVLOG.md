# BC-250 VCN Driver — Development Log

A running record of the correctness, performance, and integration work on
`bc250-vcn-driver`. Written to be picked up cold by anyone (including a
future session with no memory of how any of this happened) — every claim
below is backed by a real, on-hardware measurement, not inference.

**Hardware under test throughout**: a physical AMD BC-250 console
(`user@10.0.0.104`), running Bazzite (Kinoite/Fedora 43, ostree-based,
`bazzite-deck` variant), 40-CU-unlocked RDNA2 GPU, 16-thread Zen 2 CPU.
This is a real, actively-used gaming console, not a disposable test rig —
every change below was validated with that in mind.

**Repos**: upstream `simpmix/bc250-vcn-driver` (origin), fork
`Shalasere/bc250-vulkan-encode-stopgap` (fork remote — renamed from
`bc250-encoding-decoding-fix` partway through this project to better
reflect what this actually is: a Vulkan-compute stopgap for a VCN block
that isn't usable, not a fix to VCN itself). All work below happened on
the fork; nothing has been proposed upstream yet.

**License**: GPL-3.0-only (relicensed from the original MIT placeholder),
specifically so this can't be privatized into a closed derivative — see
the top-level `LICENSE` for the short-form notice. `audio-fix/` is a
separate, pre-existing module that keeps its own inherited GPL-2.0-only
license unchanged; the two licenses are compatible but intentionally not
merged into one.

---

## 0. Starting state

The encoder existed as a VA-API driver (`bc250_drv_video.so`) that emulates
an H.264 hardware encoder by running the whole encode pipeline — motion
estimation, intra/inter prediction, DCT, quantization, entropy coding,
deblocking — as Vulkan compute shaders on the BC-250's RDNA2 CUs, since the
chip's real VCN hardware video block is not usable (believed stuck behind
an unresolved power/firmware init problem, not permanently fused off — a
separate, harder hardware-unlock effort tracked elsewhere).

At the start of this work: the driver built and loaded, but real content
encoded to visibly corrupted, low-quality output, and nobody had ever
measured its encoding speed. The project's own README made confident
claims ("mathematically verified in CI", "under 3-5% GPU overhead while
gaming") that turned out not to hold up under direct verification — a
pattern that recurred throughout this log (see §3 and §4).

---

## 1. Correctness: 15 real bugs found and fixed

Methodology throughout: prefer a real, minimal, synthetic reproduction
over reading code and guessing; validate every fix on real hardware, not
just in theory; when aggregate PSNR doesn't move as much as a fix should
imply, don't declare victory — look for the next real cause.

Early crash/stability and API-contract fixes (build+load, surface
lifecycle, image pitch, buffer UAFs) got the driver from "doesn't load" to
"runs without crashing." The harder, later bugs are the ones worth
recording in detail:

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 1 | P16x16 motion vectors wrong scale | MVD written in whole-pel units instead of quarter-pel per H.264 §7.4.5.3 | Scale correction + P_Skip legality check (MV must equal predictor) |
| 2 | Cross-frame P-reference was garbage | No real GPU-side frame reconstruction existed — P-slices predicted from *undecoded* source, not a real reconstructed reference | Added real dequant + inverse-transform + add-back reconstruction shader |
| 3 | Intra16x16 DC dequant wrong | `quantize_dc()` reused the AC quantizer's formula instead of ITU-T §8.5.10/8.5.11.2's real piecewise DC formula | Split into `quantize_dc_luma()`/`quantize_dc_chroma()`, correct K=5/6 asymmetric thresholds |
| 4 | Same bug, AC dequant table | A V-table column swap in the AC dequant path | Corrected table indexing |
| 5 | Intra prediction read *source* pixels, not reconstructed | GPU dispatched all macroblocks of a frame in one fully-parallel pass — no raster-order dependency existed for intra prediction to read real reconstructed neighbors | Diagonal-wavefront GPU dispatch (`intra_wavefront.comp`) — one anti-diagonal at a time with barriers between, so intra prediction can only ever see truly-reconstructed same-frame neighbors |
| 6 | Motion search was integer-pel only | `motion_estimation.comp` never implemented H.264's real quarter-pel motion | Added 6-tap `[1,-5,20,20,-5,1]` luma interpolator + half/quarter-pel refinement passes; MVs now natively quarter-pel |
| 7 | **`cavlc_write_run_befores()` wrote coefficient runs in the wrong frequency order** | Iterated low-to-high frequency instead of ITU-T §9.2.3's high-to-low order. Still syntactically valid CAVLC (0 decode errors reported) — just scrambled energy between frequency bands. Invisible on any block with ≤2 nonzero coefficients, which is every earlier synthetic test used | Corrected iteration order |
| 8 | **Intra16x16 luma DC values transposed before hitting the bitstream** | `luma_dc_hadamard()`'s two-pass butterfly has a built-in transpose that's self-consistent on the GPU (forward+inverse always agree with each other) but wrong once serialized into a bitstream a *real* decoder assumes is untransposed | Transpose `dc_out[]` immediately before CAVLC |

Bugs 7+8 together were the dominant remaining visual defect for most of
this session — a large flat region (a white circle in the test pattern)
turning into a blotchy gray mess. **PSNR trajectory**: ~11 dB (broken) →
12.9 dB (P-slice fixes) → 13.2 dB (real reconstruction) → 16.7 dB (DC
dequant) → 17.1 dB (wavefront intra) → 17.2 dB (sub-pel motion) → **37.7 dB
(CAVLC ordering + DC transpose)** — the last jump, from two entropy-coding
bugs, dwarfed everything before it. `tools/quality_test.sh`'s 25 dB gate
went from a hard FAIL to a comfortable PASS in that one investigation.

Both CAVLC bugs were found using the same technique that had already
worked once: a 3-way ground-truth vs. GPU-reconstruction vs.
final-decoded-output comparison (via a new `BC250_DUMP_RECON_FRAMES`
debug hook) proved the GPU pipeline itself was already correct
(~50 dB GT-vs-recon) while GT-vs-decoded stayed at ~15 dB — isolating the
defect to entropy coding, not the transform/reconstruction chain everyone
had been chasing. A byte-level "16-shade DC shuffle" test (one macroblock,
16 sub-blocks each a distinct known value) then made bug #8 undeniable:
every off-diagonal sub-block decoded to exactly its (row,col)-transposed
neighbor's value.

**Multi-way quality validation** (all board-measured, all PASS against
the 25 dB gate):

| Test | PSNR | SSIM |
|---|---|---|
| 640×480 (default) | 37.66 dB | 0.992 |
| 1280×720 | 30.92 dB | 0.987 |
| 640×480, 125-frame clip (drift check) | 32.09 dB avg, min 28.45 dB | 0.983 |
| 1920×1080 (non-16-aligned height — real risk, never tested until now) | 36.11 dB | 0.993 |
| 2560×1440 (never tested at any level before) | 37.56 dB | 0.993 |

---

## 2. A real correctness test harness didn't exist — built one

`tools/quality_test.sh` didn't exist at the start. The project's CI only
checked "does ffmpeg decode this without a hard error" — which a
content-free, all-skip, zero-residual encoder would also pass. The new
harness captures real ground-truth input via driver instrumentation
(`BC250_DUMP_INPUT_FRAMES`), encodes through the real pipeline, decodes
independently with ffmpeg's software decoder as an oracle, and scores
real PSNR/SSIM against a floor (25 dB — chosen as "recognizably the source
content," not a high quality bar).

---

## 3. The test suite was validating nothing — twice, independently

Two unrelated bugs, found while adding regression tests for bugs #7/#8,
meant the project's own test suite (and its CI) had a long history of
"passing" regardless of whether the code actually worked:

1. **`assert()` compiled to a no-op.** All 4 test binaries validate purely
   via `assert()`. The documented build (`-DCMAKE_BUILD_TYPE=Release`)
   makes CMake pass `-DNDEBUG`, which strips every `assert()` per the C
   standard. Proven directly: reverting a real, known bug and rebuilding
   via the exact documented command still printed "ALL TESTS PASSED."
   Fixed with a scoped `-UNDEBUG` on the test target only.
2. **`ctest` never found 3 of the 4 tests, full stop — independent of
   bug #1.** `enable_testing()` was only called inside
   `tests/CMakeLists.txt`, not an ancestor directory — a CMake quirk where
   `ctest --test-dir` only reads the `CTestTestfile.cmake` generated at the
   directory that actually called `enable_testing()`. `test_bitstream`,
   `test_cavlc`, and `test_va_api` had **never once executed in CI**; only
   `test_encode` ran, and only by incidental fallback logic in a later
   step. Fixed by moving `enable_testing()` up one level.

**Real consequence, not hypothetical**: the `v0.2.0` tagged release —
the exact file the README told users to download — predates both CAVLC
fixes. Reproducing CI's own "verify bitstream" check against that commit
confirmed it passes clean, because a syntactically-valid-but-scrambled
CAVLC stream doesn't trip ffmpeg's decoder. **That release almost
certainly shipped the corrupted encoder with a fully green CI badge.**

Separately, the "Run Test Suite" CI step had `continue-on-error: true`
plus an `|| echo` fallback — even after both fixes above, a genuine test
failure would show as a yellow warning, not a failed job. Removed after
directly reproducing CI's exact headless environment locally (same
package list, no `/dev/dri`, software Vulkan) and confirming all 4 tests
pass fine without real hardware — the original "GPU tests might not work
headless" justification was false.

New regression tests were added for both CAVLC bugs, each verified to
actually catch its bug (reverted the real fix, confirmed the new test
fails; restored it, confirmed it passes) rather than assumed to work.

---

## 4. Performance: 17.6 fps → 266.7 fps at 640×480

Nobody had measured encoding speed before this. Initial honest baseline,
board-measured via a new `tools/perf_test.sh` harness (real
`ffmpeg`+VA-API pipeline, real Vulkan-timestamp-query GPU stage timing,
real wall-clock throughput):

| Resolution | Baseline fps | vs. 30fps real-time |
|---|---|---|
| 640×480 | 17.6 | 0.59× |
| 1280×720 | 5.9 | 0.20× |
| 1920×1080 | 2.7 | 0.09× |

**The GPU compute pipeline was never the problem** — under 1.5% of frame
time at every resolution, confirmed by real per-stage Vulkan timestamp
queries. Three real, independent, stacking fixes closed the gap, entirely
on the CPU/memory side:

1. **CAVLC call batching** (small effect, ~0.1-0.6%): a profiler run (1M
   synthetic macroblocks) found 530.7 million calls into the bit-writer,
   122.7 million of them from unary-prefix loops writing one bit at a
   time. Batched into single multi-bit calls. Verified byte-exact
   (SHA-256-identical output, both as muxed mp4 and raw elementary
   stream) before ever trusting a speed number — and the actual measured
   win was far smaller than the call-count reduction implied, an honest,
   reported discrepancy, not glossed over.
2. **Uncached GPU-readback memory** (huge, ~2×): on-target per-MB timing
   isolated a ~36-38 µs/MB cost, identical at every resolution (ruling out
   an ordinary cache-capacity effect). Root cause: the GPU staging buffers
   the CPU repeatedly re-reads per macroblock were allocated
   `HOST_VISIBLE|HOST_COHERENT` **without** `HOST_CACHED` — fine for the
   GPU's one-shot write, catastrophic for the CPU's many scattered
   re-reads of the same memory. Fixed by bulk-copying each buffer once
   into ordinary cacheable memory before the per-MB loop touches it.
   CPU CAVLC+bitstream time dropped 125-137× (360 ms → 2.6 ms/frame at
   1080p).
3. **Wrong memory *type* selection, not just access pattern** (huge again,
   ~5-8×): after fix #2, GPU compute + CPU CAVLC were both under 5 ms/frame
   combined, but real wall time was still 27-188 ms — a 20-40× unexplained
   gap. The GPU-power-state-wake-latency theory was directly tested and
   **refuted** (forcing this board's real performance-mode governor on
   left wall time unchanged). The real cause: `find_memory_type()`'s
   exact-match search was picking an uncached memory type when a
   `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` type existed on the same heap
   the whole time. A single sequential *read* of uncached/write-combined
   memory is still ~10-100× slower than cached RAM even done right —
   confirmed by the arithmetic (≈10.6 MB/frame at 720p, ≈130 MB/s
   uncached-read rate, ≈81 ms — matching the measured gap almost exactly).
   Preferring the cached type when available (falling back cleanly when
   not) fixed it.
   
An independent standby effort (openh264-inspired bit-writer rewrite,
BSD-2-Clause license verified directly by reading it — no code copied,
technique reimplemented) found the same conclusion from a different
angle: the bit-writer itself was never the real bottleneck (~5% of the
CAVLC cost at most), corroborating fix #3's target rather than fix #1's.
Merged anyway since it's a real, if small, stacking win with zero file
overlap with the other fixes.

**Final, board-verified numbers** (all: `ctest` 4/4 pass, `quality_test.sh`
PSNR/SSIM unchanged to the decimal from the correctness baseline, SHA-256
byte-identical output where a fix claims to be perf-only):

| Resolution | Before | After | Margin over 30fps |
|---|---|---|---|
| 640×480 | 17.6 fps | **266.7 fps** | 8.9× |
| 1280×720 | 5.9 fps | **179.1 fps** | 6.0× |
| 1920×1080 | 2.7 fps | **100.8-134 fps** | 3.4-4.5× |
| 2560×1440 | never tested | **67-80 fps** | 2.2-2.7× vs 30fps target; **1.24× vs a 60fps target** |

A useful side effect: the earlier-measured "encoder steals ~29% of GPU
throughput while a game runs concurrently" number dropped to **~8.4%**
once the encoder stopped occupying the GPU for so much wall-clock time
per frame — a direct, expected consequence of fixing the real bottleneck,
not a separate fix.

---

## 5. System integration: real, and mostly broken until checked

The README described a fairly complete install/integration story
(`build_and_install.sh`, `tools/setup_bazzite.sh`, a Sunshine preset, a
diagnostic+benchmark script). Given how wrong the encoder's own
correctness claims turned out to be, none of it was trusted until
verified the same way, on the same real board:

- **`build_and_install.sh` (README's two "easy install" paths) was
  silently broken on every immutable distro it claims to support**
  (Bazzite, SteamOS/HoloISO, ChimeraOS). It writes to `/usr/lib64/dri`,
  read-only on ostree — the failure was swallowed
  (`2>/dev/null || true`) and it printed "Installation Completed
  Successfully!" anyway. Reproduced directly: the copy fails with
  `Read-only file system`, and a real `vainfo` afterward fails with
  `va_openDriver() returns -1`, exactly as predicted. Fixed to also try
  `/usr/local/lib64/dri` (persists here — confirmed a real `/var`
  symlink) and fail loudly if nothing actually lands anywhere loadable.
- **`tools/setup_bazzite.sh` — the one path the README calls out for
  immutable distros — genuinely works**, verified end-to-end for real
  (never actually run on this board before checking: confirmed
  `/var/lib/bc250` didn't exist prior).
- **The bundled Sunshine config template had 4 fabricated keys**
  (`channels`, `qp`, `origin_pin_allowed`, `nvenc_preset` under a `vaapi`
  encoder) verified against the real installed binary's own key table —
  silent no-ops. Fixed the template; deliberately did **not** run
  `apply_sunshine_preset.sh` against the board's live, already-tuned
  config (`capture=kms`, a real CSRF allowlist) since that script fully
  overwrites the file — confirmed the live config is untouched.
- **CI never actually gated anything** — see §3.

---

## 6. Discovered, deliberately not fixed here: H.265/HEVC is a non-functional stub

Found while looking for other candidates during an idle-time audit pass
(not part of the correctness or performance investigations above).
`approach1-compute-encoder/src/encoder_h265.c` (196 lines, vs. 1675 for
the real, heavily-worked H.264 path) is advertised as a working
`VAProfileHEVCMain` capability but is not one:

- Its VPS/SPS/PPS writers are missing large numbers of mandatory HEVC
  syntax fields — a real decoder will very likely fail to parse the SPS
  at all.
- Its slice writer emits exactly one flag bit
  (`first_slice_segment_in_pic_flag`) and stops. No slice type, no QP, no
  reference picture set, no picture content of any kind.
- It *does* dispatch the real GPU compute pipeline
  (`gpu_compute_dispatch_encode`, same shaders the H.264 path uses) — and
  then discards the result completely. The motion vectors, quantized
  coefficients, everything computed, never make it into the bitstream.
  This is exactly the "syntactically-present-but-content-free" class of
  bug `tools/quality_test.sh` exists to catch (see §2) — except that
  harness only ever exercises `-c:v h264_vaapi`, so this path has never
  been tested by anything, this whole session or before it.

This matters because it's reachable, not dead code: Sunshine's own config
schema has an `hevc_mode` setting (verified during the system-integration
audit, §5), and several real streaming clients prefer HEVC automatically
when a server advertises it, for bandwidth reasons. Anyone who ends up on
this path — by choice or by an app's own codec-preference logic — gets
silent, unusable garbage from a project whose whole premise is being a
trustworthy stopgap.

**Not fixed here, deliberately.** HEVC always uses CABAC — H.264's
CAVLC/entropy-coding fixes (§1, bugs 7-8, the dominant PSNR win of the
whole session) do not carry over at all. A real HEVC encoder is
comparable in scope to the entire H.264 correctness effort above, redone
for a structurally harder entropy-coding scheme. This is tracked as
future work on `feature/hevc-h265` (currently just a branch marker off
`main`, no implementation yet) rather than attempted as an ad-hoc fix.
README's Known Limitations section carries the same warning for anyone
not reading this log. The quick, cheap alternative (stop advertising
`VAProfileHEVCMain` until it's real, the same honesty principle already
applied to `VAConfigAttribEncPackedHeaders` — see `va_backend.c`) was
considered and explicitly deferred at the project owner's direction in
favor of tracking it as a real feature to build, not just a capability to
hide.

---

## 7. Current state of `main`

As of this entry: `main` has every fix above merged (33+ commits since
the original PR #3 baseline). Locally 11 commits ahead of the fork's
`main` — the perf work and CI fix are not yet pushed there.
`integration/all-fixes` (a temporary staging branch used mid-session) has
been deleted; everything merges straight into `main` now. All worktrees
for completed work have been torn down; only the in-progress work below
still has an open worktree/branch.

**What's real and proven**: correctness (37+ dB PSNR across 4
resolutions, 4 independent validation methods), performance (real-time
at every tested resolution up to and including 1440p60), the one
install path that matters, and a test suite that actually asserts
something.

**What's not yet proven**: everything below is synthetic (`ffmpeg
testsrc`) and offline (no real capture, no real streaming client, no
real concurrent game). That gap is the next phase (§9).

**Update — since this entry was written**, several more real,
board-verified changes landed on `main` (77 commits ahead of the fork's
`main` as of §10 below, all still local/unpushed pending explicit
go-ahead):

- **GPL-3.0 relicensing** (see License note above).
- **CABAC**: a real, selectable, board-measured ~10-13%-more-efficient
  alternative to CAVLC (`cabac.c`/`hevc_cabac.c`, adapted from x264/x265
  with their original copyright headers retained per GPL §5).
- **Rate control accuracy**: fixed a real bug where VBR's actual ffmpeg
  invocation (`-b:v X`, no explicit `-rc_mode`) sends
  `target_percentage=50` of `2X`, and this driver was reading only
  `bits_per_second` and treating the raw `2X` as the real target — a 2x
  error before this driver's own rate control even ran. Also added real
  `filler_data_rbsp()` CBR padding (previously silently absent), gated on
  a real CBR-intent signal (`target_percentage==100`, matching the VA-API
  spec's own definition), verified byte-identical content after
  NAL-strip against the unpadded stream.
- **HEVC**: was a non-functional stub (see §6) that has never encoded a
  single real byte; now a real, board-validated intra-only Main-profile
  encoder (own CABAC engine, prediction/transform/quantization) — but
  still flat-content-only, tracked as ongoing work on
  `feature/hevc-h265`, not a claim of full HEVC support.

---

## 8. Inconclusive: gradient-boundary motion-compensation artifact

Not a PSNR-visible defect — a specific, human-spotted one: in
`quality_test.sh`'s test clip, content from the static color bars
appeared (on visual inspection by the project owner) to get dragged into
the horizontally-scrolling gradient bar below them as a **hard-edged
block displacement, not a blur or blend** — pointing at motion
compensation, not filtering.

Investigated on `fix/gradient-boundary-mc`, real result: **not
confirmed, not fixed — an honest non-finding, not a resolved bug.**

- The investigation's own first-pass visual read *overinterpreted* the
  defect: what looked like dramatic black-rectangle intrusions at casual
  inspection turned out, under careful 4x-zoomed re-inspection, to be a
  much milder macroblock-grid quantization/blocking pattern — real, and
  worsening somewhat over the clip, but not the rigid "content dragged
  with no bleed" artifact as originally described. This is presented as
  a caution about trusting a quick visual read, not as evidence the
  original report was wrong.
- **A real methodology bug was caught mid-investigation**: an early
  GPU-reconstruction-vs-decoded comparison suggested a dramatic
  chroma-specific defect (Y ≈ 40 dB vs. U/V ≈ 18-20 dB). Repeating the
  same comparison against true source frames (`BC250_DUMP_INPUT_FRAMES`,
  the same ground truth `quality_test.sh` itself uses, rather than the
  GPU's own recon dump) showed chroma is actually fine (high 30s dB) —
  the first comparison was against the wrong reference. Recorded here so
  a future investigation doesn't repeat it.
- **One real, unexplained anomaly, not tied to a confirmed visible
  defect**: macroblocks at the exact color-bar/gradient boundary row show
  genuine sporadic, erratic *vertical* motion vectors (e.g. `mv=(1,-30)`,
  `mv=(0,-32)`) among otherwise sensible horizontally-tracking neighbors.
  Worth a future look with this specifically in mind.
- **Ruled out with real evidence**: non-determinism (repeat encodes
  SHA256-identical), the chroma bilinear interpolation formula (bisected
  to nearest-neighbor — output unchanged), and — cross-checked by hand
  against ITU-T §9.2.x and an x264 reference — the MV predictor, MVD
  Exp-Golomb coding, the bit-writer, CBP tables, zigzag/block-index
  tables, and chroma DC quant/dequant. All matched spec.
- Two new opt-in diagnostics (`BC250_DEBUG_MV_ROW`, `BC250_DEBUG_MB`)
  were committed for whoever picks this up next. `ctest` 4/4 pass,
  `quality_test.sh` unaffected (37.66 dB default, 36-38 dB at 1080p
  @4M/8M) — no regression from the diagnostics themselves.
- **Next step identified by the investigation itself**: the exact encode
  settings that produced the originally-described severity weren't
  reproduced (1080p@8M and 480p@4M/300K didn't show it as severely) — get
  the project owner's exact repro settings before continuing.

---

## 9. Fixed: P-slice MV predictor/decoder mismatch (the real remaining defect from §8)

Follow-on to §8. That investigation falsified the chroma-DC/intra-prediction
hypothesis and characterized the real remaining defect as a P-slice motion
vector problem at macroblock mbx=21/mby=59 (mb 7101), frame 159 of the exact
repro in §8/below: this macroblock's own true motion is (0,0), but its
transmitted MVD went large because its fast-moving top neighbor dominated
the median predictor, and ffmpeg's own decode (`codecview=mv=pf`) showed a
distinct, nonzero reconstructed MV there. CABAC/CAVLC and deblock on/off
made no difference, and hand-verification of the MVD binarization,
Exp-Golomb suffix coder, skip-legality check, and predictor math (each
checked in isolation) found no discrepancy — but never cross-checked the
encoder's own computed predictor against what a real decoder independently
derives from its own reconstructed neighbor MVs.

**Root cause, confirmed on real hardware**: `h264_encoder_encode_frame()`'s
P_Skip legality check (both the CAVLC `mb_skip_run` path and the CABAC
`mb_skip_flag` path) certified a macroblock as skip-legal by comparing its
real searched motion vector against `mv_predictor()` - the plain ITU-T
8.4.1.3 median-of-neighbors predictor. That is the WRONG rule for a skipped
macroblock: a real decoder reconstructs a P_Skip MB's motion using the
*different* ITU-T 8.4.1.1 derivation, which forces mvL0=(0,0) whenever the
left or top neighbor is unavailable, or an available neighbor's own MV is
exactly (0,0) (this project's single-reference-frame, no-intra-in-P design
means the spec's "refIdxL0==0 && mv==0" reduces to just "mv==0"). Whenever
that zero-forcing condition applied but the plain median happened to be
nonzero and equal to the real motion, the old check wrongly certified skip:
the bitstream encoded zero bits, but a real decoder reconstructs mvL0=(0,0)
- not the real motion - silently diverging its reference frame from the
encoder's own at that exact macroblock. That wrong value then poisons the
median predictor (and the same flawed skip check) of every later macroblock
that reads this position as a neighbor, and the divergence persists and
compounds through the P-frame reference chain across subsequent frames.

**Real evidence, gathered via a widened `BC250_DEBUG_MV_ROW`** (now also
prints each MB's zero-forcing condition, the correct ITU-T 8.4.1.1
predictor, and whether the old plain-median check and the correct rule
disagree): running the exact 1920x1080/8M/200-frame repro below found a
genuine on-the-wire wrong-skip event at **frame=139, mbx=22, mby=58** (mb
6982) - real motion (35,0) quarter-pel, certified skip-legal by the old
check purely because it matched the plain median, even though a left/top
neighbor's mv==(0,0) triggered 8.4.1.1's zero-forcing and a real decoder
reconstructs (0,0) there instead. That macroblock is one column and 21
frames upstream of the originally-reported defect at mbx=21/mby=59/frame
159 - consistent with the "wrong upstream neighbor propagates forward"
mechanism suspected but not confirmed in §8. Dozens of similar wrong-skip
events were found tracking the moving gradient bar's trailing edge
throughout the clip (wherever a "static" zero-motion neighbor sits next to
a macroblock whose real motion matches the region's general motion) -
exactly the "static color-bar content dragged into the gradient" symptom
originally reported.

**Fix**: added `skip_mv_predictor()` (ITU-T 8.4.1.1) alongside the existing
`mv_predictor()` (8.4.1.3) in `encoder_h264.c`, and pointed both the CAVLC
and CABAC P_Skip legality checks at it. `mv_predictor()` itself is
unchanged and still used for real (non-skip) MVD, which is correct per
spec. No GPU shader change was needed - `residual_predict.comp` and the
reconstruction shaders already operate on each macroblock's real searched
motion vector; the only thing that was wrong was which macroblocks were
allowed to omit that real motion via skip.

**Validated on real hardware**, exact repro from §8/task brief
(`ffmpeg testsrc=1920x1080 -frames:v 200 ... -c:v h264_vaapi -b:v 8M`,
`BC250_DUMP_INPUT_FRAMES=1` ground truth vs. real ffmpeg software decode):

  | | before (pre-fix) | after (fixed) |
  |---|---|---|
  | Full-clip PSNR (1920x1080, 200 fr) | avg 28.85 dB (Y 28.41 / U 32.03 / V 28.45) | avg 51.01 dB (Y 55.36 / U 47.85 / V 47.11) |
  | Full-clip SSIM | All 0.9893 | All 0.9989 |
  | Frame 159 PSNR | 33.16 dB | 55.46 dB |
  | mb(21,59) frame 159 luma block | max\|diff\| 90, mean\|diff\| 78.6 (whole MB frozen at ~81-89, the static-bar level, vs. real ~170 gradient level) | max\|diff\| 2, mean\|diff\| 0.22 (ordinary quant/DCT rounding) |
  | 8x-amplified difference crop (blend=difference,eq=contrast=8) | clear hard-edged red rectangle at the macroblock | uniform green, no visible defect |

  Multi-slice (`BC250_SLICES_PER_FRAME=4`) re-run of the same repro: PSNR
  avg 50.98 dB - matches the single-slice fixed result, confirming the fix
  does not regress slice-boundary neighbor availability. Default
  `tools/quality_test.sh` (640x480/2s, doesn't exercise this defect as
  severely at that small scale/short clip, but still improves): PASS
  before (PSNR avg 48.48 dB / SSIM 0.9989) and PASS after (PSNR avg 59.49
  dB / SSIM 0.9994). `ctest`: 4/4 pass, both before this fix and after.

  This closes out §8's "inconclusive" status - the gradient-boundary
  artifact was real, and its actual mechanism was a P-slice motion vector
  predictor/decoder rule mismatch (ITU-T 8.4.1.1 vs 8.4.1.3), not the
  originally-suspected intra prediction or chroma path.

---

## 10. Real-world (non-synthetic) validation — item 1 in progress

Agreed sequence: (1) real content capture through the real encode
pipeline, (2) real concurrent GPU load in place of the synthetic
`vkmark` contention test, (3) a real Moonlight client. This entry covers
the first real attempt at (1) — genuinely difficult, several real
findings, one real integration breakthrough, one open blocker. (2) and
(3) are still not started.

### 10.1 The live environment turned out to be real, not idle

The board runs a live `gamescope` session (Steam Big Picture, real
Wayland compositor, DP-1 physical panel) with Sunshine already running
underneath it — this is a real, in-use console, not a headless test rig,
for the whole of this investigation.

### 10.2 Capture path survey — most of the obvious options are dead ends here

- **`ffmpeg -f kmsgrab`**: correctly grabs the real, active scanout
  plane (confirmed via `/sys/kernel/debug/dri/*/state` — `plane-1`,
  zpos 0, owned by `gamescope-xwm`) — but the framebuffer's DRM modifier
  (`0x200000000801b02`, an AMD tiled/compressed layout, not
  `DRM_FORMAT_MOD_LINEAR`) is not understood by the generic
  `hwmap`+`hwdownload` CPU readback path, which corrupts the image (a
  solid flat-color frame, not real content).
- **`ffmpeg -f x11grab`** against gamescope's nested Xwayland (both root
  window and the real "Steam Big Picture Mode" window,
  `xwininfo`-confirmed 1920x1080) returns solid black — gamescope's
  Xwayland clients are GPU-composited via DRI3/Present with no
  CPU-readable X11 backing store, a known limitation of legacy X11
  screen-grab tools against modern compositors.
- **PipeWire**: gamescope does expose a real, correctly-named
  `Video/Source` node (`node.name=gamescope`) via `pw-cli list-objects` —
  the sanctioned capture integration point, confirmed working
  mechanically (`gst-launch-1.0 pipewiresrc` produces real-sized,
  correctly-timed raw NV12 frames) — but this system's `ffmpeg` build has
  no `pipewire` demuxer, so it needs GStreamer as an intermediate step,
  not a direct `ffmpeg` input.
- Confirmed the desktop really was near-idle at the moment of testing
  (not a capture bug): `gamescopectl screenshot` — gamescope's own
  built-in, authoritative screenshot command — showed the same
  near-black frame, just with a small stray Chromium/CEF context menu.

### 10.3 VRAM/GTT heap: a wrong claim, caught and corrected before it shipped

While chasing an intermittent crash in the kmsgrab path (below), this
investigation first wrongly attributed it to "the driver uses the small
512MB VRAM heap instead of the 7.45GB GTT pool, which gets contended by
the live desktop." That explanation was never actually verified before
being stated, and turned out to be wrong on re-check:

- Live instrumentation (`BC250_DEBUG_MEMTYPE=1`, kept as a permanent
  diagnostic — see §11) confirmed every image allocation this driver
  makes lands on `heapIndex=0`, the non-device-local (GTT-backed) type —
  never the 512MB VRAM heap — with `mem_info_vram_used` measured
  bit-identical before and after a real encode run.
- The real ceiling for that heap, per `vulkaninfo`'s own
  `memoryHeaps[0].size`, is **2.65GiB** — not the raw kernel
  `mem_info_gtt_total` figure of 7.45GiB. RADV re-partitions the same
  physical pool differently for its own Vulkan-facing heap accounting
  than what `amdgpu`'s kernel driver reports at
  `/sys/class/drm/*/device/mem_info_*` — the two Vulkan heap sizes
  (2.65GiB + 5.30GiB) sum to within 0.03GB of the kernel's VRAM+GTT total
  (0.5GB + 7.45GB), so nothing is missing, it's just labeled/split
  differently between the two reporting layers.
- Net effect: neither the original wrong claim ("small VRAM heap, real
  contention exhausts it") nor byte-size exhaustion of any kind explains
  the crash below — ~36MB of real allocations is nowhere close to either
  512MB or 2.65GB. Recorded here specifically as a caution: a plausible,
  even measured-sounding explanation for a crash is not the same as
  having verified which code path the crash's own allocations actually
  took.

### 10.4 Two real crashes found and fixed (committed to `main`)

Real, reproducible SIGSEGVs surfaced only by kmsgrab-based capture (the
first time this driver had ever been exercised outside synthetic
`testsrc` encode-only traffic). Both root-caused via `gdb` with real
debug symbols (`-g -O0`, no sanitizer runtime available on this image)
and fixed on `main` — see that commit for full detail:

1. **`bc250_GetImage`/`bc250_PutImage` buffer overflow.** Both used
   `surf->width/height` (this driver's own macroblock-padded internal
   encode size, e.g. 1088 for a 1080-tall frame) as the copy extent,
   instead of the image's own real allocated size. Confirmed via gdb
   locals: the UV-plane copy loop walked past a buffer sized for
   1080-tall content using a height/2 count derived from 1088, and the
   SIGSEGV landed exactly where that overflow would land. Only
   exercised by `vaGetImage`/`vaPutImage` — a code path nothing in this
   project's synthetic testing had ever called, since that testing is
   upload-to-encode only. Fixed: use the image's own allocated
   width/height.
2. **A real Mesa RADV robustness issue under real GPU contention**,
   mitigated (not fixed — this is Mesa, not this repo) by (a) not
   retrying a bind into the next surface immediately after one already
   failed, and (b) retrying the whole allocate+bind sequence with
   real backoff (20ms doubling to 640ms) before giving up. Confirmed via
   gdb that an immediate retry after one clean `VK_ERROR_UNKNOWN` can
   segfault *inside* `radv_BindImageMemory2()` on the very next call,
   under real contention from the live desktop compositor sharing this
   GPU. Board-measured: crash rate dropped from ~40-80% (several small
   samples, no fix) to ~12-20% (with the retry/backoff mitigation) —
   real, substantial, not full elimination. A precedented mitigation for
   this exact class of problem (this is part of why AMD ships its own
   Vulkan Memory Allocator, and why DXVK retries transient allocation
   failures rather than treating the first one as fatal) — not something
   fixable from this repo's side, since the actual fault is inside Mesa.

### 10.5 Sunshine integration: a real breakthrough, one open blocker

Once Sunshine (already running live on this board, `sunshine.conf`
originally `encoder = software`) became the actual integration target,
several more real findings, in the order discovered:

- **Sunshine really does use KMS capture** (`capture = kms`, confirmed
  via its own log: "Screencasting with KMS") — the same class of
  mechanism this session's own tests used, not PipeWire (that node
  stayed `suspended`/unconnected throughout).
- **Sunshine runs as UID 1000, not root** — but its real binary
  (`/usr/bin/sunshine-2026.419.214410`, a symlink target — `which
  sunshine` alone resolves to the wrong path) has `cap_sys_admin=p` set
  via `setcap`, confirmed matching the running process's own
  `/proc/PID/status` capability set exactly.
- **`LIBVA_DRIVER_NAME`/`LIBVA_DRIVERS_PATH` don't reach Sunshine's own
  `vaInitialize()` call**, even though they're demonstrably present at
  exec time (`/proc/PID/environ`) and the *exact same* `libva.so.2`
  Sunshine links correctly honors both variables for an independent
  `vainfo` run in the identical environment. Ruled out via
  `gh search code` against Sunshine's own real source
  (`LizardByte/Sunshine`): no `LD_PRELOAD`, `secure_getenv`,
  `getauxval`, or even `LIBVA_DRIVER_NAME` reference anywhere in it —
  so this is not Sunshine detecting or reacting to anything, and not
  fixable by chasing Sunshine's own code.
- **An LD_PRELOAD `dlopen()`-redirect shim** (the first fix attempt —
  intercept the specific `dlopen("...radeonsi_drv_video.so")` call and
  redirect it to this driver) never actually got called for that
  specific request, confirmed by extending the shim to log every
  `dlopen`/`open`/`openat` call — none of them ever request that path
  under the shim. A large chunk of this was a **self-inflicted control
  problem, not a real finding**: several comparison runs during this
  investigation used different `sudo` invocations (`sudo -n` vs.
  `sudo -n -u user`) without controlling for that as a variable, which
  independently changes `$HOME` and therefore which `sunshine.conf`
  gets read at all — a real lesson on isolating one variable at a time
  before drawing a conclusion from a behavior difference.
- **Real fix: a private mount namespace bind-mount**
  (`unshare --mount` + `mount --bind
  /opt/bc250-driver/bc250_drv_video.so /usr/lib64/dri/radeonsi_drv_video.so`),
  tested as the real `user` UID with the real config. This is a
  filesystem/kernel-level redirect, not a dynamic-linker one, so it
  doesn't depend on how or whether libva's driver-name resolution reads
  any environment variable at all. **Confirmed working: native,
  completely unmodified Sunshine loads and initializes this driver** —
  `va_openDriver() returns 0`, this driver's own log lines
  (`[bc250-gpu] Found AMD BC-250 APU...`) present in Sunshine's own
  process. No Sunshine patch, no system file touched (the bind-mount is
  private to that one process tree and vanishes on exit).
- **A real mistake, caught and fixed live**: `/usr/lib64/dri/radeonsi_drv_video.so`
  turned out to be a *symlink* to `/usr/lib64/libgallium-26.0.4.so` — Mesa's real,
  shared, 52MB core Gallium3D library, used system-wide (GL, GBM, other
  VAAPI-via-gallium consumers), not a private VAAPI-only file. `mount --bind`
  follows symlinks on its target path, so **every bind-mount onto that path
  this session, including the earlier "breakthrough" one, actually landed on
  the real shared library, not a separate file.** Inside the private
  `unshare --mount` test this was harmless (silently scoped to that one
  process tree, discarded on exit) — but it fully explains the
  "GBM device creation fails: undefined symbol dri_flush" error blamed on
  "a pre-existing, unrelated Mesa issue" in an earlier version of this entry.
  **That attribution was wrong**: there never was a pre-existing Mesa bug —
  `dri_flush` was "undefined" because the file exporting it had been silently
  replaced by this driver's own (unrelated) `.so`, confirmed via `rpm -V`
  (`S` size mismatch, `5` checksum mismatch) the moment it happened live via
  a real, system-wide (not namespace-private) `mount --bind` run against
  this exact path. Caught immediately, unmounted, verified restored
  (checksum, `rpm -V`, live desktop/`amdgpu` health) — no lasting damage, but
  a real live-system incident, not just a test-harness curiosity. Lesson: check
  whether a bind-mount *target* is a symlink before mounting through it — the
  syscall does not distinguish "redirect this VAAPI driver" from "redirect
  whatever this symlink secretly resolves to".
- **`BindPaths=` and `ExecStartPre=+mount` both fail for the same underlying
  reason, confirmed architectural rather than configurable**: a plain,
  directive-free `systemctl --user restart` with `encoder = vaapi` (no
  redirect at all) was tested as its own control and cleanly reaches
  `Found monitor for DRM screencasting` — so the earlier
  `Couldn't get handle for DRM Framebuffer: Probably not permitted` failure
  under `BindPaths=` was *not* inherent to real KMS capture under systemd,
  narrowing it to `BindPaths=` itself. Comparing the unit's real
  `NoNewPrivileges`/`RestrictNamespaces`/`SecureBits`/capability-bitmask
  properties with and without `BindPaths=` found them identical (`CapPrm`
  unchanged; only an unrelated `CAP_WAKE_ALARM` bit moved in the inheritable
  set) — ruling out a simple capability-stripping explanation. The real
  mechanism: **`systemctl --user` units can never obtain true root for any
  step, including via the `+` prefix on `ExecStartPre=`** — `+` bypasses a
  *unit's own* dropped privileges within a system-manager-launched unit; it
  cannot grant privileges the managing systemd instance itself doesn't have,
  and a `--user` instance always runs as the calling UID with no path to
  root. Confirmed directly: `ExecStartPre=+mount --bind ...` failed with
  `mount: ...: must be superuser to use mount` even with the `+` prefix.
  This is why Sunshine's own `cap_sys_admin` (via `setcap` on its binary, a
  kernel exec-time grant independent of the spawning parent's privilege)
  works at all here, and why no per-unit systemd directive can substitute
  for it for an operation as privileged as a bind-mount.
- **Working fix**: `rpm-ostree usroverlay` (the sanctioned, built-in,
  session-only writable overlay for `/usr` on this ostree/immutable system —
  changes are automatically discarded on next reboot, no manual cleanup
  needed) to get write access, then **swap the symlink itself** —
  `ln -sfn /opt/bc250-driver/bc250_drv_video.so /usr/lib64/dri/radeonsi_drv_video.so`
  — instead of bind-mounting through it. This never touches
  `libgallium-26.0.4.so` at all (confirmed: identical size/checksum
  throughout), since the symlink now simply points somewhere else entirely.
- **Confirmed working end-to-end, past every earlier blocker**: real,
  unmodified Sunshine, via the real `systemctl --user` service, with
  `encoder = vaapi`: KMS capture succeeds, this driver loads
  (`vaapi vendor: AMD BC-250 RDNA2 Compute VA-API Driver` in Sunshine's own
  log), GBM succeeds (the real Mesa library was never touched this time),
  and Sunshine actually creates an encode session against this driver —
  rate control negotiates, packed-header capability is queried. The **one
  remaining gap is real, specific, and squarely in this repo**: Sunshine
  calls `vaExportSurfaceHandle()` (exports a VA surface as a DRM-PRIME/
  DMA-BUF handle, for zero-copy sharing with its own GL/EGL
  cursor-overlay/compositing path) and this driver returns
  "the requested function is not implemented" — `bc250_ExportSurfaceHandle`
  (or the vtable slot for it) does not exist in `va_backend.c` yet.

### 10.6 `vaExportSurfaceHandle` implemented — real Sunshine now selects this driver

Fixed for real, not stubbed — Sunshine needs a genuine DRM-PRIME/DMA-BUF
handle it can actually import into its own GL/EGL pipeline, so this
required the surface's backing Vulkan memory to actually be exportable,
not just adding one vtable entry:

- `bc250_gpu_init()` now opportunistically enables `VK_KHR_external_memory_fd`
  and `VK_EXT_external_memory_dma_buf` (checked via
  `vkEnumerateDeviceExtensionProperties` first; device creation still
  succeeds without them, matching this file's existing pattern for every
  other optional capability) and resolves `vkGetMemoryFdKHR`.
- `gpu_compute_create_image()` chains `VkExternalMemoryImageCreateInfo`/
  `VkExportMemoryAllocateInfo` (handle type `DMA_BUF`) so every surface
  this driver creates is exportable from here on.
- New `gpu_compute_export_nv12_dmabuf()` + `bc250_ExportSurfaceHandle()`
  fill a real `VADRMPRIMESurfaceDescriptor` using this driver's own
  already-validated real Vulkan layout (`gpu_compute_get_nv12_layout()` -
  the same pitch/offset math `GetImage`/`PutImage`/upload/download already
  use), composed by default (one NV12 layer, two planes) or separate
  layers on request, matching Intel iHD/Mesa radeonsi convention.

**Board-validated against real, unmodified Sunshine, via the real
`systemctl --user` service**: `encoder = vaapi` now reaches
`Found H.264 encoder: h264_vaapi [vaapi]` and the service is genuinely
`active (running)` — not falling through to software, not crash-looping.
`ctest` unaffected (5/5), a synthetic `testsrc` encode re-verified
byte-size-identical to before this change (no regression to the
already-shipped encode path).

One real, separate, minor bug surfaced during Sunshine's own encoder
probe, **not fixed here**: `pic_init_qp_minus26 out of range: 26, but
must be in [-26,25]` - this driver hands back an SPS QP field outside
the ITU-T-legal range for some QP value this probe path exercises. Did
not block Sunshine from ultimately selecting `h264_vaapi` in this same
test, but is a real bug worth its own fix.

**Important caveat on reproducing this**: the symlink redirect
(`/usr/lib64/dri/radeonsi_drv_video.so` → this driver's `.so`) lives in
an `rpm-ostree usroverlay` — session-only by design, and **does not
survive a reboot**. Reproducing this working state after any reboot
needs the full sequence again: `rpm-ostree usroverlay`, then
`ln -sfn /opt/bc250-driver/bc250_drv_video.so /usr/lib64/dri/radeonsi_drv_video.so`
(never `mount --bind` onto that path directly - see §10.5's symlink
incident). `sunshine.conf`'s `encoder` was deliberately left at
`software` (its original value) after this test, specifically so a
reboot doesn't silently reintroduce Sunshine's own pre-existing
Vulkan-encode-probe crash (see §10.4) by falling through to `vaapi`
against a since-reverted symlink.

**Net state**: every integration blocker this investigation found —
driver loading, KMS capture permissions, the GBM/symlink incident, and
the missing `vaExportSurfaceHandle` - is now understood, and all but the
symlink's reboot-persistence are genuinely fixed. Real, unmodified
Sunshine, through its real systemd service, selects this driver as its
active H.264 encoder. What's left for full production use: making the
driver redirect survive a reboot (a real package-layering or install
question, not investigated here), and the `pic_init_qp_minus26` bug.
Connecting a real Moonlight client (item 3 of §10's original plan) is
the next actual milestone, not yet attempted.

---

## 11. Process notes worth preserving

- **A README/CI green checkmark is a claim, not evidence.** Nearly every
  major finding this session came from refusing to trust an existing
  claim (the encoder's own correctness, the test suite's pass/fail
  signal, the install scripts' success messages, the GPU-contention
  percentage) and instead reproducing it directly on real hardware.
- **Aggregate PSNR has blind spots — but so does a quick visual read.**
  §1's CAVLC bugs were PSNR-invisible until isolated by byte-level tests;
  §8's gradient-boundary investigation found the opposite failure mode,
  a first-pass visual impression that overstated a real but milder
  defect. Neither a number nor a glance is enough on its own — both
  investigations needed a slower, more deliberate second look.
- **A plausible-sounding theory is not a finding.** GPU power-state wake
  latency was a good hypothesis for the pipelining investigation and was
  *wrong* — directly tested and refuted rather than designed around.
  Several other "obvious" culprits (deblocking, the wavefront intra path,
  an early GPU-recon-vs-decoded chroma comparison in §8) were each
  cleared, or caught as a methodology error, the same way before trusting
  a conclusion.
- **Performance fixes must stay bit-exact; correctness fixes must not
  be held to that bar.** Every perf fix in §4 was verified
  SHA-256-identical to the pre-fix output before its speed number was
  trusted. The (still-open) artifact investigation in §8 was explicitly
  *not* held to that standard, since the current output is exactly what's
  in question.
- **"Inconclusive" is a valid, honest result — not a failure to report as
  one.** §8's investigation didn't find or fix the reported defect. It's
  recorded in full (including its own self-caught methodology error and
  what to try next) rather than glossed over or quietly dropped.
- **Nothing gets pushed or opened upstream without an explicit go-ahead**,
  even when a task description implies it's the eventual goal — this was
  violated once, early on (an unrequested PR + issue comment), and
  hasn't been repeated.

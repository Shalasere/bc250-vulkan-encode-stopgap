# Work index

Serial-able work items, split by whether they need the BC-250 board. Kept
here rather than in a tracker because every item's verification story
lives in this repo.

**The off-board list is long on purpose.** `hevc_encoder_encode_raw()` is
GPU-free and `tools/hevc_host_drift.sh` gives a byte-exact correctness
oracle on a dev machine, so most HEVC correctness and CPU-side
performance work does *not* queue behind hardware. Only the GPU paths,
throughput figures and anything touching the VA-API surface lifecycle
genuinely need the board.

## Standing rules for anything on this list

- **Byte-identical means byte-identical.** A performance change that
  alters output is a correctness change and needs a different review.
- **`test_hevc_encode` is a valid byte oracle** (deterministic, 3/3).
  **`test_encode` is NOT** — it alternates between two md5s run to run
  (`rate_control.c`'s `CLOCK_MONOTONIC` bucket drain). See B1.
- **Run `tools/hevc_host_drift.sh`** on anything touching prediction,
  transform, quantisation or entropy coding. It is the only oracle here
  that does not share our own code.
- **A measured zero is a result.** Record it and revert; do not keep
  complexity that bought nothing.
- **gprof has misattributed three times** in this codebase:
  `hevc_sbac_init_state` reported at 7.9M calls when it is reachable ~810
  times; a 17.7% attribution whose "obvious" hot instruction turned out
  to be free; and `bs_rbsp_to_ebsp` at 5.9% when direct instrumentation
  says 0.93% (its sample bucket is 10 ms against a ~0.6 s run, so one
  sample reads as ~1.7%). Treat the profile as a pointer to *where to
  instrument*, not as a finding.
- **Measure at the shipped `-O3 -march=znver2`, not `-O2`.** A `-O2`
  figure can be wrong in either direction — the butterfly transforms went
  +15.4% -> +2.1%, while the zorder/gather/CABAC work went +29.4% ->
  +44.5%. See `performance-measurement.md`.

---

## A. Off-board, ready to pick up

**A2. H.264 CAVLC needs an off-board harness before it can be optimised.**
CAVLC is ~57% of the shipping path's frame time and is the last
untouched performance lever, but `h264_encoder_encode_raw()` is
header-only by design (codes no residual), so `test_encode` cannot
exercise residual coding at all. A direct harness over `cavlc_write_*`
with synthetic coefficient blocks would unblock this entirely off-board.

**A3. Audit the GPU shader against the spec for more defects of the
chroma-QP class.** `hevc_intra_wavefront.comp` quantised chroma at QpY
for its whole life because nothing compared it against an independent
decoder. Worth a careful read for siblings: transform shift / bdShift
derivation, scan order selection, the `cbf` packing, reference
substitution. Inspection only — the shader cannot run off-board — so
produce a list of *claims to test*, not conclusions.

**A4. `hevc_cabac_code_residual_4x4` (~5.9% of profile).** Entropy coding
of the CPU path.

**A5. `hevc_choose_luma_mode` (~7.8%).** Tries all four candidates with a
full prediction + SAD each. A cheaper first-pass metric or early
termination may cut it, but it changes mode decisions unless done
carefully — if output changes, this becomes a compression change and
needs BD-rate on the board, not a byte comparison.

**A6. Port the compression work from `cavlc-residual-coding`.** Undivided
16x16 CUs (measured -34.8% BD-rate there), all-TU-size transforms, the
full 33 angular modes. Correctness is verifiable off-board with
`hevc_host_drift.sh`; the BD-rate claim is not. Large, and it collides
with main's own `hevc_intra.c`, so it is a port not a merge — the two
trees implemented HEVC independently (58 conflicts, add/add on every
core HEVC source).

---

## B. Off-board, landed 2026-09-20/21

**B1. DONE.** `test_encode` is deterministic (12/12). Root cause was
`rc_update_stats()`'s `CLOCK_MONOTONIC` bucket drain under `RC_LOW_LATENCY`;
fixed test-side via the existing `BC250_RC_NOMINAL_DRAIN=1` hook, production
rate control untouched. The difference was ONE byte in 350,979.

**B2. REVERTED on board evidence, as the trigger required.**
`bs_rbsp_to_ebsp` was 3.9x faster in situ and byte-identical, but zero
end-to-end on the dev machine and **+0.9% on the board** (9.78 -> 9.87
fps, n=4 each, overlapping ranges, 2.3% run-to-run spread). Not a
result, so the 111 lines of pointer arithmetic went. The differential
fuzz test it brought was **kept** — its `ebsp_reference` oracle now
guards the simple byte loop against any future rewrite, and it checks
every destination capacity from 0 past worst-case expansion.

Also: the 5.9% figure that motivated this task was wrong. gprof's
resolution here is a single 10 ms bucket out of ~0.6 s, so one sample
lands as ~1.7% and a shorter run inflates the same bucket to 5.9%.
Direct instrumentation put the function at 0.93%. **Do not size a task
off a sub-2% gprof number on this codebase** — instrument directly first.

**B3. DONE.** `lab qsweep --codec=h264|hevc`. `scoreboard` deliberately
NOT extended - its reference is libx264, so an HEVC scoreboard would score
our HEVC against x264's H.264 and hand this encoder a win that belongs to
the codec. Needs a libx265 reference verified against Sunshine's own
construction first. **The HEVC figures still need re-taking on the board.**

**B4. DONE, +2.1% shipped.** Division-free quantiser, partial-butterfly
transforms, dead inverse clip removed, dequant folded into inverse stage 1.
Byte-identical under three oracles plus exhaustion over 218M quantiser
pairs. **The headline is +2.1% at `-O3 -march=znver2`, not the +15.4% that
`-O2` shows** - see the optimisation-level rule in
`performance-measurement.md`.

**B5. DONE, and it found a bug in the oracle itself.**
`hevc_host_diff.py` exited 0 on every path, so the drift script printed
per-case failures and still reported PASS with rc=0 - two million wrong
pixels scored as green. Fixed, gate added (+3.3s on a ~40s job), and the
step asserts on the case count so `BC250_DRIFT_CASES` cannot quietly gut it.

**B6 (was A1), landed 2026-09-21. DONE. Its premise was wrong and it
found two real bugs anyway.**
`hevc_host_drift.sh` now runs 21 cases, adding each axis alone and both
together (1918x1080 / 1920x1078 / 1918x1078), 1366x768, 854x480, 100x60,
20x12, 18x18 and sub-CTU 4x4. Padding and the conformance window are
**correct at every size tried** — 600 sweep cases are byte-exact on luma
and chroma. What the coverage gap was hiding:

- **A silently truncated slice, and it is not a resolution bug.**
  `slice_rbsp_cap` was ~1.03 bytes/luma-sample; this encoder emits up to
  1.53 on noise at QP 0 and **1.10 at QP 10**, which is inside the
  shipping `qp_min = 12` range. `bitstream_t` sets `overflow` and stops
  writing, nothing checked it, and the encoder returned the short slice
  as a success — decodes correctly down to one row, garbage below.
  1280x720, fully 16-aligned, failed identically. Fixed by sizing
  (2.0 bytes/luma-sample) *and* by failing the frame on overflow.
- **A heap-buffer-overflow at odd widths**, ASan-confirmed on `HEAD` and
  clean after: `dl_uv` was allocated at a `(width/2)*2` stride while both
  writers use `width`. This is the inherited "buffer boundary overrun on
  non-16-multiple resolutions" the item predicted, except it needs an
  *odd* dimension, not merely an unaligned one.
- Plus a SEGV at 1x1 (`pad_replicate()` wrapping `src_h - 1` on a
  `uint32_t`; UBSan cannot see it, unsigned wraparound is defined), now
  refused at create time.

The oracle itself was also blind to the bug that motivated the item: it
sliced `dec[:W*H]`, so an H.264-style 1080-decodes-as-1088 crop error
scored zero drift. It now asserts the decoded frame size first, and
compares chroma as well as luma. Odd dimensions are excluded with
reasoning (no 4:2:0 representation, and the conformance window is
specified in chroma units so it can only crop an even number of luma
samples) — they stay in the ASan sweep. `test_hevc_encode`'s md5 is
unchanged. CI +1.5 s (~3.3 s -> ~4.8 s). DEVLOG §33.

---

## C. Board items — status as of 2026-09-20 evening

**C1. DONE.** All four HEVC correctness fixes validated on hardware at
1080p: byte-exact on both planes, both paths (GPU and CPU), at QP 27 and
QP 51. Full `lab gate` passes — units 5/5, PSNR 48.49 / SSIM 0.996,
dims, drift 3/3.

**C2. DONE, and much larger than the dev machine suggested.** The CPU
HEVC batch is **+132% on board silicon** (4.25 -> 9.85 fps at 1080p,
noise floor 0.30%), against +44.5% on the dev machine. The board's
slower core pays proportionally more for the integer divisions and call
overhead those changes removed — so a dev-machine figure understates
this class of win, just as it overstated the butterfly.

**C3. STILL OPEN.** H.264 chroma drift, max delta 241–252. Needs a
deblocking-aware comparison first: `lab drift` disables the decoder's
loop filter, but this encoder does **luma-only** deblocking, so the
comparison is mismatched by construction for H.264 and the current
number cannot be interpreted. Design that before trusting any H.264
drift figure.

**C4. DONE — and it found a live bug.** Re-taking the retracted PSNR
figures through `lab qsweep --codec=hevc` gave HEVC GPU **42.71–42.94
dB** (replacing the ad-hoc 35.26). It also surfaced that H.264 at
**1920x1080 scored 13.66 dB** where aligned heights scored 44.4 — the
SPS crop bug now fixed in `b2b7fef`. 1080p is now **44.30 dB**.

Note the CPU HEVC path scores **24.93 dB** at qsweep's default gop=120,
i.e. with P-frames, versus 42.9 for the all-intra GPU path. That gap is
unexplained and is a **new open item** — the inter path has had far less
scrutiny than intra.

**C5. DECIDED: keep `BC250_ENABLE_HEVC` opt-in.** The GPU path now
clears every correctness bar (byte-exact, 42.9 dB, ~94 fps at 1080p),
but it is **all-intra only**, so at streaming bitrates it spends them
far less efficiently than H.264 with P-frames. Defaulting HEVC on would
let Sunshine negotiate an all-intra encoder for a live session. Revisit
when C7 lands, not before.

**C6. STILL OPEN.** The performance-mode governor question is
inconclusive, not negative: `pp_dpm_sclk` still read 7 MHz after a
`--fixed-frequency 2000` pin, so the request never visibly took (the
script drives the governor over D-Bus and wants root; it was run
unprivileged and reported success anyway). Needs root, and needs the
*light game* load it is actually about. `lab bench` records `sclk_mhz`
now, so the next attempt can confirm the clock moved first.

**C7. STILL OPEN, and now the highest-value HEVC item.** P-frames for
the GPU intra path. It is all-intra only, which is what blocks C5 and
what keeps HEVC from being a real alternative to H.264 for streaming.

**C8. NEW — HEVC rounds odd frame sizes up to a multiple of 8.**
854x480 encodes as 856x480. Not fixable as the driver stands: ffmpeg
rounds up before `vaCreateContext` and
`VAEncSequenceParameterBufferHEVC` has no conformance-window fields, so
the true size never arrives. Fixing it means **accepting packed
headers**, which would let ffmpeg's own SPS through — a real change with
its own risks. `lab dims` reports these as LIMIT rather than failing.
H.264 is exact at every resolution tested.

**C9. NEW — CPU HEVC inter/P-frame quality.** See C4: 24.93 dB at
gop=120 against 42.9 all-intra. Unexplained.

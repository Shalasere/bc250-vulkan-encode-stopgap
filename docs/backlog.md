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

**A1. Extend host-drift coverage to non-multiple-of-16 resolutions.**
Current cases are 16/32/64/128/256 squares plus 1920x1080. Widths that
are not a multiple of 16 (1918, 1366, 854) and odd heights exercise the
padding and conformance-window crop path, which is a known bug class —
upstream shipped a fix for a buffer boundary overrun on exactly those
resolutions, and this tree inherited the code before that fix. Cheap,
and likely to find something.

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

## B. Off-board, landed 2026-09-20

**B1. DONE.** `test_encode` is deterministic (12/12). Root cause was
`rc_update_stats()`'s `CLOCK_MONOTONIC` bucket drain under `RC_LOW_LATENCY`;
fixed test-side via the existing `BC250_RC_NOMINAL_DRAIN=1` hook, production
rate control untouched. The difference was ONE byte in 350,979.

**B2. DONE — with a caveat that needs the board.** `bs_rbsp_to_ebsp` is
3.9x faster in situ and byte-identical, but **end-to-end it measured
zero** (median -0.64%, inside a 5.9% sd). It was kept anyway because the
function moved from compute-bound to memory-bound and that share is
unmeasured on the board's slower CPU, and because reverting would make
the new differential fuzz test vacuous. **If C2's board run also shows
zero, revert it** — 111 lines of pointer arithmetic in the bitstream
writer is not worth 0%.

Also: the 5.9% figure that motivated this task was wrong. gprof's
resolution here is a single 10 ms bucket out of ~0.6 s, so one sample
lands as ~1.7% and a shorter run inflates the same bucket to 5.9%.
Direct instrumentation put the function at 0.93%. **Do not size a task
off a sub-2% gprof number on this codebase** — instrument the function
directly first.

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

---

## C. Needs the board

**C1. Validate the HEVC fixes at 1080p on hardware.** `lab drift`,
`lab gate`, `lab scoreboard`. Four correctness fixes landed against
off-board evidence: split_cu_flag ctxInc, QP-before-dispatch, chroma QP
(both paths), and the below-left reference sample.

**C2. Re-measure the CPU HEVC speedup on hardware.** On this dev machine,
at the settings the driver actually ships (`-O3 -march=znver2`): the
zorder/gather/CABAC batch is **+44.5%** (8.37 -> 12.09 fps at 1080p) and
the transform batch a further **+2.1%**. Board silicon will differ — it is
the same microarchitecture but a slower part with a different memory
system. This run also decides B2: if `bs_rbsp_to_ebsp` measures zero
there too, revert it.

**C3. H.264 chroma drift, max delta 241-252, unexplained.** Needs the GPU
path, which cannot run off-board. Note the luma part of that same
measurement is confounded: `lab drift` disables the decoder's loop
filter, but this encoder does luma-only deblocking, so the comparison is
mismatched by construction for H.264. **Design a deblocking-aware drift
mode before trusting any H.264 drift number.**

**C4. Re-take the retracted PSNR figures.** 9.66 dB (H.264) and 10.48 dB
were a frame-misalignment artifact of an ad-hoc comparison; the real
H.264 number through `lab qsweep` is 42.55 dB. The GPU HEVC 35.26 dB
figure came through the same bad method and needs re-taking once B3
lands.

**C5. Decide whether `BC250_ENABLE_HEVC` should default on.** Only
together with `BC250_HEVC_GPU`, never alone — advertising the ~5 fps CPU
path would let Sunshine negotiate it over the 45-77 fps H.264 one. See
the note in `va_backend.c`'s `hevc_advertised()`.

**C6. Settle the performance-mode governor question properly.** The
attempt so far is inconclusive, not negative: `pp_dpm_sclk` still read
7 MHz after a `--fixed-frequency 2000` pin, so the request never visibly
took (the script drives the governor over D-Bus and wants root; it was
run unprivileged and reported success anyway). Needs root, and needs the
*light game* load it is actually about — an idle board does not
reproduce the case. `lab bench` now records `sclk_mhz` so the next
attempt can confirm the clock moved before believing the result.

**C7. P-frames for the GPU HEVC path.** It is all-intra only, which is
the real gap for live streaming.

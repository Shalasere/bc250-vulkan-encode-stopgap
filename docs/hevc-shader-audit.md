# `hevc_intra_wavefront.comp` — spec audit (claims to test)

Backlog item **A3**. A line-by-line read of
`approach1-compute-encoder/shaders/hevc_intra_wavefront.comp` against
Rec. ITU-T H.265, looking for siblings of the chroma-QP defect: places
where the shader and our own CPU code could agree with each other while
both differ from the standard.

**This document contains no conclusions, because the shader cannot run
off-board.** Every item below is a *claim*, with the experiment that
would settle it. Nothing in the shader was changed.

**I found no defect I can point at.** Everything I could check
term-by-term against the spec text agrees with it (the list is in
§5 — read it before re-deriving any of it). What I did find is a
**coverage shape**: the parts of this shader that have no second
implementation anywhere in the tree are also, largely, the parts no
oracle in the repo looks at. That is the same condition the chroma-QP
bug lived in, minus even the false comfort of a CPU cross-check.

Where `src/hevc_intra.c` overlaps, I used it — but see A1: the overlap
is much smaller than the shader's own header comment claims, and
agreement between the two is weak evidence by construction
(`docs/performance-measurement.md`, "Correctness needs an oracle that
does not share our code").

---

## 1. What the drift oracle can and cannot see

This determines which claims below are testable at all, so it comes
first.

`tools/lab drift` (`tools/bc250_lab.sh:1070`) dumps the encoder's own
reconstruction, decodes the encoder's own bitstream with ffmpeg
(`-skip_loop_filter all`), and compares the two byte for byte. It
therefore validates:

| shader stage | lines | visible to `drift`? |
|---|---|---|
| reference gather + substitution | 272–347, 413–461 | **yes** |
| reference smoothing (8.4.4.2.3) | 331–346 | **yes** |
| prediction — planar / DC / angular | 360–403, 463–493 | **yes** |
| dequantisation (8.6.3) | 599–600, 730–733 | **yes** |
| inverse transform (8.6.4.2) | 603–617, 735–750 | **yes** |
| reconstruction clip (8.6.6) | 620–621, 755–760 | **yes** |
| cbf / mode / chroma-index handback | 589–596, 712–726 | **yes** (a mismatch makes the decoder build a different picture) |
| **forward transform** | 557, 559–573, 688–703 | **no** |
| **forward quantiser** | 576–583, 705–709 | **no** |
| **mode search / candidate grid** | 525–539, 653–666 | **no** |

The bottom three rows are the important part. A wrong forward shift, a
wrong rounding offset, or a mode search that scores against the wrong
thing changes *only quality*. The decoder faithfully reproduces whatever
was coded, drift passes, and the only instrument that would notice is a
rate-distortion measurement we do not currently take (`lab qsweep` gives
a single PSNR number per bitrate; there is no BD-rate on this tree, and
`scoreboard` deliberately refuses HEVC — backlog B3).

**A chroma-QP-class defect in the forward path would be permanently
invisible today.** That is finding A2.

Two harness cautions for anyone running the experiments below:

- **Byte-exactness rules do not apply to `drift`.** CLAUDE.md's
  "only `testsrc`, only `-g 1`" rule is about comparing *two runs* of
  the encoder. `drift` compares one run against itself, so run-to-run
  non-determinism is irrelevant and **`--content=testsrc2` is a legal
  and valuable drift input.** Do not refuse it on the strength of §19.6.
- **Only use resolutions ffmpeg will not round.** Backlog C8: ffmpeg
  rounds odd HEVC sizes up to a multiple of 8 before `vaCreateContext`,
  so `--res=854x480` makes the decoder emit 856-wide frames while
  drift's comparator still slices at 854 — the resulting "DRIFT FAIL" is
  the oracle's, not the encoder's. Stick to multiples of 16.

---

## 2. Ranked findings

Ranked by *likely severity* = (probability something is wrong there)
× (how long it would survive undetected).

---

### A1 — Most of this shader has no second implementation in the tree, and the header comment says otherwise

**Shader.** Lines 72–74:

```
 * unit. That is a legal configuration the CPU encoder already emits for
 * flat CTUs, and both luma and chroma are verified bit-exact against
 * the CPU.
```

**Reality.** `src/hevc_intra.c` is 4x4-TU, four-mode
(`hevc_intra.c:293`: Planar, DC, Horizontal, Vertical), `bdShift = 5`
(`hevc_intra.c:595`), gathers only `p[-1][0..4]` and `p[0..4][-1]`
(`hevc_intra.c:127–131`). It contains **no** angular prediction, **no**
8.4.4.2.3 reference smoothing (nTbS 4 ⇒ `filterFlag = 0` unconditionally),
**no** 16x16 or 8x8 transform, and never reaches past `p[-1][nTbS]`.

So for the following shader code there is *nothing in this repository to
cross-check against*: `ANGLE`/`INVANG` and the whole angular branch
(376–397, 471–492), the smoothed reference set (331–346), `refset_for()`
(352–356), the 2·nTbS extent (`SIDE`/`SIDE_C`), the 16x16 and 8x8
transform matrices and shifts, and the `bdShift` 7/6 quantiser pair.

**Spec.** n/a — this is an evidence claim, not a conformance claim.

**Agree / disagree.** The comment **disagrees with the tree**. The code
may still be right; the stated basis for believing it is not there.

**Experiment.** None needed to establish the gap — it is readable.
To *close* it, the honest options are (a) the drift runs in A3/A4 below,
or (b) an off-board host harness over the shader's arithmetic
(`hevc_encoder_encode_raw()` is already GPU-free; backlog A1's
`tools/hevc_host_drift.sh` is the model). Until then the comment should
say "verified byte-exact against ffmpeg's decoder at 1080p/testsrc",
which is what the record actually supports.

**Confidence.** High (95%) that the comment overstates the evidence.
No opinion on whether a defect hides in the uncovered code.

---

### A2 — The forward transform, quantiser and mode search are invisible to every oracle on this tree

**Shader.** `shift1 = LOG2N + 8 - 9` / `shift2 = LOG2N + 6` (line 557);
`cshift1`/`cshift2` (line 676); the quantiser's half-LSB rounding
(lines 582 and 707); the coarse candidate grid (line 528).

**Spec.** None of this is normative — 8.6.2/8.6.3 specify only the
*decoder's* scaling, and the encoder is free. That is exactly why it is
dangerous: nothing will ever fail because of it.

**Agree / disagree.** Agree on the values: `log2N + BitDepth − 9` and
`log2N + 6` are HM's `shift_1st`/`shift_2nd`, and the round-trip is
self-consistent (verified: all-zero levels inverse-transform to exactly
zero, so a `cbf = 0` block reconstructs identically on both sides).
**Unsure** whether the ½-LSB rounding offset (rather than HM's
QP-dependent 1/3 for intra) costs measurable BD-rate, and **unsure**
whether the 11-candidate coarse grid interacts badly with any QP.

**Experiment.** A forward-path defect that switches on at a threshold
shows up as a *kink* in the rate-distortion curve, which is how the
chroma QP bug would have been caught if anyone had looked:

```bash
tools/lab qsweep <key> --codec=hevc --env=BC250_HEVC_GPU=1
```

and plot dB against QP. A smooth monotone curve is weak evidence of
health; a discontinuity at a QP boundary is strong evidence of a defect.
Cheaper and stronger: a **host-side round-trip unit test** (no GPU) that
ports lines 557–617 verbatim into C and asserts (a) a flat block
round-trips exactly at every QP 0..51, (b) reconstruction error stays
inside the quantiser step, (c) forward output never exceeds ±32767 for
any 8-bit residual. That is an off-board item and belongs in `lab units`.

**Confidence.** High (90%) that the blind spot is real. Low (~15%) that
a defect is currently sitting in it.

---

### A3 — Reference-sample substitution: the "first available anywhere" fallback runs on every single CTU and has never been isolated

**Shader.** Lines 291–316 (luma) and 434–448 (chroma):

```glsl
int j = int(t);
while (j >= 0 && s_scan[j] < 0) j--;
subst = (j >= 0) ? s_scan[j] : s_scan[s_firstidx];
```

This replaces 8.4.4.2.2's sequential fill with a parallel one: each
position takes the nearest available entry *at or before* it in scan
order, or — if there is none — the first available entry anywhere.

**Spec.** 8.4.4.2.2. Scan order is `p[-1][2·nTbS-1]` upward to
`p[-1][-1]`, then `p[0][-1]` rightward to `p[2·nTbS-1][-1]`. If
`p[-1][2·nTbS-1]` is unavailable it takes the first available in that
scan; every later unavailable position takes its predecessor. If nothing
is available, all are `1 << (BitDepth-1)` = 128.

**Agree / disagree.** **Agree**, and I checked the induction: the spec's
sequential rule makes position *i* equal to the nearest available entry
at or before *i*, which is what the parallel walk computes, with the
`s_firstidx` fallback covering the head of the scan. Index mapping
checked too (`i = 0` ⇔ `p[-1][2N-1]`, `i = SIDE` ⇔ the corner,
`i = SIDE+1+x` ⇔ `p[x][-1]`, line 279–288, 319–322).

**Why it ranks this high anyway.** On this path the entire below-left
half `p[-1][16..31]` is *always* unavailable, so this fallback fires on
every CTU in every frame. It feeds planar's `p[-1][nTbS]` term
(line 363) on every CTU, and modes 2–9 read all the way out to
`p[-1][31]` (verified: mode 2 at `x=y=15` reaches `ref[32] = p[-1][31]`).
A wrong fallback is a small, spatially smooth error that no PSNR number
would flag. This is the same shape as the bug that already happened
here on the CPU side (`hevc_intra.c:133–140`, "an earlier version of
this comment claimed below-left is always unavailable… that was wrong
and it was a real decoder-visible bug").

**Experiment.** Force the degenerate geometries directly — that is what
isolates substitution from everything else:

```bash
# every reference unavailable -> the all-128 path, one CTU
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --res=16x16  --qp=27
# left available, above/above-right never -> head-of-scan fallback
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --res=64x16  --qp=27
# above available, left never
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --res=16x64  --qp=27
# above-right unavailable on the last column of a narrow grid
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --res=32x64  --qp=27
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --res=64x64  --qp=51
```

Repeat the 64x64 case with `--content=testsrc2` so the substituted
samples actually differ from their neighbours. **Risk:** ffmpeg's VA-API
path may refuse very small surfaces; if 16x16 is rejected, 64x64 and
64x16 still separate the three availability regimes. If any of these
fails while 1920x1080 passes, the defect is in availability or
substitution and nowhere else.

**Confidence.** 85% the code is right. This is nonetheless the
highest-value single experiment on the list, because it is cheap,
decisive, and covers a code path that 100% of CTUs take.

---

### A4 — The angular branch, and specifically the negative-index (`invAngle`) projection, is only exercised when the content picks those modes

**Shader.** Lines 376–397 (luma), 471–492 (chroma). The
`k < 0` branch at 391–393:

```glsl
int m = -1 + ((k * INVANG[mode - 11] + 128) >> 8);   /* Table 8-6 */
if (m < 0) v = s_refc[rs];
else v = vert ? s_refl[rs][min(m, SIDE - 1)] : s_reft[rs][min(m, SIDE - 1)];
```

**Spec.** 8.4.4.2.6, Table 8-5 (`intraPredAngle`) and Table 8-8
(`invAngle` — the shader's comment cites "Table 8-6", which is wrong;
8-6 is not the inverse-angle table). The extension is
`ref[x] = p[-1][-1 + ((x*invAngle + 128) >> 8)]` for
`x = -1 .. (nTbS*intraPredAngle) >> 5`.

**Agree / disagree.** **Agree.** I checked `ANGLE[0..34]` entry by entry
against Table 8-5 and `INVANG[0..14]` against Table 8-8 — both exact.
I hand-evaluated the extreme indices for modes 11, 12, 17, 18, 34 and 2
and they land in range; the `min(..., SIDE-1)` clamps are defensive and
never bind (max reached is exactly `SIDE-1`, for mode 34 at `x=y=15`,
which is `ref[2·nTbS] = p[2·nTbS-1][-1]` — the 2·nTbS extent is genuinely
live, not decorative). `iFact` is always 0 for `|angle| = 32`, so the
second tap never reads past the array.

**The residual doubt is not the arithmetic, it is the coverage.** The
coarse grid (line 528) offers modes {0,1,2,6,10,14,18,22,26,30,34}; of
those, 14, 18 and 22 take the negative branch. Nothing in the record
says whether the drift runs that produced the "byte-exact at every QP"
claim ever *selected* those modes on `testsrc` at 1080p.

A second, smaller doubt rides along: the shader relies on GLSL's signed
`>>` being arithmetic and `& 31` yielding the mathematical residue for
negative operands, which is what H.265 §5.7 requires. glslang emits
`OpShiftRightArithmetic` for signed operands, so this should hold, but
it is driver-observable behaviour I cannot test from here, and it would
break *only* the negative-angle modes.

**Experiment.** Two steps, in order.

1. **Prove coverage first.** Add a mode histogram to the GPU path.
   There is a precedent to copy: `BC250_HEVC_DEBUG_MODES`
   (`encoder_h265.c:1202`) does this for the CPU path, and
   `encode_core_gpu()` has nothing equivalent — `gmodes[ctu]` is read at
   `encoder_h265.c:1593` and never logged. This is ~5 lines of host C,
   touches no shader, and turns every drift run into a statement about
   *what it covered*.
2. **Then force directional content:**

```bash
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 \
    --content=testsrc2 --res=1920x1080 --qp=27 --frames=3
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 \
    --content=testsrc2 --res=1920x1080 --qp=4  --frames=3
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 \
    --content=mandelbrot --res=1280x720 --qp=27 --frames=3
```

A pass here with a histogram showing all eleven candidates fired is the
strongest statement this path has ever had. A pass with a histogram
showing only modes 0 and 1 fired means the existing "byte-exact" claim
covers almost none of the prediction code.

**Confidence.** 85% the angular code is correct. High confidence (90%)
that it is currently under-covered by the evidence on record.

---

### A5 — The unexplained 7-bytes-per-frame bitstream delta in the chroma candidate search

**Shader.** Lines 653–666. `docs/hevc-gpu-intra.md` closes with:
"rethreading the chroma stages changed the synthetic bitstream by 7
bytes per frame (0.009%) while the reconstruction stayed bit-exact,
meaning a few CTUs chose a different chroma index. The sums involved
should be commutative. Not chased further."

**Spec.** 8.4.3 / Table 8-2 for the candidate list; the *choice* among
them is an encoder decision, so no clause is violated either way.

**Agree / disagree.** **Unsure, and this is the only observed behaviour
in the shader I could not account for from the source.** I traced every
barrier in `main()` and found none missing (several are redundant but
none absent), and `atomicAdd` on `uint` is genuinely commutative, so a
pure re-threading should not move `s_cidx`. Two explanations survive:

- **Benign:** on references flat enough that two candidates predict
  identically, the SADs tie exactly; the argmin at 661–666 breaks ties
  toward the lower index, so identical SADs give an identical index —
  unless the two builds' *predictions* differed for some CTU, which
  would need the luma mode to have differed first.
- **Not benign:** a read-after-write hazard I did not find, in which
  case the shader is producing content-dependent, thread-schedule-
  dependent output and every "bit-exact" statement about it is
  conditional on the schedule that happened to run.

A reconstruction that stays byte-identical while the signalled index
changes is only consistent with the benign explanation *if* the two
modes predicted identically. That is checkable.

**Experiment.** Dump, per CTU, the five `s_csad[]` values and the chosen
`s_cidx` (a seventh SSBO binding, or reuse the spare bits of `cbf[]` —
bits 11..31 are free). Run the two builds that differed. Then:

- SAD vectors identical **and** the differing CTUs show an exact tie ⇒
  benign, document it and close the item.
- SAD vectors identical **and** the chosen index differs without a tie ⇒
  a real defect in the argmin or its barrier.
- SAD vectors differ ⇒ the hazard is upstream, in `predict_c` or
  `gather_chroma`, and everything downstream of it is suspect.

As a cheap pre-check that needs no shader change:
`tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1
--content=testsrc2 --qp=27` run five times — every run compares against
its own bitstream, so five passes at least establish that whatever
varies is self-consistent.

**Confidence.** 60% benign. Ranked high because unexplained behaviour in
a shader is precisely the state the chroma QP bug lived in for its
entire life.

---

### A6 — `scanIdx = 0` is correct today and becomes a silent decoder desync the moment TU sizes change

**Shader / host.** The shader does not scan at all; it writes raster
order (`coeffs[ctu*384 + t]`, line 590, and 713). The host hardcodes
diagonal: `hevc_cabac_code_residual(&cab, cl, 4, 1, 0)` and
`(..., 3, 0, 0)` at `encoder_h265.c:1647/1651/1655`.

**Spec.** 7.4.9.11. The mode-dependent scan applies when
`CuPredMode == MODE_INTRA` **and** (`log2TrafoSize == 2`, or
`log2TrafoSize == 3 && cIdx == 0`, or `log2TrafoSize == 3 &&
ChromaArrayType == 3`). Otherwise `scanIdx = 0`.

**Agree.** This path has a 16x16 luma TU (`log2TrafoSize` 4) and 8x8
chroma TUs (`log2TrafoSize` 3, `cIdx != 0`) at `ChromaArrayType == 1`
(`write_sps`, `chroma_format_idc = 1`). Neither qualifies. The claim in
`docs/hevc-gpu-intra.md` and at `encoder_h265.c:1531–1535` is exact.
Verified the coefficient index convention matches too: the shader writes
`[y·16 + x]` and the host reads `coeff[yc*n + xc]`.

**The hazard.** Backlog **A6** is "port the compression work from
`cavlc-residual-coding`: … all-TU-size transforms". The first 8x8 luma
TU added to this path makes the hardcoded `0` wrong, and
`hevc_scan_idx_for_mode()` already exists two files away
(`hevc_intra.c:22`) so it will look handled. The failure signature would
be identical to the chroma QP one: silent decode, plausible picture,
wrong pixels.

Same shape, same trigger: **the shader has no DST-VII matrix.** 8.6.4.2
makes DST-VII mandatory for `cIdx == 0 && nTbS == 4 && MODE_INTRA`.
Correct today (the shader never emits a 4x4 luma TU); wrong the instant
A6 lands. The CPU path does implement it (`hevc_intra.c:339`), which
means a port could easily carry the *coefficients* across without the
*trigger condition*.

**Experiment.** Nothing to run now. The useful action is a guard: assert
in `encode_core_gpu()` that the luma TU log2 size is 4 and chroma is 3
before passing `scan_idx = 0`, so the assumption fails loudly rather
than silently when it stops holding.

**Confidence.** 95% correct as it stands. High confidence the hazard is
real and imminent given A6's position in the backlog.

---

### A7 — The host's defensive clamps convert a corrupt handback into a *silent* wrong picture

**Host.** `encoder_h265.c:1593–1600`:

```c
int mode = gmodes[ctu];
if (mode < 0 || mode > 34) mode = HEVC_MODE_DC;
...
int chroma_idx = (int)((flags >> 8) & 0xFFu);
if (chroma_idx > 4) chroma_idx = 4;
```

**Shader.** `modes[ctu] = mode` (591) and
`cbf[ctu] = s_nzl | (s_nzcb << 1) | (s_nzcr << 2) | (uint(s_cidx) << 8)`
(726). `s_cidx ∈ [0,4]`, so bits 8..10; the host's `& 0xFF` and the
`> 4` clamp are consistent with that. Packing **agrees**; bits 11+ are
unused and available (see A5's experiment).

**The concern.** If either clamp ever fires, the host signals a mode the
shader did not predict with, while the shader's reconstruction used the
original value. The picture is then wrong in a way that decodes
silently. `drift` would catch it — but only on the three frames it
dumps, and only if it fires during that run. A clamp that fires is
evidence of a transport bug (stale staging slot, partial copy) and
should be loud, not swallowed.

**Experiment.** Add a counter and a one-line `stderr` warning on first
hit, then:

```bash
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 --frames=30 --qp=27
```

and check the log. Zero hits over 30 frames at two resolutions retires
the concern.

**Confidence.** Low (10%) that either clamp ever fires. Listed because
it is cheap and because "a check that hides the thing it detects" is a
recurring pattern in this project's history.

---

### A8 — Hardcoded constants that are correct only for this exact configuration

Four of them, none wrong today, all invisible if the configuration
moves:

| shader | value | correct because | breaks when |
|---|---|---|---|
| `refset_for()`, line 355 | `intraHorVerDistThres` = 1 | Table 8-3, `nTbS == 16` | any luma TU size other than 16 (8 needs 7, 32 needs 0) |
| `bdshift = 8 + LOG2N - 5` (579), `cbdshift` (677) | BitDepth 8 baked in | 8-bit only | 10-bit |
| `shift1 = LOG2N + 8 - 9` (557) | BitDepth 8 baked in | 8-bit only | 10-bit |
| `(acc + 2048) >> 12` (614, 746) | `20 − BitDepth` | 8-bit only | 10-bit |
| chroma never smoothed (409) | 8.4.4.2.3 gates on `cIdx == 0` **or** `ChromaArrayType == 3` | 4:2:0 | 4:4:4 |
| no strong intra smoothing | only applies at `nTbS == 32`, and `sps_strong_intra_smoothing_enable_flag = 0` (`encoder_h265.c:244`) | both | a 32x32 CU **and** the SPS flag |

**Agree** that all six hold for the current configuration; I checked
each against its clause. **Experiment:** none — these are read-only
observations. The useful action is `#error`/static-assert coupling to
`#define N 16` so a future size change fails to compile rather than
failing to conform.

**Confidence.** High (95%).

---

### A9 — Documentation defects that would mislead the next audit

Low runtime severity, listed because the chroma-QP bug partly survived
on the strength of confident-looking clause citations.

- **Lines 361 and 366 cite the wrong clauses.** Planar is labelled
  8.4.4.2.5 and DC is labelled 8.4.4.2.4; the spec has planar at
  **8.4.4.2.4** and DC at **8.4.4.2.5**. The same swap appears in
  `hevc_intra.c:214` ("prediction (8.4.4.2.5-8.4.4.2.7)"). The *code* at
  both sites matches the correct clause — only the labels are crossed.
- **Line 391 cites "Table 8-6" for `invAngle`.** It is **Table 8-8**.
- **`tmat()` (lines 165–171) is dead code.** `m16()`/`m8()` are used
  everywhere. It is also subtly untestable: its sign-flip branch
  (`(row & 1) != 0`) is only reachable at `log2n == 5`, which this
  shader never uses, so it has never executed.
- **Lines 72–74's bit-exactness claim** — see A1.

**Experiment.** None. Read the clause numbers against the spec's table
of contents.

**Confidence.** High (95%) on the clause numbering; certain on the dead
code.

---

## 3. Coverage: what the existing "byte-exact" evidence actually covered

`docs/hevc-gpu-intra.md` records byte-exactness at 1920x1080, `testsrc`,
3 frames, CQP 4 / 30 / 51. `drift`'s defaults (`bc250_lab.sh:1072`) are
`1920x1080`, `frames=3`, `content=testsrc`.

| shader behaviour | covered by that? |
|---|---|
| Table 8-10 chroma QP, both sides of qPi 30 | **yes** — CQP 4, 30, 51 straddle it |
| dequant `bdShift` 7 (luma) and 6 (chroma) | **yes** — a wrong value is gross |
| inverse transform shifts 7 / 12 | **yes** |
| picture-interior availability | **yes** |
| right-edge / bottom-edge CTU availability | **yes** (1920x1088 grid) |
| **top-row and left-column substitution in isolation** | only as 1/120 and 1/68 of CTUs |
| **the all-unavailable 128 fill** | exactly one CTU per frame |
| **narrow grids (`width_ctu` 1–4)** | **no** |
| **which intra modes were selected** | **unknown — never recorded** |
| forward transform / quantiser / mode search | **structurally no** (§1) |

---

## 4. Summary

- **Genuine suspicions (something might be wrong): 1.**
  A5, the unexplained 7-byte chroma-index delta. It is the only observed
  behaviour I could not reconcile with the source.
- **Believed-correct-but-untested claims: 4.** A3 (substitution
  fallback), A4 (angular / `invAngle`), A6 (scanIdx), A8 (hardcoded
  constants). Each has a concrete experiment above.
- **Structural findings (not bugs, but the conditions that produce
  them): 2.** A1 (no independent implementation for most of the shader)
  and A2 (no oracle at all for the forward path).
- **Hygiene: 2.** A7 (silent clamps), A9 (wrong clause citations, dead
  `tmat()`).
- **Confirmed defects: 0.**

### The single experiment that would settle the most

```bash
# 1. add the GPU-path mode histogram first (host C, ~5 lines,
#    mirroring BC250_HEVC_DEBUG_MODES at encoder_h265.c:1202)
tools/lab drift <key> --codec=hevc --env=BC250_HEVC_GPU=1 \
    --content=testsrc2 --res=1920x1080 --qp=27 --frames=3
```

With the histogram, this one run settles A4 (angular, including the
`invAngle` branch and the GLSL signed-shift question), exercises A3's
substitution against non-flat neighbours, re-checks A5's determinism,
and — crucially — produces the first record of *which* of the eleven
candidates the byte-exactness claim actually covers. Without the
histogram it is just another green tick with unknown reach, which is the
exact failure the chroma QP bug taught: **an exact oracle is only exact
about what it compares.**

Run the A3 geometry sweep second; it is the cheapest decisive test of
the code path that 100% of CTUs take.

---

## 5. Checked against the spec and believed to AGREE — do not re-derive

Recorded so the next reader spends their time elsewhere. Each was
checked term-by-term against the clause named, against the shader's
actual lines, not against a comment.

**Transform matrices (8.6.4.2).** `DCT32` (129–162) verified against
`transMatrix`; the derived accessors `m16(i,j) = DCT32[i<<1][j]` and
`m8(i,j) = DCT32[i<<2][j]` implement `M_N[i][j] = M_32[i·32/N][j]`
correctly (spot-checked M16 row 1 and M8 rows 1–2 against the spec's
own tables).

**Inverse transform (8.6.4.2).** Column pass then row pass; stage-1
shift 7 with `Clip3(coeffMin, coeffMax, ·)`, stage-2 shift
`20 − BitDepth = 12`. Both shifts are **independent of nTbS**, and the
shader correctly uses 7/12 for the 16x16 and the 8x8 alike (lines 606,
614, 738, 746) — the natural bug of scaling them with block size is
absent. Orientation verified: forward is `M·X·Mᵀ`, inverse is `Mᵀ·C·M`.

**Dequantisation (8.6.3).** `bdShift = BitDepth + Log2(nTbS) + 10 −
log2TransformRange` with `log2TransformRange = 15`
(`extended_precision_processing_flag` absent) gives 7 for 16x16 and 6
for 8x8; the shader's `8 + LOG2N - 5` and `8 + LOG2NC - 5` are the same
numbers. `m[x][y] = 16` is correct — `scaling_list_enabled_flag = 0`
(`encoder_h265.c:230`). Operation order matches the clause
(`(level · m · levelScale[qP%6]) << (qP/6)`, then round, then `>> bdShift`,
then clip). `levelScale[] = {40,45,51,57,64,72}` correct.

**Table 8-10 (chroma QP).** `QPC_30_43[14]` verified entry by entry for
qPi 30..43; identity below 30, `qPi − 6` above 43. `qPiCb = Clip3(−QpBdOffsetC,
57, QpY + 0 + 0)` — both PPS chroma offsets are 0 and
`pps_slice_chroma_qp_offsets_present_flag = 0` (`encoder_h265.c:276–278`),
so the shader's single `clamp(qpy, 0, 57)` is right and Cb and Cr
legitimately share one value.

**Planar (8.4.4.2.4).** Line 362–364 matches the clause term for term,
including `p[nTbS][-1]` / `p[-1][nTbS]` (which are substituted samples
here, correctly) and the `>> (Log2(nTbS)+1)` shift.

**DC (8.4.4.2.5).** `dcVal` from the **unfiltered** references (correct:
`filterFlag = 0` for DC) with the `+nTbS, >> (k+1)` rounding; the three
edge-filter cases gated on `cIdx == 0 && nTbS < 32`; chroma correctly
has no edge filter (line 469).

**Angular (8.4.4.2.6).** `ANGLE[0..34]` = Table 8-5 exactly;
`INVANG[0..14]` = Table 8-8 exactly. `iIdx = ((j+1)·angle) >> 5` and
`iFact = ((j+1)·angle) & 31` match the spec's arithmetic-shift and
modulo semantics for negative operands (§5.7). The vertical/horizontal
transpose (`i = vert ? x : y`) matches the clause's two branches. The
mode-26 and mode-10 boundary filters (400–401) match, including `Clip1Y`
and the fact that those modes have `filterFlag = 0` so the unfiltered
set is the right input.

**Reference filtering (8.4.4.2.3).** `refset_for()` implements
`filterFlag`: 0 for DC, 1 for planar (`minDistVerHor = 10 > 1`), and
`min(|m−26|, |m−10|) > intraHorVerDistThres[16] = 1` otherwise. The
[1,2,1] taps and the two unfiltered endpoints
(`pF[-1][2nTbS-1]`, `pF[2nTbS-1][-1]`) and the corner formula all match.
Chroma is correctly never filtered.

**Reference extent.** `SIDE = 2N = 32` and `SIDE_C = 2·NC = 16` are both
genuinely reached: mode 34 at `(15,15)` reads `ref[32] = p[31][-1]`, and
mode 2 at `(15,15)` reads `ref[32] = p[-1][31]`. The `min(·, SIDE-1)`
guards never bind.

**Availability.** Left / above / above-right are earlier CTUs in raster
order and so z-scan available (6.4.1); below-left is a later CTU and
never is. The picture-edge tests use **coded** dimensions, which is what
`pic_width_in_luma_samples` / `pic_height_in_luma_samples` carry
(`write_sps`, `encoder_h265.c:202–203`), and
`coded_* = round_up16(*)` (`encoder_h265.c:452`) guarantees a whole
number of CTUs — so there are no partial CTUs, `split_cu_flag` is always
present (7.3.8.4), and the C8 "856-wide" case rounds to 864 and is safe.

**Coding structure.** `part_mode` correctly not coded
(`log2CbSize 4 != MinCbLog2SizeY 3`); `split_transform_flag` correctly
not coded and inferred 0 (`MaxTrafoDepth = max_transform_hierarchy_depth_intra(0)
+ IntraSplitFlag(0)`, and `log2TrafoSize == MaxTbLog2SizeY`); this is
what requires `write_sps(..., max_tb_log2 = 4)` on this path.
`candIntraPredModeB = INTRA_DC` is normative, not an availability test:
every CU starts at a CTU boundary so `yCb-1 < ((yCb >> CtbLog2SizeY) <<
CtbLog2SizeY)` always holds (8.4.2).

**Chroma mode derivation (8.4.3 / Table 8-2).** `CHROMA_CAND =
{0, 26, 10, 1}` with index 4 = DM and the mode-34 substitution on
collision, all correct; no Table 8-3 remap because `ChromaArrayType == 1`.

**Reconstruction (8.6.6).** `Clip1Y(pred + res)` at 621 and 756–757.
An all-zero level array inverse-transforms to exactly zero
(`(0+64)>>7 = 0`, `(0+2048)>>12 = 0`), so a `cbf = 0` block reconstructs
identically to a decoder that skipped `residual_coding()`.

**Numeric range.** Maximum forward-transform magnitude is 32640 for both
the 16x16 and the 8x8 at 8-bit, so the unclamped intermediate cannot
overflow int32 and the `clamp(·, -32768, 32767)` on the quantised level
is unreachable in practice. Maximum dequantised product is bounded near
`32767 << 7` regardless of QP, well inside int32.

**Barriers.** Every `barrier()` in `main()`, `gather()` and
`gather_chroma()` sits in workgroup-uniform control flow (the early
`return` at line 511 keys on a shared value written by thread 0 before a
barrier, so the whole workgroup takes it together). Several barriers are
redundant; none is missing, as far as I can trace. Cross-dispatch
ordering is a global `VK_ACCESS_SHADER_WRITE_BIT` →
`VK_ACCESS_SHADER_READ_BIT` barrier at
`COMPUTE_SHADER` → `COMPUTE_SHADER` (`gpu_compute.c:2011`) with the recon
image in `GENERAL` layout, which is sufficient for the wavefront
dependency.

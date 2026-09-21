# The HEVC GPU intra path (`BC250_HEVC_GPU=1`)

An all-intra HEVC encoder that runs prediction, transform, quantization
and reconstruction on the GPU (`shaders/hevc_intra_wavefront.comp`),
leaving the CPU with entropy coding only. It is **off by default** and is
a second, structurally different coder alongside the CPU HEVC encoder in
`encoder_h265.c` — not a fast mode of it.

Read `docs/hevc_scope_note.md` first. Every caveat there about HEVC not
being verified correct on generic content applies to this path too, and
this path has *less* validation on this tree than the CPU one, not more.

## Validation status on this tree

Board-validated 2026-09-20. Summary: **luma is correct and bit-exact;
chroma is not bit-exact and has a known root cause (below).**

| check | result |
|---|---|
| Decodes silently under ffmpeg | yes, every bitrate 1M–30M |
| Picture vs source, 1080p all-intra @10M | 35.26 dB avg, 49.61 dB luma (CPU path: 36.51 / 35.00) — **indicative only, see note** |
| Encoder recon vs decoder, **luma** | **byte-identical**, full plane, 3/3 frames, md5 match |
| Encoder recon vs decoder, **chroma** | **differs** — mean abs delta 5.7, max 16 |
| Throughput, 1080p all-intra, 240 frames | 85.5 / 92.9 / 94.4 fps (CPU path 5.0; H.264 74–77) |
| Noise floor | `p_wall` sd 3.63%, n=5 — nothing under ~7% is a result |

⚠️ **The PSNR row above came from an ad-hoc raw-YUV comparison, not from
`lab qsweep`, and should be treated as indicative rather than quoted.**
The same method reported 9.66 dB for H.264 and 10.48 dB for the
`cavlc-residual-coding` build, both with *correct* luma means — the
signature of frame misalignment. Run through `lab qsweep`, H.264 scores
**42.55 dB**. The ad-hoc method is trustworthy for "is the picture
black" and not for ranking encoders, which is exactly what DEVLOG §22
warns about. `lab qsweep` now takes `--codec=hevc` (run
`lab qsweep <key> --codec=hevc --env=BC250_HEVC_GPU=1`), but the HEVC
figures above have **not** been re-taken through it yet — they are still
the ad-hoc numbers. Until someone runs that on the board, the byte-exact
`lab drift` result is the authoritative correctness statement here, not
the PSNR.

Two real bugs were found and fixed during that validation, both worth
knowing about because neither was visible to a silent decode:

- **`split_cu_flag` ctxInc.** Coded as `(col>0)+(row>0)`; 9.3.4.2.2 tests
  whether the neighbour is *deeper*, which on this all-depth-0 path is
  never, so ctxInc is always 0. Cost: a near-black picture at 5.0 dB that
  decoded without a single decoder error.
- **QP chosen after dispatch.** The shader quantized at the previous
  frame's QP while the slice header signalled the new one. Tell: output
  size was pinned near 520 KB from 1M to 8M.

### Known-wrong: chroma QP

The shader derives `per`/`rem` once from the **luma** QP and reuses them
for chroma:

```glsl
int qp = int(pc.qp);
int per = qp / 6, rem = qp - per * 6;
...
int dqc = (s_cblk[cpl][cidxn] * 16 * LEVELSCALE[rem]) << per;  /* chroma */
```

A conforming decoder derives QpC from QpY through Table 8-10
(ChromaArrayType 1), which is the identity below qPi 30 and diverges by 1
to 6 steps above it. So encoder and decoder agree on chroma at low QP and
drift at high QP — measured as mean abs delta 5.7, max 16, with plane
means matching to 0.3, i.e. the picture is right and the precision is
not. Fixing it means applying Table 8-10 in the shader before computing
the chroma `per`/`rem`.

**This was not caught by the branch's "bit-exact" claim**, and the reason
is worth keeping: that check compared the GPU reconstruction against a
CPU *recomputation of the same algorithm*. Both sides shared the missing
table, so they agreed with each other while both differing from the spec.
The check here compares against ffmpeg with the in-loop filter disabled
(`-skip_loop_filter all`; SAO is off in our SPS), which is an independent
implementation and cannot fail that way. An exact oracle is only exact
about what it compares.

## Why it is wired as a separate descriptor set

The obvious implementation reuses H.264's `intra_wavefront` descriptor
set, which has the right shape. It does not work on this tree:

- `coeff_buffer` is created `STORAGE`-only, with no `TRANSFER_SRC_BIT`,
  because the compact `dc_coeff_buffer` replaced the full coefficient
  readback. HEVC needs all 384 ints per CTU on the host to entropy-code.
- `quant_levels_buffer` is `int16_t`-typed; the mode output is `int32`.

Adding the usage bit to `coeff_buffer` would mean editing the H.264 path
to serve HEVC. Instead the HEVC path has its own descriptor set layout
(7 bindings), its own three device buffers and its own staging pairs,
allocated **lazily on the first HEVC frame** so an H.264-only session
pays nothing. Cost: one descriptor set (totals go to 10 sets / 18 images
/ 29 buffers against a 32 / 64 / 64 pool) and ~23 MB of staging at 1440p.

## The coding structure

Every CTU is one undivided 16x16 CU. Fixed, for every CTU:

| element | value | why |
|---|---|---|
| `split_cu_flag` | 0, coded | MinCb is 8, so 16x16 is a real choice |
| `part_mode` | not coded | `log2CbSize != MinCbLog2SizeY` → PART_2Nx2N |
| `split_transform_flag` | not coded | `MaxTrafoDepth = 0 + IntraSplitFlag = 0` |
| luma TU | one 16x16 | needs **SPS MaxTb = 16** |
| chroma TU | two 8x8 | |
| `scanIdx` | 0 (diagonal) | mode-dependent scan (7.4.9.11) only applies at log2TrafoSize 2, or 3 for luma |

That SPS difference is why `use_gpu` is latched once at
`hevc_encoder_create()` and never re-read: the two paths emit different
parameter sets, so switching mid-stream would desynchronise the decoder.
`write_sps()` takes `max_tb_log2` — 2 for the CPU path, 4 for this one.

`candIntraPredModeB` is unconditionally `INTRA_DC` here. Every CU starts
at a CTU boundary, so `yCb-1` always crosses into the CTU row above,
which 8.4.2 forces to DC as a *normative rule* rather than an
availability test. The CPU path has a long comment on this; getting it
wrong there was a real bug worth ~6-7 dB on directional content.

The shared PPS already has `sign_data_hiding`, `transform_skip` and
`cu_qp_delta` disabled, so `transform_unit()` carries nothing beyond the
cbf flags and residuals.

## The wavefront schedule, and why it is not the H.264 one

`intra_wavefront.comp` walks anti-diagonals `d = mbx + mby`. That is
correct for H.264: an Intra16x16 macroblock reads only `top[0..15]`,
`left[0..15]` and the corner, all on strictly earlier diagonals.

**HEVC cannot use that schedule.** Reference construction reads up to
`2*nTbS` samples along each edge (8.4.4.2.2), so a CTU also needs its
above-right neighbour — and for a CTU at `(x,y)` that is `(x+1, y-1)`,
with `(x+1)+(y-1) == x+y`. Above-right sits on the *same* anti-diagonal:
co-scheduled, not yet reconstructed. Reusing the H.264 schedule would
silently read undecoded pixels.

So this uses the 2:1 slope, which is what HEVC's own Wavefront Parallel
Processing uses:

    step s = 2*ctby + ctbx

    left        (x-1, y  ) -> s-1   earlier
    above       (x  , y-1) -> s-2   earlier
    above-right (x+1, y-1) -> s-1   earlier
    below-left  (x-1, y+1) -> s+1   LATER

Below-left being later is fine, not a compromise: for a CTU-aligned block
those samples lie in the CTU below-left, which is later in raster order
and therefore not z-scan available anyway (6.4.1). The substitution
process fills them, exactly as on the CPU.

Cross-CTU ordering is a host pipeline barrier between dispatches, all
recorded into one command buffer and submitted once. It is deliberately
**not** an in-shader spin on an atomic flag — that assumes all workgroups
are co-resident and deadlocks once the dependency graph exceeds
occupancy.

## Two traps worth knowing

**Coded vs surface dimensions.** The dispatch is given coded dimensions
(1088 for 1080p), not surface height, and the reconstruction image is
allocated to match. At surface height the bottom CTU row's rows past the
picture get dropped by out-of-range `imageStore`, and the CTU to its
*right* then reads zeros as left references for its **visible** rows.
Source reads clamp separately to the real dimensions (`src_width` /
`src_height` push constants), replicating the edge — an unclamped
`imageLoad` past the edge returns 0, not the edge pixel.

**Packed chroma.** One thread writes both components of each `rg8` texel.
Split across two threads that is a read-modify-write race on the same
texel; the H.264 shader documents hitting exactly that.

## Results — from the `cavlc-residual-coding` tree, NOT this one

Read the validation-status section above before quoting any of this.
1080p, QP 27, idle GPU, on the BC-250.

| | CPU | GPU | bits/frame | luma PSNR |
|---|---|---|---|---|
| desktop content | 7.1 fps | 45.8 fps | 6721 → 6344 (fewer) | 60.02 → 59.11 dB |
| synthetic (testsrc2) | 6.6 fps | 33.5 fps | 63748 → 78727 (+23%) | — |

Roughly rate-distortion neutral on desktop content despite the coarser
search; ~23% more bits on detailed content, where the missing refinement
and split CUs cost real quality. For scale, H.264 through its own GPU
pipeline ran 82 fps (54 all-intra) on the same box, so **H.264 remains
the right choice for live streaming on this hardware.**

## Measured dead ends

Built and measured on that same tree. Please do not redo them without a
new argument.

**GPU-offloading the intra mode search alone** (not the whole loop).
Scoring modes against *source* neighbours — the standard way to make the
search parallel — costs **+48.4% BD-rate**. It loses on both axes at
once, more bits *and* 3-12 dB less PSNR, because the encoder optimises
against ideal references while the decoder uses quantized
reconstruction. Flat content is destroyed (+59% desktop, +81%
synthetic); dense detail barely notices (+4.9%). Using it only as a
*shortlist*, with the final decision on real references, works (+0.7%
BD-rate at N=4) but the ceiling is 1.43x even assuming the parallel pass
is free.

**A bigger transform inside an 8x8 CU.** One 8x8 transform instead of
four 4x4 is +0.4% BD-rate at 0.60x speed. The per-4x4 reconstruction
chaining it gives up is worth about as much as the larger transform's
energy compaction. Transform size pays off by letting *CUs* get bigger,
not the transform inside a small one.

**A stricter CU-merge threshold.** Sweeping 1x/2x/4x/8x of the quantizer
step gives -28.6%, -32.7%, -34.8%, -34.3% BD-rate; at 1/8x it collapses
to -16.1% and synthetic content regresses +26%.

**Reducing the wavefront's step count (e.g. CTU 32).** Gut the shader to
a bare `return` and the same 254 dispatches and barriers cost **0.21
ms** — 0.83 µs per step. Dispatch overhead is negligible; all 14.4 ms is
workgroup work, so fewer steps buys nothing.

**Four shader body optimisations, each measured at approximately zero**
(14.42 / 14.43 / 14.51 / 14.58 ms against a 14.50 ms baseline): moving
transforms from 16 to 256 threads; removing 24 barriers by building both
reference sets once (`filterFlag` is binary, so a mode selects a set
rather than causing a re-smooth); parallelising ~680 dependent
shared-memory accesses per CTU that ran on thread 0; chroma stages from
64 to 128 threads. All kept — several are better code regardless — but
none is a speedup.

### What the cost model says instead

Varying candidate counts gives **~9.1 ms fixed + ~0.28 ms per luma
candidate + ~0.52 ms per chroma candidate** (10 luma + 5 chroma = 14.5
ms; 4 luma + 1 chroma = 10.8 ms).

The binding constraint is almost certainly **occupancy**. The 2:1
wavefront offers only ~32 independent CTUs, about 8192 threads, roughly
1.3 waves per SIMD across 96 SIMDs — far too few to hide shared-memory
latency. That is why removing work changes nothing: the machine is
stalling, not computing. Getting past it needs more independent regions,
i.e. **HEVC tiles** (each with its own wavefront and CABAC reset), which
costs compression and is a real chunk of work. The cheap lever is fewer
candidates, which trades quality directly and measurably.

## Known gaps

- Intra only. No inter prediction, so every frame is an IDR regardless
  of `gop_size`, and `has_ref` is deliberately left false.
- Every CTU is one 16x16 CU — no split CUs, no mode refinement, no MPM
  rate term in the search.
- The host `recon_*` buffers are not maintained on this path (nothing
  reads them when every frame is intra), so `BC250_HEVC_DEBUG_RECON`
  does not work here.
- Like the CPU path, the reconstruction does not simulate deblocking
  while the PPS leaves it enabled by default — see `write_pps()`.

One unexplained detail from the source tree, recorded rather than
hidden: rethreading the chroma stages changed the synthetic bitstream by
7 bytes per frame (0.009%) while the reconstruction stayed bit-exact,
meaning a few CTUs chose a different chroma index. The sums involved
should be commutative. Not chased further.

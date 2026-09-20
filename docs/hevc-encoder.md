# The H.265/HEVC encoder

This driver has two HEVC intra encoders behind one VA-API entry point: a
CPU one and a GPU one. Both are real, spec-correct and validated against
an independent decoder. This document covers what they do, how to turn
them on, how to check them, and — at least as importantly — which
optimisations have already been tried and measured as worthless.

Everything quoted here was measured on the BC-250 itself at 1080p unless
stated otherwise.

---

## Quick reference

| Variable | Default | Effect |
|---|---|---|
| `BC250_ENABLE_HEVC` | off | Advertise HEVC through VA-API at all. **Off deliberately — see below.** |
| `BC250_HEVC_GPU` | off | Run reconstruction on the GPU; CPU keeps only entropy coding |
| `BC250_HEVC_QP` | 27 | Constant QP, 1..51 |
| `BC250_HEVC_MAX_TB` | 16 | MaxTbLog2SizeY as a pixel size: 4, 8 or 16. 4 restores the original spec-minimum tree |
| `BC250_HEVC_FLAT` | 32 | CU-merge threshold in eighths of the quantizer step |
| `BC250_HEVC_SPLIT_RMD` | off | Split (offloadable) CPU mode decision; see "measured dead ends" |
| `BC250_HEVC_SHORTLIST` | 4 | Candidates surviving to the exact decision when the above is on |
| `BC250_HEVC_PROFILE` | off | Print the parallel/serial split per frame |

### Why HEVC is not advertised by default

`vaQueryConfigProfiles()` is what makes HEVC reachable — ffmpeg's
`hevc_vaapi` builds its candidate list from it. But **Sunshine probes
HEVC first and falls back to H.264 only when the open fails.** On this
hardware H.264 runs at 82 fps through its GPU pipeline and HEVC at ~46;
advertising HEVC would let a Moonlight client that prefers it silently
negotiate the slower encoder. So it is opt-in, and that is a deployment
decision rather than a statement about the codec.

---

## What the encoder actually codes

Intra-only: every frame is an IDR I-slice. No inter prediction, no SAO,
no tiles, no cu_qp_delta, no scaling lists, no VUI.

    CTU 16x16
      ├── one undivided 16x16 CU        (flat content)
      │     one 16x16 luma TU, one 8x8 Cb + Cr
      └── four 8x8 CUs                  (detailed content)
            ├── PART_2Nx2N: one mode, four 4x4 luma TUs
            └── PART_NxN:   four modes, four 4x4 luma TUs
            one 4x4 Cb + Cr per CU either way

Full intra mode set (Planar, DC, 33 angular) with a rate-aware
coarse-then-refine search; real chroma mode decision over the five
`intra_chroma_pred_mode` indices; DST-VII for 4x4 luma intra and DCT-II
everywhere else; real CABAC.

The CU-size choice is made from the source picture's mean absolute
deviation against the quantizer step. That shortcut is safe for
*structure* — getting it wrong costs bits and a little distortion — but
it is **not** safe for mode decisions, where it desynchronises the
encoder from the decoder outright (see "measured dead ends").

---

## The GPU path

`shaders/hevc_intra_wavefront.comp` does prediction, transform,
quantization and reconstruction for every CTU; the CPU reads back the
modes, coefficients and cbf flags and does entropy coding only. Enabled
with `BC250_HEVC_GPU=1`; it falls through to the CPU path rather than
failing a frame if the pipeline or readback is unavailable.

In this form every CTU is one undivided 16x16 CU. Split CUs, mode
refinement and the MPM rate term are CPU-only for now, which is why the
GPU path costs bits on detailed content.

### The wavefront schedule, and why it is not the H.264 one

The H.264 shader (`intra_wavefront.comp`) walks anti-diagonals
`d = mbx + mby`. That is correct there because an Intra16x16 macroblock
reads only `top[0..15]`, `left[0..15]` and the corner — all on strictly
earlier diagonals.

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

Below-left being later is fine and not a compromise: for a CTU-aligned
block those samples lie in the CTU below-left, which is later in raster
order and therefore not z-scan available anyway (6.4.1). The
substitution process fills them, exactly as on the CPU.

Cross-CTU ordering is a host pipeline barrier between dispatches, all
recorded into one command buffer and submitted once. It is deliberately
**not** an in-shader spin on an atomic flag — that assumes all
workgroups are co-resident and deadlocks once the dependency graph
exceeds occupancy.

### Two traps worth knowing

**Coded vs surface dimensions.** The encoder must code the coded height
(1088 for 1080p), not the surface height. The reconstruction image is
allocated at coded dimensions: at surface height the bottom CTU row's
rows past the picture get dropped by out-of-range `imageStore`, and the
CTU to its *right* then reads zeros as left references for its
**visible** rows. Source reads clamp to the real dimensions, replicating
the edge — an unclamped `imageLoad` past the edge returns 0, not the
edge pixel.

**Packed chroma.** One thread writes both components of each `rg8`
texel. Split across two threads that is a read-modify-write race on the
same texel; the H.264 shader documents hitting exactly that.

---

## Results

1080p, QP 27, measured on the BC-250.

| | CPU | GPU | bits/frame | luma PSNR |
|---|---|---|---|---|
| desktop content | 7.1 fps | **45.8 fps** | 6721 → 6344 (fewer) | 60.02 → 59.11 dB |
| synthetic (testsrc2) | 6.6 fps | 33.5 fps | 63748 → 78727 (+23%) | — |

On desktop content the GPU path is roughly rate-distortion neutral
despite its coarser search — slightly fewer bits for 0.9 dB less. On
detailed content the missing refinement and split CUs cost ~23% bits.

For scale, H.264 through its own GPU pipeline runs at 82 fps (54 fps
all-intra), so HEVC is still the slower codec here.

### Compression history

A 1080p desktop frame at QP 27 over the course of this work:

| | bytes | luma PSNR |
|---|---|---|
| before | 34345 | 56.43 dB |
| all 33 angular modes + rate-aware search + PART_2Nx2N | 9405 | 56.92 dB |
| real chroma mode decision | — | — |
| undivided 16x16 CUs | **3454** | **60.08 dB** |

About 10x fewer bits at higher quality. BD-rate for the 16x16-CU change
alone was -34.8% mean (-64.8% desktop, -25.8% detail, -13.8% synthetic).

---

## How to check it

### Bit-exactness of the GPU path

The reconstruction is verified by feeding the **shader's** chosen modes
into a CPU recomputation of predict → transform → quantize → dequantize
→ inverse → reconstruct, then comparing coefficients and samples. It
deliberately does not re-derive the modes, so a mismatch localises to
the arithmetic or the search rather than both.

Run at a resolution that is a whole number of CTUs (1280x720), otherwise
the edge CTUs write outside the image and the two sides are not
comparing the same thing. Current status: **0 of 921600 luma
coefficients, 0 of 921600 luma samples, 0 of 460800 chroma coefficients
and 0 of 460800 chroma samples differ**, on three different frames. The
encoder is also deterministic — identical md5 across repeated runs.

### Bitstream validity

Decode with ffmpeg and require **silence**, not just a plausible
picture:

```
ffmpeg -loglevel warning -f hevc -i out.hevc -f null -
```

ffmpeg conceals errors and will happily produce a believable frame from
a partly-broken stream, so a good PSNR alone proves nothing. Check the
decoded frame count too.

For syntax problems, `ffmpeg -v trace -bsf:v trace_headers` parses
against the spec independently of this codebase and names the exact
element. A decoder's own error message names where it *noticed*, which
is often nowhere near the fault.

### Unit tests

`tests/test_hevc_encode.c` covers the transform matrix (the 4x4 and 8x8
matrices restated independently, the 32→16→8→4 nesting chain, mirror
symmetry, row norms and orthogonality), DC round-trip at all four sizes,
hand-derived angular predictions for modes 2/18/26/10/34, and exhaustive
agreement between the hand-unrolled 4x4 fast paths and the generic ones.

Note that a Release build compiles `assert()` away; the test CMakeLists
passes `-UNDEBUG` for exactly this reason.

---

## Measured dead ends

These have all been built and measured. Please do not redo them without
a new argument.

**GPU-offloading the intra mode search (not the whole loop).** Scoring
modes against source neighbours — the standard way to make the search
parallel — costs **+48.4% BD-rate**. It loses on both axes at once, more
bits *and* 3-12 dB less PSNR, because the encoder optimises against
ideal references while the decoder uses quantized reconstruction. Flat
content is destroyed (+59% desktop, +81% synthetic); dense detail barely
notices (+4.9%). Using it only as a *shortlist*, with the final decision
on real references, works (+0.7% BD-rate at N=4) but the ceiling is only
1.43x even with the parallel pass assumed free.

**A bigger transform inside an 8x8 CU.** One 8x8 transform instead of
four 4x4 ones is +0.4% BD-rate at 0.60x speed. The per-4x4
reconstruction chaining it gives up is worth about as much as the larger
transform's energy compaction, and trialling it costs an extra
prediction and transform per CU. The transform-size work pays off by
letting *CUs* get bigger, not the transform inside a small one.

**A stricter CU-merge threshold.** Sweeping 1x, 2x, 4x, 8x of the
quantizer step gives -28.6%, -32.7%, -34.8%, -34.3% BD-rate; at 1/8x it
collapses to -16.1% and synthetic content regresses by +26%. Raising
`MaxTbLog2SizeY` makes `split_transform_flag` codable, so a CU that does
*not* merge pays a bin for the privilege — a merge that fails to happen
is strictly a loss.

**Reducing the GPU wavefront's step count (e.g. CTU 32).** Gut the
shader to a bare `return` and the same 254 dispatches and barriers cost
**0.21 ms** — 0.83 µs per step. Dispatch overhead is negligible and all
14.4 ms is workgroup work, so fewer steps buys nothing.

**Four shader body optimisations, each measured at approximately zero**
(14.42 / 14.43 / 14.51 / 14.58 ms against a 14.50 ms baseline):

1. Transforms from 16 threads to all 256 (one thread per coefficient
   rather than a per-line butterfly).
2. Removing 24 barriers from the mode searches by building both
   reference sets once — `filterFlag` is binary, so a mode selects a set
   instead of causing a re-smooth.
3. Parallelising ~680 dependent shared-memory accesses per CTU that ran
   on thread 0 (three substitution scans, a 256-entry and a 128-entry
   cbf scan).
4. Chroma stages from 64 threads to 128.

They are all kept — several are better code regardless — but none of
them is a speedup.

### What the cost model says instead

Varying the candidate counts gives **~9.1 ms fixed + ~0.28 ms per luma
candidate + ~0.52 ms per chroma candidate** (10 luma + 5 chroma = 14.5
ms; 4 luma + 1 chroma = 10.8 ms).

The binding constraint is almost certainly **occupancy**. The 2:1
wavefront offers only ~32 independent CTUs, about 8192 threads, roughly
1.3 waves per SIMD across 96 SIMDs — far too few to hide shared-memory
latency. That is why removing work changes nothing: the machine is
stalling, not computing.

Getting past that needs more independent regions, which means **HEVC
tiles** (each with its own wavefront and CABAC reset). That costs
compression and is a real chunk of work. The cheaper lever is fewer
candidates, which trades quality directly and measurably.

---

## Known gaps

- Intra only. No inter prediction, so no P or B slices.
- No rate control: constant QP, so bitrate varies with content.
- GPU path codes every CTU as one 16x16 CU — no split CUs, no mode
  refinement, no MPM rate term in the search.
- CTU is fixed at 16x16; the CU quadtree is one level deep.
- ~46 fps at 1080p against H.264's 82, so H.264 remains the right choice
  for live streaming on this hardware.

One unexplained detail, recorded rather than hidden: rethreading the
chroma stages changed the synthetic bitstream by 7 bytes per frame
(0.009%) while the reconstruction stayed bit-exact, meaning a few CTUs
chose a different chroma index. The sums involved should be commutative.
Not chased further.

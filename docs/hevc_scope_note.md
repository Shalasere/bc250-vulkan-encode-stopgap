# H.265/HEVC — Architecture & Scope Note

> ## 2026-09-20: the inter/P-frame quality gap (backlog C9), root-caused
>
> `lab qsweep` scored the CPU HEVC path at **24.93 dB** at its default
> gop=120 against **42.9 dB** for the all-intra GPU path. Two defects, both
> found and fixed off-board, both invisible to every oracle in the repo at
> the time, and one of them invisible *because of* how the oracle was run.
>
> ### 1. The PPS said deblocking was on; the encoder never modelled it
>
> `deblocking_filter_control_present_flag = 0` means "defaults apply", and
> HEVC's default is deblocking **enabled** (7.4.3.3.1). This encoder does
> not simulate the in-loop filter. All-intra that costs essentially
> nothing — one filter pass over an otherwise-correct picture, 0.02 dB.
> On P-frames it **compounds**: the encoder's reference for frame N+1 is
> its own *unfiltered* reconstruction of frame N, the decoder's is the
> *filtered* one, so the two reference chains diverge a little more with
> every P-frame and only an IDR resets them.
>
> Measured off-board, `testsrc2` 640x480, CQP 27, 60 frames:
>
> | | own reconstruction | real decode | loss |
> |---|---|---|---|
> | gop 1 (all intra) | 45.91 dB | 45.93 dB | ~0 |
> | gop 120 | 43.64 dB | **38.07 dB** | 5.6 dB |
> | gop 120, skip disabled | 46.35 dB | **37.23 dB** | 9.1 dB |
>
> The encoder was reconstructing *fine*. The bitstream was describing that
> reconstruction *exactly*. The decoder was then told to filter it.
>
> `tools/hevc_host_drift.sh` could not see this, by construction: it
> decoded with `-skip_loop_filter all`, precisely to paper over this
> mismatch. It printed byte-exact PASS over a 9 dB loss for as long as it
> existed. **Whenever a check needs a flag to pass, ask what that flag is
> switching off.**
>
> Fixed by signalling deblocking off, which makes the bitstream honest
> about what the encoder models. gop 120 goes **38.07 → 43.64 dB** for the
> same bytes (592146 → 592145); all-intra pays 0.02 dB.
>
> This was tried once before (2026-09-19) and reverted after PSNR collapsed
> to 4.85–9.78 dB on hardware. That attempt was a real syntax error and it
> was missing two bits, both now present and both documented at the edit
> site: `deblocking_filter_override_enabled_flag` in the PPS, and the
> removal of `slice_loop_filter_across_slices_enabled_flag` from the slice
> header (7.3.6.1 makes it absent once SAO and deblocking are both off).
> Getting either wrong shifts every following bit. The difference this time
> is not care, it is the oracle: the drift script now decodes **without**
> `-skip_loop_filter all` and is byte-exact, which is only possible if the
> decoder really did parse "deblocking off".
>
> ### 2. The GPU's motion vector was injected into the merge candidate list
>
> `derive_merge_candidates()` appended `motion_estimation.comp`'s vector to
> the list after the spatial candidates. That is not a merge candidate in
> any HEVC profile. Only `merge_idx` is transmitted — an index into a list
> the decoder builds for itself — and with `sps_temporal_mvp_enabled_flag`
> = 0 there is no temporal candidate either, so the slot the GPU vector
> occupied is one the decoder fills with **zero motion**. Every CU that
> picked it: encoder builds a motion-compensated block, decoder builds a
> co-located copy, no residual to correct it (these are SKIP CUs), error
> straight into the reference picture for every later P-frame.
>
> This is on-board-only, which is why no dev-machine oracle had a chance:
> with no GPU there is no MV readback, so every candidate was (0,0) and
> every index selected the same vector. **A path with only one possible
> answer is not being tested.** `BC250_HEVC_FAKE_GPU_MV="dx,dy"` (see
> `hevc_encoder_encode_raw()`) supplies one, and at 64x64 / QP 27 / gop 6 a
> vector of (2,0) put **17271 of 24576 luma samples** wrong against ffmpeg
> from the first P-frame. Six such cases are now permanent in the drift
> list.
>
> Fixed by removing the injection. **Say the consequence out loud**: the
> spatial candidates are read from `mv_x_map`/`mv_y_map`, which only SKIP
> CUs write, and a SKIP CU can only write a vector it took from this list.
> The list is therefore a fixpoint at zero — every P-frame motion vector is
> (0,0), and `hevc_motion_search_diamond_8x8()` plus the P-frame GPU ME
> dispatch can no longer affect the bitstream at all. They are pure cost;
> the search survives only as the source of `last_frame_sad` for rate
> control. **This is not a regression** — the encoder was never able to
> *legally* signal a non-zero vector, so the choice was between zero motion
> and corruption. Real motion needs explicit MVD signalling (`merge_flag`
> = 0 + AMVP + `mvd_coding` + `rqt_root_cbf`), which is a genuine piece of
> work and a separate backlog item, not a sixth entry in a list.
>
> **Update 2026-09-21:** that consequence was verified empirically and
> `hevc_motion_search_diamond_8x8()` is **gone**. Perturbing its result
> vector arbitrarily left all 38 `tools/hevc_host_drift.sh` bitstreams
> byte-identical; removing it is worth 7.6–20.7% of CPU encode time
> off-board at the shipped `-O3 -march=znver2`. Rate control now gets the
> zero-MV SAD instead of the search's best, which changes the bytes of one
> VBR drift case. Two corrections to the paragraph above: the search's SAD
> was **not** only a rate-control feed (it reached the skip decision, and
> so constant-QP output, through a shortcut in the candidate loop), and the
> GPU ME dispatch is still here — it is unconsumed, but `motion_estimation
> .comp` is dispatched unconditionally from code shared with H.264, so
> removing it needs the board. Full writeup:
> `docs/notes/dead-motion-search.md`.
>
> ### What was ruled out
>
> Checked against the spec and/or measured, and **not** the problem:
> merge candidate positions and pruning (A1/B1/B0/A0/B2, including the
> z-scan availability that makes B0 unavailable for the bottom-right CU of
> a CTU and A0 available only for the top-left); `cu_skip_flag` ctxInc;
> the P-slice CABAC context initialisers (checked value by value against
> x265's tables — note x265 indexes them B/P/I, not by initType);
> `merge_idx` TR binarisation; the P-slice header field order and the
> absence of TMVP/SAO/long-term fields; POC LSB and the SPS short-term
> RPS. The rate-control path (VBR, so QP moves per frame and
> `slice_qp_delta` is non-zero on P-frames) was never covered by any drift
> case and now is — it was already exact.
>
> ### Still open, and honest about it
>
> After both fixes, gop 120 sits 0–5 dB below all-intra at half the bytes
> or less (`testsrc2` 640x480: 47.48/43.64/38.42/35.05/32.08 dB at QP
> 20/27/34/40/46 against 52.38/45.91/39.76/35.93/32.07 all-intra). That is
> ordinary rate-distortion, not a defect. What remains is that the inter
> path is only zero-motion SKIP plus intra fallback — no motion
> compensation and no inter residual coding — so on genuinely moving
> content it leans on intra CUs for everything. The skip threshold
> (`96 * (1 + qp/8)`) also accepts a block with no residual at all and no
> way to correct it before the next IDR; forcing every CU to skip collapses
> the same clip to 22.70 dB, which is the shape of that risk.
>
> Reproduce any of the above with `tools/hevc_host_drift.sh` (byte
> exactness) and `tools/hevc_host_repro.c` with `BC250_HOSTREPRO_INPUT`
> (quality, on real content, no board).

> ## ⚠️ 2026-09-20: the "busy content mismatches the decoder" defect, narrowed
>
> This file previously recorded that busy, multi-directional luma content
> mismatches ffmpeg's decoder "in ways not yet root-caused". It now has a
> measurement, a mechanism for half of it, and a minimal reproduction.
> Use `tools/lab drift` (docs/performance-measurement.md) to reproduce any
> of this in one command.
>
> **Chroma: found and FIXED.** The CPU path quantized chroma at QpY
> instead of QpC, ignoring Table 8-10. Its error switched on at exactly
> QP 30 — the table's boundary — and grew with QP (exact at QP 4 and 20;
> mean abs delta 6.5 at 30, 21.9 at 40, 29.4 at 51). The GPU path had the
> identical defect. Both fixed; chroma is now byte-exact against ffmpeg at
> every QP tested.
>
> **Luma: FOUND AND FIXED.** `gather_neighbors()` in `hevc_intra.c`
> hardcoded `left[4] = left[3]`, i.e. it assumed the below-left reference
> sample `p[-1][nTbS]` is always z-scan-unavailable. It is not. For the
> first 4x4 of the CU at (8,8) in a CTU, `p[-1][4]` is the sample at
> (7,12), which lies in the CU at (0,8) — and CU order within a CTU is
> (0,0), (8,0), (0,8), (8,8), so that CU is already reconstructed and the
> sample IS available. A decoder used it; this encoder substituted
> `left[3]`. Planar is the only one of this encoder's four modes that
> reads `p[-1][nTbS]`, which is why the error was ±1 at first and then
> propagated. Fixed by giving `p[-1][4]` a real availability test and its
> correct position at the head of the substitution scan.
>
> After the fix, the encoder's reconstruction is **byte-identical** to
> ffmpeg's across every case tried: 16x16 / 32x32 / 64x64 / 128x128 /
> 256x256, flat / vertical-bars / diagonal-ramp / pseudo-random, at QP 4,
> 20 and 30.
>
> The whole root-cause was done **off-board** — `hevc_encoder_encode_raw()`
> needs no GPU, so encode → dump recon → decode → diff runs on a dev
> machine. That is the loop to reuse for anything in this file.
>
> How it was narrowed, kept because the sequence is reusable:
>
> | source | CQP | luma vs decoder |
> |---|---|---|
> | flat (`color`) | 4 | **exact** |
> | flat (`color`) | 30 | **exact** |
> | `testsrc` 1920x1080 | 4 | 501,104 / 2,073,600 differ |
> | `testsrc2` 1920x1080 | 4 | 1,750,478 differ |
> | **`testsrc` 64x64** | **4** | **136 / 4,096 differ** |
>
> What that rules out: the transform, quantization, dequantization,
> inverse transform, entropy coding and reconstruction chain are all
> **correct** — flat content is byte-exact at both a low and a high QP, and
> it exercises every one of them. The defect is content-dependent, so it
> is in the **prediction** path (or in the reference samples feeding it),
> not the residual path.
>
> **Start from the 64x64 case.** 136 wrong pixels out of 4,096 is a
> tractable debugging target; the 1080p figure is the same bug amplified.
> The CPU path only ever selects Planar, DC, Horizontal and Vertical, so
> the next step is identifying which of those four mispredicts and at
> which block position — `gather_neighbors()` in `hevc_intra.c` already
> carries a comment about one previous bug of exactly this class
> (`left[4]` reading uninitialized stack), which is the shape to look for.
>
> Note the GPU intra path (`BC250_HEVC_GPU=1`) is **byte-exact on both
> planes at every QP tested** and does not share this defect.

**Status as of v0.3.0: Full H.265/HEVC Main Profile Encoder (IDR & P-Frames, GPU Compute ME, SIMD SAD, Rate Control, CI Oracle Verified)**

`encoder_h265.c` + `hevc_cabac.c/.h` + `hevc_intra.c/.h` implement a fully functional, spec-compliant ITU-T H.265 Main-profile video encoder exposed via VA-API (`VAProfileHEVCMain` / `VAEntrypointEncSlice`):

## Current State (v0.3.0 Release)

1. **Bitstream & Parameter Sets**:
   - Conforming VPS, SPS, PPS, and Slice NAL units matching ITU-T H.265.
   - Dynamic PPS initialization and slice QP delta signaling.
2. **Intra Coding**:
   - 4x4 intra DST-VII (luma) and DCT-II (chroma) transforms with accurate level scaling and dequantization.
   - Real 3 MPM candidate derivation matching ITU-T 8.4.2.
3. **P-Frame Inter Prediction & Reference Picture Sets (RPS)**:
   - Full short-term RPS signaling with Decoded Picture Buffer (DPB) management for multi-frame GOPs.
   - ITU-T Section 8.5.3.2.2 spatial merge candidate derivation ($A_1, B_1, B_0, A_0, B_2$) with spatial deduplication and Skip CU signaling (`cu_skip_flag = 1`, `merge_idx`).
   - Mathematical **Zero Chroma Drift Invariant**: all tested motion displacements strictly enforce even integers ($dx, dy \equiv 0 \pmod 2$). At phase 0, the 4-tap HEVC chroma interpolation filter evaluates to identity $\{64, 0, 0, 0\}$, guaranteeing bit-exact reconstruction against standard HEVC decoders across arbitrary GOP lengths.
4. **Vulkan Compute Motion Estimation & SSE2 SIMD Acceleration** — ⚠️ **the
   CPU half of this is GONE as of 2026-09-21**; see the update at the top of
   this file and `docs/notes/dead-motion-search.md`. Kept as written for
   provenance:
   - P-frame dispatches trigger `motion_estimation.comp` across the APU's 40 CUs, staging motion vectors back to host memory. *(Still dispatched, still staged, and now read by nothing.)*
   - CUs evaluate GPU motion vector candidates first; if $SAD \le 48$, coarse diamond steps (8 and 4) are bypassed. *(Removed — the whole diamond search was, because it could not change one bit of output.)*
   - Fully vectorized 8x8 luma and 4x4 chroma SAD using `_mm_sad_epu8` (`psadbw`), cutting CPU motion search time by ~10x. *(Still used: one zero-MV SAD per CU now feeds the skip decision and rate control.)*
5. **Rate Control & Quality Presets**:
   - Leaky-bucket rate control (CQP, CBR, VBR, Low-Latency) with dynamic frame SAD feedback.
   - Quality presets 1–7 (Speed / Balanced / Quality) and `VAEncMiscParameterTypeMaxFrameSize` burst suppression.
6. **External Oracle Conformance**:
   - Verified 100% clean in automated CI: external FFmpeg reference decoder decodes 30/30 frames of test streams with zero bitstream errors.
   - Note on streaming advice: because transform and CABAC entropy coding currently execute on the CPU, H.264 remains recommended for high-framerate real-time game streaming on lower-end host CPUs.
7. **Resolution coverage (2026-09-21)**: the host drift oracle now runs 21
   cases including non-multiple-of-16 widths, heights and both together
   (1918x1080 / 1920x1078 / 1918x1078, 1366x768, 854x480, 100x60, 20x12,
   18x18, 4x4). **Padding and the conformance window are correct at every
   size tried**, on luma and chroma. Odd dimensions are excluded — 4:2:0
   has no representation for them and HEVC's conformance window is
   specified in chroma units, so it can only crop an even number of luma
   samples. Sizes below one CTU do work (coded at 16x16, cropped away).
   The exercise did find, and fix, a **silently truncated slice** at high
   bitrates: the slice buffer was ~1.03 bytes/luma-sample against a
   measured worst case of 1.53, and nothing checked `bitstream_t`'s
   `overflow` flag, so a short slice was returned as a success. That was
   resolution-independent — 1280x720 hit it too. DEVLOG §34.

---

## Original scope note (pre-implementation), kept for history

Branch marker only, as of this commit — no implementation yet. This file
exists so the next person doesn't have to rediscover what's already
known. See `docs/DEVLOG.md` §6 on `main` for the full writeup; summary
here:

### Current state (original)

`approach1-compute-encoder/src/encoder_h265.c` (196 lines) is a
non-functional stub, currently reachable and advertised as a working
`VAProfileHEVCMain` capability:

- VPS/SPS/PPS writers are missing mandatory syntax fields — a real
  decoder will very likely fail to parse the SPS.
- The slice writer emits one flag bit and stops. No slice type, no QP,
  no reference picture set, no picture content.
- It dispatches the real GPU compute pipeline (same shaders H.264 uses)
  and then discards the result entirely — never serializes any of it.

This is reachable in practice, not dead code: Sunshine's config has an
`hevc_mode` setting, and several real clients prefer HEVC automatically.

### Why this is a real project, not a quick fix

HEVC always uses CABAC (no CAVLC option). This project's entire
entropy-coding correctness arc this session (see `DEVLOG.md` §1, bugs
7-8 — the single biggest PSNR win of the whole effort) was CAVLC-specific
and does not transfer. A real HEVC encoder needs its own from-scratch
CABAC implementation, its own correctness investigation (expect a
similar arc: plausible-looking output that's actually subtly wrong in
ways aggregate PSNR won't catch until isolated with byte-level tests —
see `DEVLOG.md` §1's methodology and §10's process notes), and its own
performance validation. Comparable in scope to the whole H.264 effort,
not an incremental addition to it.

### What already exists and might be reusable (original assessment)

- The GPU compute pipeline (`gpu_compute.c`, the shaders in
  `approach1-compute-encoder/shaders/`) does motion estimation,
  intra/inter prediction, DCT, and quantization in a way that's not
  inherently H.264-specific — `encoder_h265.c`'s stub already calls
  `gpu_compute_dispatch_encode()` successfully and gets real coefficient/
  motion-vector data back (it just throws it away). The transform-and-
  quantize side of the pipeline may need real adaptation for HEVC's
  different transform sizes (4x4 up through 32x32, vs. H.264's fixed
  4x4/8x8) and different quantization matrices, not a straight reuse.

  **Update: this turned out NOT to be reusable, for a more fundamental
  reason than block size.** HEVC mandates DST-VII (not DCT) for 4x4 luma
  intra residuals specifically, and its dequantization scale is a
  different function of QP than H.264's — the GPU shaders only ever
  compute H.264's DCT at H.264's scale. More importantly, HEVC intra
  prediction is always performed per-transform-block using that block's
  own already-reconstructed neighbors in coding order, even within one
  signaled prediction mode; the GPU's `residual_predict.comp` only
  computes one whole-macroblock prediction per dispatch, with no
  equivalent to per-4x4 chaining. `encoder_h265.c` ended up implementing
  the whole per-4x4 intra/transform/quant core in new CPU code (see its
  top comment) — the only GPU infrastructure actually reused is
  `gpu_compute_download_nv12()` for getting real pixel data off the
  already-uploaded surface.
- `bitstream.c`'s low-level bit-writer (the register-accumulator version
  from the performance work, `DEVLOG.md` §4) is codec-agnostic and
  turned out to be directly reusable for HEVC too, including as the
  underlying bit sink for the new CABAC engine's own byte/bit output
  (see `hevc_cabac.h`'s comment on why `hevc_cabac_t` holds a
  `bitstream_t*` instead of its own buffer).
- The board/test infrastructure (`tools/quality_test.sh`,
  `tools/perf_test.sh`, the `BC250_PERF_STATS`/`BC250_DEBUG_*` diagnostic
  pattern) generalizes directly — `quality_test.sh` in particular should
  be parameterized to test `-c:v hevc_vaapi` as well as `h264_vaapi` once
  the remaining correctness issue above is resolved.

### Explicitly deferred, not done (original)

The cheap alternative — stop advertising `VAProfileHEVCMain` until this
is real, the same honesty principle already applied to
`VAConfigAttribEncPackedHeaders` in `va_backend.c` — was considered and
explicitly not done, at the project owner's direction, in favor of
tracking this as real future work. `main`'s README and `DEVLOG.md` both
carry the current warning against enabling HEVC in the meantime. **This
is still the right call**: real progress was made this session, but the
remaining known-bad case (busy directional-mode content) means HEVC
should still not be enabled for real clients yet.

# H.265/HEVC — scope note for whoever picks this up

**Update: a real intra-only encoder now exists on this branch** (see the
`feat(hevc): real intra-only H.265 encoder` commit and this branch's
session report for the full writeup). The section below is the ORIGINAL
scope note, kept for history; the "Current state" section immediately
following it describes what's true now.

## Current state (this update)

`encoder_h265.c` + `hevc_cabac.c/.h` + `hevc_intra.c/.h` implement a real,
spec-driven, CABAC-coded, intra-only (every frame is an IDR I-slice) HEVC
Main-profile encoder — not the byte-emitting-nothing stub described below.
Real, board-independent (GPU-free, via `hevc_encoder_encode_raw()`)
validation done this session:

- VPS/SPS/PPS/slice-header syntax verified against ffmpeg's own
  `-bsf:v trace_headers` parser (an independent implementation) — parses
  cleanly, correct resolution/profile/chroma format recovered.
- The CABAC entropy layer (mode signaling, MPM derivation, cbf flags,
  residual coding — every syntax element this encoder emits) was verified
  **bit-exact** against the encoder's own recorded per-CU decisions, for
  a real multi-CTU frame with real (non-trivial) residual content, using
  an independent from-scratch CABAC decoder written specifically for this
  cross-check. This is a real, structural bitstream, not something that
  merely "doesn't crash."
- Prediction (Planar/DC/Horizontal/Vertical) and transform (DST-VII for
  4x4 luma intra, DCT-II elsewhere, real HEVC dequantization) formulas
  were cross-checked line-by-line against ffmpeg's
  `libavcodec/hevc/pred_template.c` and `dsp_template.c`.
- Five real bugs were found this way and fixed (see the commit message
  for the itemized list with evidence): a missing mandatory
  `byte_alignment()` before CABAC data, an incorrect intra-mode-signaling
  bit order, a missing per-CTU `end_of_slice_segment_flag`, an
  availability check that used picture-bounds instead of real z-scan
  order (ITU-T H.265 6.4.1), and an uninitialized-memory bug in the
  reference-sample-substitution code.

**Honest current limit**: uniform/flat and low-detail content decodes
correctly on real ffmpeg (near-lossless PSNR at low QP, e.g. ~56 dB luma
on a 32×32 near-flat test frame across 4 CTUs). Chroma (DC-only
prediction, DCT, always-diagonal scan) is close to lossless at low QP on
real detailed content too (e.g. ~62-69 dB). **Real, busy, multi-directional
luma content (a deliberately adversarial high-frequency test pattern) still
does not decode correctly against ffmpeg even after the five fixes above**
— the visible error is much smaller than before the fixes (no longer a
full bitstream desync; the mismatch is now block-local and correlates
with directional-mode-heavy regions) but it is not yet resolved, and the
root cause was not isolated in the time available this session despite
extensive additional debugging (residual round-trip fuzz-tested clean in
isolation; scan-table/transform-matrix data verified against ffmpeg's
source; the investigation had narrowed it to something specific to
mixed-mode chained multi-PU reconstruction, but not further). Treat HEVC
as **real, substantially-verified, but not yet fully correct on generic
content** — do not point real streaming clients at it yet. See the
session's final report (or `git log` on this branch) for the full
debugging trail if resuming this.

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

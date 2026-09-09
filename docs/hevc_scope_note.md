# H.265/HEVC — scope note for whoever picks this up

Branch marker only, as of this commit — no implementation yet. This file
exists so the next person doesn't have to rediscover what's already
known. See `docs/DEVLOG.md` §6 on `main` for the full writeup; summary
here:

## Current state

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

## Why this is a real project, not a quick fix

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

## What already exists and might be reusable

- The GPU compute pipeline (`gpu_compute.c`, the shaders in
  `approach1-compute-encoder/shaders/`) does motion estimation,
  intra/inter prediction, DCT, and quantization in a way that's not
  inherently H.264-specific — `encoder_h265.c`'s stub already calls
  `gpu_compute_dispatch_encode()` successfully and gets real coefficient/
  motion-vector data back (it just throws it away). The transform-and-
  quantize side of the pipeline may need real adaptation for HEVC's
  different transform sizes (4x4 up through 32x32, vs. H.264's fixed
  4x4/8x8) and different quantization matrices, not a straight reuse.
- `bitstream.c`'s low-level bit-writer (the register-accumulator version
  from the performance work, `DEVLOG.md` §4) is codec-agnostic and should
  be directly reusable for HEVC's own bitstream syntax.
- The board/test infrastructure (`tools/quality_test.sh`,
  `tools/perf_test.sh`, the `BC250_PERF_STATS`/`BC250_DEBUG_*` diagnostic
  pattern) generalizes directly — `quality_test.sh` in particular should
  be parameterized to test `-c:v hevc_vaapi` as well as `h264_vaapi` once
  there's something real to test, rather than staying H.264-only forever.

## Explicitly deferred, not done

The cheap alternative — stop advertising `VAProfileHEVCMain` until this
is real, the same honesty principle already applied to
`VAConfigAttribEncPackedHeaders` in `va_backend.c` — was considered and
explicitly not done, at the project owner's direction, in favor of
tracking this as real future work. `main`'s README and `DEVLOG.md` both
carry the current warning against enabling HEVC in the meantime.

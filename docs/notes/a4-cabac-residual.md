# A4 — `hevc_cabac_code_residual_4x4()`

Backlog item A4, worked 2026-09-21. Off-board only (no BC-250 available),
dev machine AMD Ryzen 7 7700X, WSL2, GCC 13.3.0.

**Result in one line:** the 5.9% figure was an *under*estimate on detailed
content — direct instrumentation puts the function at **6.5–11.5% of CPU
HEVC frame time** depending on content and QP — and batching the sign bits
into one `encode_bypass_bins()` call makes it **14.2–19.3% faster**,
byte-identical, with two further ideas measured at zero or negative and
reverted.

---

## 1. The measurement, before any code was changed

A4 was sized at "~5.9% of profile". That is the same number gprof gave for
`bs_rbsp_to_ebsp` in item B2, which direct instrumentation put at 0.93%
after 111 lines had already been written against it, so the first job was
to get a real number.

`tools/hevc_cabac_bench.c` (new, CMake targets `hevc_cabac_bench` and
`hevc_cabac_bench_prof`) drives the real frame path —
`hevc_encoder_encode_raw()`, which is GPU-free — and measures the function
**two independent ways**:

- **`share`**: an rdtsc bracket around each call in the profile build. The
  rdtsc-pair cost is calibrated in-process (median of 201 batches of 1024,
  ~28.8 cycles here) and subtracted; both the raw and corrected figures are
  printed so the correction is visible.
- **`ablate`**: the function is switched to an immediate return and the
  whole-frame time is remeasured, ABBAABBA-interleaved in one process
  against the un-ablated side. This *includes* the downstream cost of the
  bytes it writes, so it should read slightly higher than `share`, and it
  does.

It is a CMake target, not a `gcc -O2` line, so it gets the shipped
`-O3 -DNDEBUG` plus `-march=znver2 -mtune=znver2` and the pinned
`-falign-functions=64 -falign-loops=32`, exactly like `cavlc_bench`.

The profile hook in `hevc_cabac.c` lives entirely inside
`#ifdef HEVC_CABAC_PROFILE` and wraps the function rather than editing its
body, so the thing being timed is the thing that ships. The shipped
`hevc_cabac.c.o` is **byte-identical** before and after the hook was added
(md5 `4053dc918a23732c1aaad2cb11d12b1f` both sides), same check
`cavlc.c`'s `CAVLC_PROFILE` hooks get.

### Share of CPU HEVC frame time, 1280x720, before the change

Content generated with ffmpeg (`testsrc2`, `smptebars`, `mandelbrot`), 4
frames, all-intra, RC_CQP.

| content | QP | bytes/frame | calls/frame | nonzeros/call | `share` (rdtsc, corrected) | `ablate` (best / median) |
|---|---|---|---|---|---|---|
| testsrc2 | 22 | 56 889 | 13 625 | 5.08 | **9.57%** | 9.17% / 11.32% |
| testsrc2 | 27 | 45 033 | 7 514 | 6.79 | **7.97%** | 9.23% / 10.18% |
| testsrc2 | 32 | 37 141 | 7 138 | 5.32 | **6.64%** | — |
| mandelbrot | 22 | 72 056 | 22 022 | 4.01 | **11.30%** | — |
| mandelbrot | 27 | 58 160 | 16 080 | 4.17 | **9.67%** | 11.46% / 11.05% |
| mandelbrot | 32 | 46 551 | 12 419 | 3.94 | **7.69%** | — |
| smptebars | 27 | 16 370 | 2 725 | 1.57 | **0.79%** | 0.08% / −0.38% |

The two methods agree within ~1.5 points everywhere, and agree that
`smptebars` is ~zero. **The A/A floor of the `ablate` rig is ±0.6% (best)
to ±0.9% (median)** — the four A/A rows measured 1.0060/0.9973,
1.0047/0.9916, 0.9976/0.9945, 1.0012/0.9937.

So 5.9% was in the right neighbourhood but low: on detailed content the
function is 8–11.5% of frame time, and the work is justified.

**Content dependence is the headline, not the average.** Across the
synthetic patterns the share spans 0.65% (`smptebars` QP 32) to 41.7%
(pseudo-random noise, QP 22) — a 60x range on the same code. Any single
number for "the share of residual coding" is meaningless without the
content, which is probably how 5.9% arose in the first place.

## 2. Rig validation

Three rigs were used and all three were validated against themselves.

| rig | what it compares | A/A floor |
|---|---|---|
| `ablate` (in-process, whole frame) | normal vs ablated frame time | **±0.6% best, ±0.9% median** |
| `share` (in-process, rdtsc, ~300k samples/run) driven ABBAABBA across two processes | cycles/call, binary A vs binary B | **−2.30%** |
| whole-frame A/B across two binaries, ABBAABBA | ms/frame, binary A vs binary B | **+0.73%** |

The middle rig is the one the optimisation decisions were made on, so
**−2.30% is the floor that every "measured" claim below is judged
against.** Its A/A was run with the same binary copied to two paths
(`base_prof` / `base_prof2`, identical md5).

## 3. What shipped: batch the sign bits

`residual_coding()` emitted the sign of every coded coefficient as
`num_nonzero` separate `encode_bypass()` calls, via an `int16_t sign[16]`
staging array. The signs now accumulate into one MSB-first bitmask in the
significance loop and go out as a single `encode_bypass_bins()` call.

This is identical arithmetic, not an approximation. n successive
`encode_bypass()` calls compute
`low = low<<n + range*(b0<<(n-1) | b1<<(n-2) | ...)`, which is exactly what
`encode_bypass_bins(value, n)` computes; the 8-bit chunking inside
`encode_bypass_bins` is what keeps `low` inside 32 bits between byte
emissions. x265 codes them the same way
(`Entropy::encodeBinsEP(signbits, numNonZero)`). What it buys is
`num_nonzero − 1` fewer `bits_left` tests and write-out branches per coded
block, plus the loss of the staging array.

### Measured, on the function itself (cycles/call, 7 rounds/side ABBAABBA)

| content | QP | base | after | delta |
|---|---|---|---|---|
| testsrc2 | 27 | 809.9 | 684.1 | **−15.53%** |
| mandelbrot | 27 | 458.3 | 385.4 | **−15.91%** |
| testsrc2 | 22 | 543.9 | 439.1 | **−19.27%** |
| smptebars | 27 | 201.0 | 172.4 | **−14.23%** |

Against a −2.30% A/A floor, and consistent in sign and magnitude on all
four content points with no overlap between the two distributions.

### Measured, on whole CPU HEVC frame time

| content | QP | B/A median | A/A floor |
|---|---|---|---|
| testsrc2 | 27 | **−1.29%** | +0.73% |
| mandelbrot | 27 | **−2.62%** | +0.73% |

14 rounds per side. **This is at or below the repo's 2.5% whole-frame
bar, and it is stated that way deliberately**: a 15% cut to something that
is 8–10% of the frame cannot be more than ~1.5% of the frame, and claiming
otherwise would be the arithmetic mistake, not the measurement. The
function-level number is the real result; the frame-level number is what
it is worth.

The share afterwards, re-measured with both methods on the final build:

| content | QP | `share` before | `share` after | `ablate` before | `ablate` after |
|---|---|---|---|---|---|
| testsrc2 | 27 | 7.97% | **6.50%** | 9.23%/10.18% | 8.10%/7.70% |
| mandelbrot | 27 | 9.67% | **8.02%** | 11.46%/11.05% | 9.29%/10.38% |
| testsrc2 | 22 | 9.57% | **7.96%** | — | — |

### Correctness

Byte-exactness is the bar, and the change clears it:

- `tools/hevc_host_drift.sh` — **HOST DRIFT PASS (38 cases byte-exact)**,
  intra and inter, luma and chroma, against ffmpeg's decoder.
- `ctest -R HevcEncodeBitstreamTest` — passes, md5 of
  `bc250_test_stream.hevc` unchanged at `3f2d00acec79afa32cced4e53e298dae`.
- `hevc_cabac_bench emit` — **13/13 streams byte-identical** to the
  pre-change binary (testsrc2/mandelbrot/smptebars x QP 22/27/32, plus the
  four synthetic patterns at 320x192 QP 22, which is where the dense
  all-16-nonzero blocks live).
- ffmpeg decodes all 13 silently.
- Full `ctest` passes except `VaApiDriverTest`, which fails identically on
  the unmodified tree in WSL: `VK_ERROR_OUT_OF_DEVICE_MEMORY` from
  `gpu_compute.c:1623` on image allocation. Not related to this change.

## 4. Measured zero — three things, reverted

All three were byte-identical and all three are recorded as
"MEASURED AND REJECTED" comments at the relevant point in `hevc_cabac.c`,
so the next person finds them before re-deriving them.

**(a) Branchless `encode_bin()`.** Compute both the MPS and LPS arms and
select with cmov, so the "did this bin go the way the context model
expected" test cannot mispredict. GCC does emit the cmovs (9 in
`residual_4x4`). Measured **+0.79%, +2.53%, +0.99%, −4.31%** — inconsistent
in sign, inside the 2.30% floor. At `-O3 -march=znver2` the branch is
predicted well enough that removing it only pays for the unconditional
`clz`. *Still worth one board run* — Zen 2 at BC-250 clocks has a different
mispredict-to-ALU ratio.

**(b) Branchless significance-loop store.** Store
`abs_coeff[num_nonzero]` unconditionally, `num_nonzero += sig`,
`sign_bits = (sign_bits << sig) | (val < 0)`, on the theory that `if (sig)`
is a coin flip. Measured **+4.93%, +4.18%, +3.41%, −1.17%** — i.e. a real
*loss* on three of four points. The unconditional store costs a store-queue
slot on every scan position including the ~50% that are zero.

This one is worth noting as a process point: it was originally bundled with
the sign batching, and the bundle measured −13.6 to −16.5%. Decomposing it
showed sign batching alone was −14.2 to −19.3% and the branchless store was
giving some of it back. **The combined number was hiding a regression.**

**(c) `g_hevc_sig_ctx4` pre-permuted by scan.** A 48-byte table so the
significance loop's context load is indexed by `sp` instead of by
`scan[sp]`, removing a load-to-load dependency. Measured **−0.17%, +0.45%,
+3.34%, +2.21%** — zero. `sp` is a loop induction variable, so the address
chain is known many iterations ahead and those loads were never on the
critical path.

## 5. Still open

- **Board run of (a).** The only one of the three rejects with a plausible
  mechanism for behaving differently on BC-250 silicon.
- **The remaining ~690 cycles/call** (testsrc2 QP 27) are roughly 30
  context-coded and bypass bins, i.e. ~23 cycles/bin. No internal stage
  ablation was built, so *where inside the function* those cycles sit is
  still unmeasured. That is the next thing to build if A4 is reopened —
  the `cavlc_bench profile` ablation pattern applied to the six syntax
  stages here.
- **A SIMD last-significant search** (pack the 16 coefficients to a
  nonzero byte mask, `pshufb` by the scan table, `movemask`, `31−clz`) would
  make the whole backward search branch-free in ~6 instructions. Not tried:
  it needs SSSE3, which the generic x86-64 fallback build does not
  guarantee, and this repo deliberately removed its previous CPU-SIMD layer.
- **`hevc_cabac_code_residual()`** (the >4x4 coder used by the GPU
  reconstruction path) has the same sign loop and would take the same
  change. Not touched here: it is not on the CPU path A4 is about, and it
  has no off-board frame-level harness.

## 6. Using the harness

```bash
cmake -S approach1-compute-encoder -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j12 hevc_cabac_bench hevc_cabac_bench_prof

ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 8 \
       -pix_fmt nv12 -f rawvideo /tmp/testsrc2_720.nv12

./build/hevc_cabac_bench_prof stats  --input=/tmp/testsrc2_720.nv12 --qp=27
./build/hevc_cabac_bench_prof share  --input=/tmp/testsrc2_720.nv12 --qp=27
./build/hevc_cabac_bench_prof ablate --input=/tmp/testsrc2_720.nv12 --qp=27 --samples=11
./build/hevc_cabac_bench      bench  --input=/tmp/testsrc2_720.nv12 --qp=27
./build/hevc_cabac_bench      emit   --input=/tmp/testsrc2_720.nv12 --qp=27 --out=/tmp/x.hevc
```

Read `stats` before trusting any number from the other modes: the share
this function has is a function of how many coefficients survive
quantisation, and `stats` is what makes that checkable rather than
asserted. Always quote the content and the QP with the figure.

# A5 — `hevc_choose_luma_mode()`

Backlog item A5, worked off-board on a dev machine (AMD Ryzen 7 7700X,
WSL2, GCC 13.3). No BC-250 board was available, so **every number here is a
dev-machine number** and none of it has been confirmed on board silicon.
That matters in a known direction: C2 measured the same class of change
(divisions, call overhead, redundant work) at **+44.5% on the dev machine
and +132% on the board**, so a dev-machine figure for this kind of work
understates rather than flatters.

## Headline

| | |
|---|---|
| Real share of `hevc_choose_luma_mode` | **~24% of CPU HEVC encode**, not the 7.8% in the backlog |
| Change | byte-identical, `-O3 -march=znver2` |
| Result | **+12.9%** end-to-end at 1080p all-intra (+7.0% to +14.9% across five cases) |
| A/A floor | **1.0%** (median), 2.6% (min), n=16/side |
| Correctness | `HOST DRIFT PASS (38 cases byte-exact)`, `HevcEncodeBitstreamTest` pass, bitstream checksum identical on all five measured cases |

A decision-changing mode search was **priced and not done** — see the last
section. The measured ceiling for it is *below* what the byte-identical
change already delivered.

## 1. The 7.8% figure was wrong, in the unusual direction

The standing rule is "do not size a task off a small gprof percentage"
because gprof has misattributed three times here. This is the fourth, and
the first one that reads *low*: A5 is worth roughly three times what the
backlog says.

Two independent off-board measurements, both at the shipped
`-O3 -march=znver2`, both on 1080p `testsrc2`, 8 frames, all-intra, QP 27:

| method | share of total encode |
|---|---|
| **ablation** — run the mode search twice per 4x4 PU, assert the second answer matches, discard it; the delta is the marginal cost of exactly one search | **24.27%** |
| **`rdtsc` around the real call** — 1,044,480 calls, 301.8–312.7 cycles/call | **24.07%** |
| A/A control for the ablation rig (identical work both sides) | −0.36% |

The two estimates bracket the answer from opposite sides. The ablation's
second call runs with the block's data already hot, so it *under*states a
cold first call; the `rdtsc` figure *includes* its own probe overhead, so it
overstates. They land 0.2 points apart, which is the useful part — a single
number from either one alone would have been worth much less.

At QP 40 the same pair reads 23.30% / 24.80%. On the inter path (720p,
gop 6) it is 15.62% / 15.89%, lower because P-frames skip most CUs.

**gprof's error here is not a rounding error.** 7.8% versus 24% would have
made this look like a marginal item; it is the single largest identified
cost in the CPU HEVC path after CABAC.

## 2. The rig

`tools/hevc_bench.c`, a new CMake target pair (`hevc_bench`,
`hevc_bench_prof`), built exactly the way `tools/cavlc_bench.c` is and for
exactly the same reason: `tools/hevc_host_drift.sh` already reaches this
code off-board but builds it by hand at `-O2`, and this repo has been burnt
in both directions by `-O2` figures. As a CMake target it inherits the
shipped `-O3 -DNDEBUG` plus `-march=znver2 -mtune=znver2`, with
`-falign-functions=64 -falign-loops=32` pinned — the two flags that settled
the 1.30x alignment phantom the last time a rig here lied.

- `hevc_bench bench` times `hevc_encoder_encode_raw()` only (clip generation
  and allocation are outside the timer) and prints an FNV-1a checksum over
  every byte of every frame. **A timing claim on this path is only a result
  if that checksum is unchanged**; it also refuses to report at all if two
  samples inside one run disagree.
- `hevc_bench_prof profile` is the ablation above. It is a separate binary
  so `bench` numbers are taken on code carrying no instrumentation.
- The ablation hooks in `src/hevc_intra.c` sit behind `#ifdef
  HEVC_INTRA_PROFILE` and are a recursive self-call with the flag cleared,
  not a restructure into an inner function plus a wrapper — deliberately, so
  the shipped code is not reshaped for the profiler's benefit. Verified by
  disassembly diff: the shipped object is **byte-identical** to one built
  from a copy with the hook block and both call sites physically deleted.
- A/B is driven at process level in **ABBAABBA** order (plain alternation
  gives the first side of each pair a reproducible penalty), 4–8 rounds per
  case, comparing medians.

**Rig validation, stated before the result.** Two identical binaries at
different paths, same case, n=16 per side:

```
A n=16 min=323.317 median=330.743 max=344.851 sd=1.85%
B n=16 min=314.800 median=334.108 max=348.302 sd=2.95%
B/A median = 1.0102x  (-1.02%)     B/A min = 0.9737x  (+2.63%)
```

So the floor is **±1% on the median and ±2.6% on the min**, consistent with
the project's standing "nothing under ~2.5% is a result". The median is the
better estimator here and is what the table below quotes.

## 3. What changed (all byte-identical)

Three things, none of which touches the decision:

1. **The fifth gather and the fifth prediction are gone.** `encoder_h265.c`
   called `hevc_choose_luma_mode()` and then immediately
   `hevc_predict_4x4()` with the mode it returned. Nothing writes `recon_y`
   between the two, so that second call re-ran `gather_neighbors()` — five
   z-scan availability tests over an 11-entry substitution scan — and re-ran
   a prediction the search had already computed and thrown away.
   `hevc_choose_luma_mode()` now hands back the winning mode's 16 samples
   in an out-parameter. Cost: at most three 16-byte copies per block.
2. **The source block is hoisted out of the candidate loop.** The four SAD
   loops each re-derived `src_y[(y0+y)*stride + (x0+x)]` for all 16 samples
   — 64 strided byte loads off a row multiply. It is now four 4-byte copies
   into a contiguous 16-byte local, once per block.
3. **The candidate loop is unrolled with the mode a compile-time constant**,
   and `predict_from_refs()` is `inline`, so each candidate collapses to
   just that mode's arithmetic instead of a runtime `switch`. With both
   SAD operands contiguous, GCC's vectoriser then reduces each SAD to a
   single packed instruction: the function contains **4 `vpsadbw`** where
   the old one had none.

Everything else is unchanged — same gather, same prediction arithmetic,
same SAD value, same strict `<` so ties still go to the earlier candidate,
same Planar/DC/Horizontal/Vertical order. The SAD accumulator narrowed from
`long` to `int`, which cannot change an outcome (a 4x4 SAD tops out at
16×255 = 4080).

Measured per step, 1080p `testsrc2` all-intra QP 27, against the same
baseline: step 1 alone **+9.7%**; steps 2+3 add **+3.3pp** on top.

## 4. Results

All five cases byte-identical (bench checksum equal on both sides).

| case | A median | B median | delta |
|---|---|---|---|
| A/A floor (identical binaries) | 330.7 ms | 334.1 ms | −1.0% |
| 1080p `testsrc2`, all-intra, QP 27 | 326.7 ms | 284.5 ms | **+12.9%** |
| 1080p `testsrc2`, all-intra, QP 40 | 324.9 ms | 276.6 ms | **+14.9%** |
| 720p `testsrc2`, gop 6 (inter path) | 81.4 ms | 74.8 ms | **+8.1%** |
| 1080p synthetic pattern 3 (noise), all-intra, QP 27 | 592.3 ms | 551.1 ms | **+7.0%** |

The spread is the expected shape, not noise: the mode search is a fixed
per-PU cost, so its share falls when the rest of the frame gets more
expensive. Pattern 3 is pseudo-random blocks, where CABAC residual coding
dominates (592 ms versus 327 ms for `testsrc2` at the same size and QP), and
the inter path skips most CUs entirely.

Function-level cross-check, same case: **301.8 → 229.0 cycles per call**
(−24%) by `rdtsc`, *and* the function now also does the work
`hevc_predict_4x4()` used to do separately. 190 saved cycles × 1,044,480
calls ÷ 4.08 GHz ≈ 48 ms of a ~325 ms encode ≈ 15%, against 12.9% measured
end to end. The two agree within the noise floor, which is the point of
computing both.

## 5. Correctness

- `tools/hevc_host_drift.sh` — **`HOST DRIFT PASS (38 cases byte-exact)`**,
  luma and chroma, against ffmpeg's decode. This is the oracle that does not
  share our code.
- `ctest -R HevcEncodeBitstreamTest` — pass (a valid byte oracle,
  deterministic; it is `RC_CQP` so the `CLOCK_MONOTONIC` bucket-drain
  problem that makes `test_encode` unusable does not apply).
- Full `ctest`: 6/7. `VaApiDriverTest` fails **identically on the unmodified
  baseline** in this environment — WSL2 has no Vulkan device, so
  `vaCreateSurfaces` returns `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Not caused by
  this change and not a GPU-path claim either way.
- `hevc_bench`'s own checksum: identical A vs B on all five cases above,
  which covers intra and inter, two QPs, two resolutions and two contents.

## 6. The GPU shader — explicitly, the two paths were never equal

`shaders/hevc_intra_wavefront.comp` is **not** a second implementation of
this function, and nothing here makes the two agree any less than they did.
The shader searches 35 modes over one undivided 16x16 CU with 8.4.4.2.3
reference smoothing; the CPU searches four modes per 4x4 TU. Its own SCOPE
comment says so: "Most of this file has no second implementation in the tree
to be bit-exact against." The byte-exactness this project relies on is each
path against **ffmpeg's decode of its own bitstream**, not against the
other, and that property is untouched — the CPU path is still byte-exact on
38 drift cases and the shader was not modified.

Worth stating because `docs/hevc-shader-audit.md` (A3) records that the
shader's *mode search* is invisible to every oracle in the repo. That is
still true, and this change neither helps nor hurts it.

## 7. Deliverable (2) — priced, and deliberately not done

The backlog frames A5 around a cheaper first-pass metric or early
termination, with the warning that it becomes a compression change needing
BD-rate on the board. Rather than implement one, I measured what any such
change could *at most* buy, using two throwaway builds whose output is
wrong on purpose (both A/B'd against the optimised tree, same rig):

| decision-changing variant (output CHANGES) | delta on top of the change above |
|---|---|
| 4 candidates → 2 (Planar + DC) | +6.7% |
| **no mode search at all** (Planar everywhere) — the absolute ceiling | **+10.3%** |

**Deleting the mode search entirely buys less than keeping it and making it
faster did.** A realistic two-candidate scheme is worth 6.7%, for an
unquantified BD-rate cost that cannot be measured off-board. So there is no
version of deliverable (2) that is a better trade than what already landed,
and none was written.

## 8. Measured zeros, recorded so nobody redoes them

- **The neighbour-substitution scan in `gather_neighbors()` is free.**
  Deleting it outright (the `first` search plus both fixup loops — output
  intentionally wrong, timing only) measured **+0.87% median / −2.05% min**,
  i.e. inside the A/A floor. Any clever rewrite of that scan — bitmask,
  `ctz`, group-wise substitution, an all-available fast path — is bounded by
  a number that is already zero. Do not spend time on it.
- **`gather_neighbors()` as a whole is ~5.3% of encode**, measured by
  calling it a second time per PU and discarding the result
  (byte-identical, +5.26% median). Since the scan inside it is free, that
  cost is the five z-scan availability tests and the strided sample loads.
  That is the remaining lever in this function, and it is worth at most
  5.3% even if it could be driven to zero, which it cannot.

## 9. What is left here, for whoever picks it up

- **Cb and Cr gather the same availability twice.** `hevc_predict_4x4()` is
  called back to back for Cb and Cr at the same `(cx, cy)` with the same
  dimensions, so `gather_neighbors()` runs its five z-scan availability
  tests twice for an answer that cannot differ — only the sample values do.
  Sharing the availability between the two planes is byte-identical by
  construction. Smaller than the luma case (2 gathers per CU against 4) and
  outside A5's scope, so it was not done.
- **This all needs a board run.** Per C2, this class of change is worth
  substantially more on BC-250 silicon than on a modern desktop core, so
  +12.9% is a floor, not the number to publish. `lab bench` / `lab compare`
  on the board, and `lab gate` for the byte-exactness the dev machine
  already shows.

## 10. Reproducing

```bash
cmake -S approach1-compute-encoder -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target hevc_bench hevc_bench_prof

ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 8 \
       -pix_fmt nv12 -f rawvideo t2_1080.nv12

# where the time goes (ablation + rdtsc, with an A/A control)
./build/hevc_bench_prof profile --size=1920x1080 --qp=27 --frames=8 \
                                --samples=7 --input=t2_1080.nv12

# timing + byte-identity checksum
./build/hevc_bench bench --size=1920x1080 --qp=27 --frames=8 \
                         --samples=5 --input=t2_1080.nv12
```

A/B is two build directories of `hevc_bench` run ABBAABBA and compared on
the median; validate the pair against itself first and quote the floor.

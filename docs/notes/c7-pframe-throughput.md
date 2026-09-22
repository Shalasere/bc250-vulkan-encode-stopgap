# C7 follow-up: moving the P-frame skip decision onto the GPU

Status: **implemented, off-board verified where possible, gated behind the
same `BC250_HEVC_GPU_PFRAME=1` opt-in c7-gpu-pframes.md already used.** No
board run has happened. Read `docs/notes/c7-gpu-pframes.md` first - this
file assumes everything in it and only changes WHERE the P-frame skip
decision runs, not what it decides or the correctness invariants that
feature already established.

## The task

`docs/backlog.md`'s C7 entry recorded the first board throughput numbers
for `BC250_HEVC_GPU=1 BC250_HEVC_GPU_PFRAME=1` at 1280x720/gop=30:

| mode | fps |
|---|---|
| GPU intra-only (baseline) | 68.6 / 72.0 / 73.1 |
| GPU intra + P-frame | 35.3 / 34.2 / 34.5 |

Enabling P-frames cost roughly half the throughput, and the entry named
three options: a cheaper skip decision, avoiding the full source download,
or accepting the cost for the bitrate win. This is the first of those.

## 1. Confirming the cost model before building anything

Read `hevc_encoder_encode_frame()`'s GPU branch and `decide_gpu_ctu_skips()`
in `approach1-compute-encoder/src/encoder_h265.c` (pre-this-change state,
still visible in git history) and `gpu_compute_hevc_dispatch_intra()` /
`gpu_compute_hevc_download_recon_nv12()` / `gpu_compute_download_nv12()` in
`approach1-compute-encoder/src/gpu_compute.c`. The prompt's own hypothesis
was accurate; here is what the code actually did, per P-frame candidate
(i.e. every non-IDR frame once `BC250_HEVC_GPU_PFRAME=1`):

1. `gpu_compute_dmabuf_sync_start()` + `gpu_compute_download_nv12()` -
   downloads the CURRENT SOURCE frame from the VA-API surface to
   `encoder->dl_y`/`dl_uv` (a `vkMapMemory` + row-by-row `memcpy`, i.e. a
   real GPU-CPU synchronization point, since the destination has to be
   current). At 1280x720 that is `1280*720 + 1280*720/2` = 1,382,400 bytes
   NV12 (~1.32 MiB) - the "~1.66 MB" figure in the task brief was the round
   number quoted from the board review; the exact math is smaller but the
   same order of magnitude and the same shape (one full-frame readback).
2. `gpu_compute_hevc_download_recon_nv12()` - a second, equally-sized
   readback of `ctx->recon_image` (the previous frame's reconstruction),
   de-interleaved from packed NV12 UV into planar Cb/Cr afterward on the
   CPU (`encoder->prev_recon_cb/cr`).
3. `decide_gpu_ctu_skips()` - a CPU loop over every CTU
   (`width_ctu * height_ctu` = 80*45 = 3600 CTUs at 720p), each calling
   `compute_sad_8x8_luma()` four times and `compute_sad_4x4_chroma()` four
   times (real SIMD SAD code, but still O(pixels) CPU work: 3600 * (4*64 +
   4*32) = ~1.38M byte compares).

Only after all three does `gpu_compute_hevc_dispatch_intra()` get called at
all (with the resulting mask as its `skip_mask` parameter). The all-intra
baseline does none of this - `hevc_encoder_encode_frame()`'s GPU branch
skips straight to the dispatch. This confirms the brief's hypothesis
exactly: two full-frame host readbacks (each a real synchronization point,
not overlapped with anything) plus a real CPU compute pass, all novel cost
that scales with PIXELS, added in front of a shader dispatch that itself
does not need any of it, run in program order relative to the eventual
`gpu_compute_begin_picture()`/dispatch - i.e. this is on the frame's
critical path, not something that could already be hidden by GPU/CPU
overlap.

## 2. Design: read where the data already lives, write only the verdict

### The mask's two consumers, and why the GPU write can't be the only copy

`hevc_intra_wavefront.comp`'s binding 7 (`skip_mask`) is a plain SSBO read
- whatever last wrote it is what the shader sees, whether that was a host
`memcpy` or a compute shader. That consumer can move to the GPU cleanly.

But `encode_core_gpu()` (the CPU CABAC entropy coder) *also* reads
`encoder->gpu_ctu_skip[]` directly, for `cu_skip_flag`'s ctxInc (needs to
know whether the left/above CTU was itself a skip), `merge_idx` signalling,
and the DC-for-skip-neighbour MPM rule. That is inherently host-side
entropy coding - CABAC's context state lives on the CPU in this project (see
`CLAUDE.md`'s framing: "CABAC... the one part that must stay serial on the
CPU") - so this task does not attempt to move it, and the mask cannot be
GPU-only. It has exactly one required host round trip: the decision itself,
`width_ctu * height_ctu` `uint32_t`s (3600 at 720p = 14,400 bytes, not 3600
- see the correction in the numbers table below), read back ONCE after the
frame's fence signals, instead of ~2.7 MB of raw pixels read back BEFORE
the shader can even be dispatched.

### New shader: `hevc_pframe_skip.comp`

Added alongside `hevc_intra_wavefront.comp`. One workgroup (64 threads) per
CTU, dispatched as `(width_ctu, height_ctu, 1)` - unlike the wavefront
shader's diagonal schedule, every CTU's skip decision is independent of
every other CTU's (it only ever compares that CTU's own source samples
against its own co-located reference samples), so there is no same-frame
ordering dependency here at all and the whole grid runs in one dispatch,
no per-step barriers.

Reads `srcY`/`srcUV`/`reconY`/`reconUV` (bindings 0-3) and writes
`skip_mask` (binding 7) - **the exact same bindings and the exact same
`hevc_wavefront_desc_set`** `hevc_intra_wavefront.comp` already uses; see
"Reusing the descriptor set" below for why that is deliberate, not
incidental. Threshold is a push-constant, computed on the host (see below),
not recomputed in the shader.

**Equivalence to the CPU formula it replaces** (also documented in the
shader's own top comment): `compute_sad_ctu_zero()` sums
`compute_sad_8x8_luma()` over four 8x8 luma tiles covering the CTU's 16x16
area, plus `compute_sad_4x4_chroma()` over four 4x4-per-plane tiles
covering the 8x8 chroma area. Summing the same abs-difference terms over
four disjoint tiles that exactly cover a region is the same total as
summing them over the whole region directly - the shader does the latter
(one 16x16 luma SAD, one 8x8 Cb SAD, one 8x8 Cr SAD, cooperatively across
64 threads with a standard shared-memory tree reduction) in a single pass
instead of four separate calls. Same arithmetic, different grouping, not
an approximation.

Source-edge clamping matches `srcLuma()`/`srcUV`'s use in
`hevc_intra_wavefront.comp` (clamp to `src_width`/`src_height - 1`), which
that shader's own comment already establishes as equivalent to the CPU
path's `pad_replicate()`. Reference (recon) reads are never clamped, on
either side, because `recon_image` is always allocated at the full
CTU-aligned coded size.

### One formula, one place

`decide_gpu_ctu_skips()`'s threshold formula (`4 * 96 * (1 + qp/8)`,
`BC250_HEVC_GPU_SKIP_THRESHOLD` override) is unchanged in VALUE, but was
extracted into a new shared helper, `hevc_gpu_pframe_skip_threshold(int
qp)`, in `encoder_h265.c`. Both `decide_gpu_ctu_skips()` (still used by the
off-board `hevc_encoder_encode_gpu_raw()` test path, which has no GPU to
dispatch a shader on) and the real board path (which now hands the
computed value to the GPU as a push constant) call this one function, so
the two callers cannot silently drift onto two different answers to "what
does the threshold mean" while this task changes "where does the decision
run." No value of this threshold can make the resulting bitstream
non-conforming - unchanged from c7-gpu-pframes.md - because a CTU marked
skip always reconstructs as an exact copy of the reference by construction,
in `hevc_intra_wavefront.comp`'s early return, which this change does not
touch.

### Reusing the descriptor set, and why that's what makes the barrier simple

`hevc_pframe_skip.comp` is dispatched through a **new pipeline**
(`ctx->hevc_skip_decide_pipeline`) but the **same descriptor set**
(`ctx->hevc_wavefront_desc_set`) `hevc_intra_wavefront.comp` uses. This
works because Vulkan descriptor-set-layout compatibility is by object
identity: `ctx->hevc_skip_decide_layout` (the new pipeline's
`VkPipelineLayout`) is built from the exact same `VkDescriptorSetLayout`
object (`ctx->hevc_wavefront_desc_layout`) the wavefront pipeline's layout
uses, so the same `VkDescriptorSet` can be bound to either pipeline. Bindings
0-3 (source/recon images) are already updated, every dispatch, before this
new code runs; binding 7 (skip mask) is the SAME buffer both shaders touch.
There is exactly one descriptor set to keep in sync, not two.

Concretely, in `gpu_compute_hevc_dispatch_intra()`, right where the old
code memcpy'd a host-provided mask into `hevc_skip_buffers[current_buf]`:

- If this is an all-intra frame (`!want_pframe_skip`) or the recon image
  was just (re)created (`recon_was_reset`, unchanged safety net from
  c7-gpu-pframes.md): the mask is zeroed by a plain host-side `memset`,
  same as before - no GPU dispatch, no behaviour change from the
  already-board-validated all-intra path.
- Otherwise: `vkCmdBindPipeline`/`vkCmdBindDescriptorSets` the new
  pipeline, push the 7-word constant block (coded width/height, CTU grid
  width/height, threshold, source width/height), `vkCmdDispatch(wc, hc,
  1)`, then **one `VkMemoryBarrier`** with `srcAccessMask =
  VK_ACCESS_SHADER_WRITE_BIT` and `dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  | VK_ACCESS_HOST_READ_BIT`, from `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` to
  `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT`.

That single barrier does two jobs at once, both real dependencies of this
same write: it orders the write before `hevc_intra_wavefront.comp`'s
binding-7 reads later in the SAME command buffer (the identical
compute-to-compute form `insert_compute_barrier()` already uses between
every other pair of dependent compute stages in this file - not a new
mechanism), and it makes the write visible to a HOST read that happens
later, after this frame's fence signals. **This is the one place in this
change I want to be explicit about instead of asserting confidence past
what I've verified**: every OTHER host readback in `gpu_compute.c`
(mode/coeff/cbf) goes DEVICE_LOCAL buffer -> `vkCmdCopyBuffer` into a
HOST_VISIBLE staging buffer -> read after the fence, and none of those
sites add an explicit `VK_ACCESS_HOST_READ_BIT`/`VK_PIPELINE_STAGE_HOST_BIT`
barrier - they rely on fence completion alone. `skip_mask` is the first
buffer in this file a compute shader writes directly into HOST_VISIBLE
memory with no intervening transfer command, so there was no existing
precedent to copy. Rather than extend that fence-only convention to a case
it was never established for, this uses the general, spec-documented
mechanism for "make a shader write visible to a future host read"
(combining `VK_ACCESS_SHADER_READ_BIT` and `VK_ACCESS_HOST_READ_BIT` in one
`dstAccessMask`, which is valid per the Vulkan synchronization model - a
barrier's scopes are OR'd, not required to be single-purpose). I am
confident this is *correct* Vulkan; I have not run it, because nothing in
this environment can execute a real or even software Vulkan device against
this exact path (see Verification below) - the fence-only convention
elsewhere in this file has real hardware behind its confidence and this
does not yet.

### Two ~1.3 MiB downloads become one 14.4 KB one, and only when needed

Numbers at 1280x720 (`width_ctu=80`, `height_ctu=45`, `nctu=3600`):

| | before | after |
|---|---|---|
| source NV12 download | ~1.32 MiB, every P-frame candidate | none |
| recon NV12 download | ~1.32 MiB, every P-frame candidate | none on the common path (see below) |
| CPU SAD loop | ~1.38M byte-compares, every P-frame candidate | none (moved to the GPU shader) |
| host round trip | ~2.7 MiB pixels | 3600 * 4 bytes = 14,400 bytes (mask only) |

### The recon download's one remaining job: the untested CPU fallback path

`c7-gpu-pframes.md` already flagged, honestly, an untested interaction:
"If a GPU P-frame candidate's dispatch or readback fails and
`hevc_encoder_encode_frame()` falls through to the CPU path
(`encode_core()`) for that same frame, the CPU path will find `has_ref`
true... and `prev_recon_y/cb/cr` already populated - by this same
feature's own pre-dispatch reference download." That download is exactly
the "full reference download" this task removes from the common path - so
removing it outright would have silently regressed an already-acknowledged,
already-fragile correctness path: `encode_core()`'s own zero-motion skip
(`encode_cu()`) reads `enc->prev_recon_*` as the literal reference pixels a
SKIP CU copies from, and normally keeps it fresh itself via an end-of-frame
`memcpy` - which never runs while the GPU path is handling frames. Without
a fresh `prev_recon_*`, a CPU-path SKIP CU in that fallback would copy from
stale data, and a real decoder (which always has a correct reference)
would reconstruct something different - a genuine conformance risk, not
just a quality one.

Fix: relocated the download (now `hevc_refresh_prev_recon_from_gpu()`, a
small helper, same body as before) to run **only** in the CPU-fallback
branch, right before `encode_core()` is called, gated on
`!is_idr_candidate`. The common path (dispatch succeeds, which is the
expected case) never pays for this download at all; the one case that
actually needs it still gets it, exactly as before - this relocates an
existing, already-acknowledged defensive measure, it does not add a new
untested one, and does not change what happens when the dispatch fails
(still falls through to `encode_core()`, still with a fresh reference when
the refresh succeeds).

### The rate-control estimate: a deliberate, honest approximation

`decide_gpu_ctu_skips()` also computed `enc->last_frame_sad`, the CPU
path's own convention for feeding `pick_frame_qp()` a "how much motion was
there" estimate for the NEXT frame's QP choice. This is a rate-control
heuristic, not a correctness value (same status as the skip threshold
itself - see `docs/backlog.md` C7's "Still needed" note on the threshold's
own untuned status). The exact CPU-computed total SAD is not available on
the host any more without the pixel downloads this change removes.
Replacement: after reading the mask back, count non-skip CTUs (a few
thousand single-bit checks on the already-local mask, not a pixel
operation) and use `(non-skip count) * threshold` as the estimate - every
non-skip CTU failed the threshold by definition, so this is an
order-of-magnitude-correct proxy in the same units, not the same number the
old code would have produced. This is a real, acknowledged behavioural
difference, scoped deliberately: the task said to reuse the threshold
formula unless the two were entangled, and this is exactly that kind of
entanglement (the CPU SAD total is a genuine side-effect casualty of moving
the SAD computation to the GPU) rather than a change to the skip decision
itself. It affects only future QP choice on the P-frame candidate path -
bitrate/quality, never conformance.

## What changed, by file

- `shaders/hevc_pframe_skip.comp` (new): the GPU-side decision. Compiles
  with `glslangValidator -V` and validates with `spirv-val`, both clean;
  builds through the real `compile_shaders` CMake target (confirmed by the
  presence of `hevc_pframe_skip.comp.spv` in a from-scratch `cmake && make`
  run, not just standalone validation).
- `src/gpu_compute.h`: new `hevc_skip_decide_pipeline`/`hevc_skip_decide_layout`
  fields; `hevc_skip_buffers[]`'s comment rewritten (host-write-once-input ->
  GPU-write/host-read-back); `gpu_compute_hevc_dispatch_intra()`'s
  `const uint32_t *skip_mask` parameter replaced with `bool want_pframe_skip,
  uint32_t skip_threshold`; new `gpu_compute_get_hevc_skip_data_slot()`
  getter, same slot contract as the existing mode/coeff/cbf getters.
- `src/gpu_compute.c`: `hevc_skip_decide_layout` created from the same
  `hevc_wavefront_desc_layout` object as `hevc_wavefront_layout`, and its
  pipeline loaded/created right after `hevc_wavefront_pipeline` (same
  "quiet unless it loaded and then failed" treatment - a missing `.spv`
  here just means P-frame skip can never be honoured, not that HEVC is
  unavailable). `gpu_compute_hevc_dispatch_intra()`'s skip-mask block
  rewritten as described above; new `gpu_compute_get_hevc_skip_data_slot()`;
  both new objects destroyed in the context teardown path.
- `src/encoder_h265.c`:
  - `hevc_gpu_pframe_skip_threshold()` (new): the threshold formula,
    factored out of `decide_gpu_ctu_skips()` so the real dispatch path and
    the off-board test path cannot compute two different answers.
  - `decide_gpu_ctu_skips()`: unchanged behaviour, now calls the shared
    helper; its doc comment updated to say plainly that it is CPU-only,
    used by `hevc_encoder_encode_gpu_raw()` alone now.
  - `hevc_refresh_prev_recon_from_gpu()` (new): the relocated recon
    download, used only by the CPU-fallback branch.
  - `hevc_encoder_encode_frame()`'s GPU branch: removed the source+recon
    download and the `decide_gpu_ctu_skips()` call; computes
    `skip_threshold` (pure arithmetic) instead; passes `!is_idr_candidate,
    skip_threshold` to the new dispatch signature; after sync, reads the
    mask back via `gpu_compute_get_hevc_skip_data_slot()` into
    `encoder->gpu_ctu_skip[]` and derives the `last_frame_sad` proxy; calls
    `hevc_refresh_prev_recon_from_gpu()` only in the CPU-fallback branch.
  - `gpu_ctu_skip`'s field comment updated to describe the new data flow
    (GPU-computed, copied back, rather than host-computed then uploaded).

## Verification

### What actually ran, off-board

Same environment constraints as c7-gpu-pframes.md: no board, no working
Vulkan device in this environment (`test_va_api`/`VaApiDriverTest` still
fails on llvmpipe with the same pre-existing OOM this session started with
- reproduced again below, unchanged, to confirm it is not a new failure).

1. **The full project builds clean from scratch.** `rm -rf` the build
   directory, fresh `cmake` + `make -j$(nproc) all`, covering
   `bc250_drv_video`, every `tests/` target, and `hevc_bench`/
   `hevc_cabac_bench`/`hevc_host_repro`/`rc_bench`/`cavlc_bench`. Zero
   compiler warnings (checked by grepping the full build log for
   `warning:` and finding none besides `make`'s own unrelated clock-skew
   notices from the Windows-filesystem-backed WSL mount). This exercises
   every call site of the two changed signatures
   (`gpu_compute_hevc_dispatch_intra()`, one call site;
   `gpu_compute_get_hevc_skip_data_slot()`, new) - a real signal that
   nothing was left on the old signature.
2. **The new shader compiles and validates standalone**, in addition to
   building through `compile_shaders`: `glslangValidator -V
   hevc_pframe_skip.comp` succeeds, `spirv-val` on the result reports
   nothing.
3. **`ctest` passes everything except the pre-existing, environment-caused
   `VaApiDriverTest` failure**: `CavlcHarnessRoundTrip`,
   `CavlcHarnessRoundTripTypical`, `BitstreamTest`, `CavlcUnitTest`,
   `EncodeBitstreamTest`, `HevcEncodeBitstreamTest` all pass.
   `VaApiDriverTest` aborts with the same `VK_ERROR_OUT_OF_HOST_MEMORY`
   (Vulkan error -2) retry-and-fail sequence on `llvmpipe` this session's
   brief said was already confirmed, by multiple prior agents, unrelated to
   any encoder change - not a regression from this work.
4. **`tools/hevc_host_drift.sh` - 53/53 cases, `HOST DRIFT PASS`, unchanged.**
   This is the CPU path's own byte-exact oracle against ffmpeg's real
   decode, and this task was not supposed to touch the CPU path
   (`encode_core()`) at all - it didn't, and this confirms it.
5. **A scratch off-board harness exercising `hevc_encoder_encode_gpu_raw()`**
   (not committed, same status `c7_pframe_test.c` had in
   c7-gpu-pframes.md), reproducing that design doc's own strongest
   check after this refactor: `BC250_HEVC_GPU=1 BC250_HEVC_GPU_PFRAME=1`,
   one IDR frame then one P frame with the reference set to the IDR
   frame's own (uniform) source, at 32x32/64x64/100x60/128x128. Every size:
   encode succeeds, the resulting two-frame bitstream decodes through
   ffmpeg with **zero** `-v error` output (a real independent decoder,
   silent), and **the decoded P-frame is byte-identical to the decoded IDR
   frame** in all four cases - the same "fully-skipped P-frame reconstructs
   as an exact copy of its reference" property c7-gpu-pframes.md's original
   bug hunt established, still holding after `decide_gpu_ctu_skips()`'s
   threshold formula was refactored into a shared helper and
   `gpu_compute_hevc_dispatch_intra()`'s signature changed. This exercises
   `decide_gpu_ctu_skips()` (and therefore `hevc_gpu_pframe_skip_threshold()`)
   and all of `encode_core_gpu()`'s P-slice signalling - everything this
   task's refactor could plausibly have disturbed on the CABAC/signalling
   side.

### What this does NOT cover (same limitation as c7-gpu-pframes.md, unchanged)

`hevc_encoder_encode_gpu_raw()` has no GPU at all - it takes synthetic
mode/coeff/cbf arrays standing in for what a real shader dispatch would
produce, and takes the reference as a host pointer directly rather than
reading it from `ctx->recon_image`. **It does not, and cannot, exercise any
of the actual GPU-side change this task made**: `hevc_pframe_skip.comp`
itself has never executed on any Vulkan implementation, software or real,
in this pass - the shader's SAD arithmetic, its cooperative
load/reduce, the new pipeline/descriptor-set-reuse plumbing in
`gpu_compute.c`, and above all the `VkMemoryBarrier` this note spent a
whole section justifying, are all reviewed carefully and are, I believe,
correct, but **none of it has run**. Static shader validation (compiles,
SPIR-V valid) is real signal but is not execution.

### DRIVER_LOCK discipline

Confirmed intact by inspection, not by an automated check I could add real
value with: this change does not touch `va_backend.c` at all, and does not
introduce any new call site of `gpu_compute_end_picture()` or
`gpu_compute_submitted_slot()` - the one existing call site of each inside
`hevc_encoder_encode_frame()`'s GPU branch (used by the P-frame candidate
path) is unchanged in position, still inside `bc250_EndPicture()`'s
existing `DRIVER_LOCK`/`DRIVER_UNLOCK` span (verified by reading
`va_backend.c:1048-1147`, which wraps the entire function body including
the `hevc_encoder_encode_frame()` call at line 1111). All of this task's
new code (the skip-decide dispatch and its barrier) runs strictly between
the existing `gpu_compute_begin_picture()`/`gpu_compute_end_picture()`
pair, inside that same already-locked span, and does not call either
guarded function itself.

## What a board session needs to confirm next

1. **The real fps number.** This is the entire point of the change and
   cannot be produced off-board. Needs the exact same measurement the
   board review already did - `1280x720/gop=30`, 3 reps, `BC250_HEVC_GPU=1
   BC250_HEVC_GPU_PFRAME=1` - compared directly against the recorded 35.3 /
   34.2 / 34.5 fps figures. Cost-model arithmetic above (~2.7 MiB of
   readback and a real CPU loop removed from the per-P-frame critical
   path, replaced by one small compute dispatch and a 14.4 KB readback)
   supports a real recovery, but a shader dispatch's own cost (occupancy,
   the barrier's actual stall behaviour on real hardware, whether 3600
   tiny workgroups underutilize the GPU compared to the wavefront's
   larger per-step batches) is not something arithmetic can predict - only
   a timer can.
2. **Pixel-exactness at the previously-validated scale.** Same standard
   c7-gpu-pframes.md's board review already met and this task must not
   regress: 640x480 and 1280x720, 3 GOPs each, `testsrc2` (real motion),
   byte-exact encoder-recon-vs-independent-decode. This task changed real
   GPU dispatch code (a new pipeline, a new descriptor-set reuse, a new
   barrier) that the off-board harness structurally cannot exercise (see
   above), so this is not a formality - it is the first time any of this
   new code runs on real Vulkan at all.
3. **The barrier, specifically.** Section "Reusing the descriptor set..."
   above explains why the `VK_ACCESS_HOST_READ_BIT` addition is believed
   correct and why it had no existing precedent in this file to copy
   instead. If board testing ever shows the CPU CABAC stage reading a
   stale or partially-written mask (a symptom that would look like
   intermittent cu_skip_flag/merge_idx corruption, not a clean crash),
   that barrier is the first place to re-examine - though I have reasoned
   through it as far as I can without a device to run it against.
4. **The rate-control proxy's effect, if any.** `last_frame_sad`'s new
   approximation (non-skip-count * threshold) has not been compared
   against the old exact-SAD number's effect on QP choice or bitrate. Not
   a correctness question, but a real behavioural difference worth a
   `qsweep`-style bitrate/quality comparison before or alongside any future
   threshold tuning.

## Bottom line

The cost the board measured - two full-frame NV12 downloads plus a CPU SAD
loop, every P-frame - is now a single small GPU compute dispatch reading
data that was already GPU-resident, with the host round trip cut from
~2.7 MiB to 3600 `uint32_t`s (14.4 KB) at 720p. The one remaining piece of
the old download (the recon readback) was not deleted but relocated to the
one narrow, already-acknowledged fallback path that actually needs it, so
the common (dispatch succeeds) path pays nothing at all. The decision
itself - the threshold formula and the "SKIP reconstructs as an exact
reference copy, unconditionally" invariant - is untouched, and is provably
the same arithmetic reorganized, not a new heuristic. This is verified off
board to the same standard c7-gpu-pframes.md set: builds clean, shader
validates clean, the CPU path's 53-case drift oracle is unaffected, and the
project's own strongest off-board signalling check (a fully-skipped
P-frame decoding byte-identical to its reference) still passes after the
refactor. What it cannot do off-board - the real fps number, and running
the new GPU dispatch code on any actual Vulkan device at all - needs a
board, same as before.

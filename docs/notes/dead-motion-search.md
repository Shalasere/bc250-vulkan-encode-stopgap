# The HEVC motion search was dead code — verified, then removed

`docs/backlog.md` C9 left a consequence nobody had acted on:

> with the GPU vector gone, the merge list is a fixpoint at zero, so every
> P-frame MV is (0,0) and both `hevc_motion_search_diamond_8x8()` and the
> P-frame GPU ME dispatch are now pure cost with no effect on the bitstream
> (the search still feeds `last_frame_sad` for rate control). Not a
> regression — the encoder could never legally signal a non-zero vector —
> but it is dead weight, and removing it is a free CPU win.

**Verdict: the claim is correct, with one qualification it does not state
(§2), and one consequence it does state that has a real cost (§3).** The
CPU-side search is removed; the GPU-side half is *not*, and why is §5.

Everything here is off-board. There was no board available, and nothing
below needs one.

---

## 1. Verifying it — perturb the search, diff the bytes

Reading the code is how this class of claim goes wrong here, so the search
was verified by breaking it and looking at the output.

The existing oracle could not answer the question. `tools/hevc_host_drift.sh`
compares the encoder's *reconstruction* against *ffmpeg's decode* of the
bitstream it just produced, so it passes for any self-consistent bitstream
and is blind to a change that alters the bytes while staying decodable.
"The search cannot affect the bitstream" is a statement about the bytes.

So the harness now takes `BC250_DRIFT_BS_MD5=<abs path>` and records an md5
of every emitted bitstream alongside its usual check. Recording implies
`BC250_RC_NOMINAL_DRAIN=1`, because the three VBR cases otherwise drain
their leaky bucket by real elapsed time and are not byte-reproducible
against *themselves* (`docs/performance-measurement.md`, "a second, CPU-only
way to lose byte-exactness"). Without that they would have produced a
spurious diff on every run — including an A/A run.

**A/A floor first.** Two runs of the unmodified tree: 38/38 `HOST DRIFT
PASS`, and the two md5 lists identical, 38 for 38. A rig that cannot report
"nothing changed" when nothing changed cannot report anything else.

Then four perturbations, injected at the search's call site in `encode_cu()`
(temporary code, not committed), each run over the full 38-case set:

| # | perturbation | drift | bitstreams vs baseline |
|---|---|---|---|
| 0 | hook present, disabled | PASS 38/38 | identical, 38/38 |
| 1 | force the result vector to (2,0) | PASS 38/38 | **identical, 38/38** |
| 2 | force it to a per-CU pseudo-random even vector | PASS 38/38 | **identical, 38/38** |
| 3 | corrupt `best_sad` (`×2+7`), keep the vector | PASS 38/38 | differs on 7 cases |
| 4 | full stub: vector (0,0) + SAD at (0,0) | PASS 38/38 | differs on 1 case |

Rows 1 and 2 are the claim. The search's *vector* — the thing a motion
search exists to produce — can be replaced with anything at all and every
one of the 38 bitstreams comes out byte-identical, including the six inter
cases that set `BC250_HEVC_FAKE_GPU_MV` precisely because off-board every
merge candidate is otherwise (0,0). The search cannot move a bit.

## 2. The qualification: the SAD was not only a rate-control feed

Row 3 is why the perturbation was split in two, and it contradicts the
parenthesis in C9's wording. Corrupting `best_sad` while leaving the vector
alone changed **seven** bitstreams — and six of them are constant-QP cases,
where `rc_get_frame_qp()` returns `current_qp` before it ever looks at
`est_sad`. Rate control was not the only consumer.

The second consumer is a shortcut in the merge-candidate loop:

```c
if (c_dx == best_dx && c_dy == best_dy) c_sad = best_sad;   /* removed */
```

`c_sad` is compared against the skip threshold, so `best_sad` reaches
`cu_skip_flag` — i.e. the bitstream — whenever the search's best vector
equals the candidate being evaluated. That is safe *only* because
`best_sad` is the true SAD at `(best_dx, best_dy)`: when the search returns
(0,0) the shortcut hands back exactly the SAD at (0,0), which is what the
`else` branch would have computed anyway, and when it returns anything else
the shortcut does not fire. Row 3 breaks that identity by making the pair
inconsistent, and the CQP output moves immediately.

Worth recording because it is the trap in this removal: a replacement that
feeds the skip decision any number other than the honest zero-MV SAD
changes constant-QP output, and no VBR case is needed to see it.

## 3. What rate control gets now, and why

`last_frame_sad` used to accumulate the diamond search's best SAD. It now
accumulates the SAD of the co-located reference block — the zero-MV SAD —
which `encode_cu()` computes once and shares with the skip decision.

This was chosen, not defaulted to:

- **It is the residual the encoder will actually face.** Every P-frame
  vector is (0,0), so a motion-compensated SAD describes a prediction the
  bitstream has no syntax to ask for. The old number was `<=` the new one
  by construction (the search starts at (0,0) and only accepts
  improvements), so rate control was systematically told frames were easier
  than they are.
- **Rate control's only use of it is a ratio.** `RC_VBR` compares
  `est_sad / prev_frame_sad` against 1.3 / 0.7 as a temporal-complexity
  signal. Both sides move together, which is why the effect is small — but
  where it lands, it now lands on a realisable number.
- **It is free.** The skip decision already needs this exact SAD. The old
  code computed it *again* — up to five times per CU, once per (0,0) merge
  candidate — on top of the search.

The alternative, keeping the search purely to feed rate control, was
rejected: that is 100% of the cost for a number that is worse.

**Cost, stated plainly:** this is a rate-control change, so in non-CQP modes
it changes the bitstream. Of the 38 drift cases, 37 are byte-identical and
one moves: `128x128 p2 gop8 8 frames VBR 4000 kbps`, 4889 → 4786 bytes
(−2.1%), diverging at frame 3, where the higher complexity estimate buys
one QP step. It still decodes byte-exactly against ffmpeg on both planes,
as do all 38. The other two VBR cases (400 and 2000 kbps) are unchanged.
`ctest -R HevcEncodeBitstreamTest` keeps its stream md5 exactly
(`3f2d00acec79afa32cced4e53e298dae`, measured before and after), and its two
assertions on `hevc_encoder_get_last_frame_sad()` — non-zero on a static
P-frame, higher on a moving one — still hold on the new feed.

The final code was checked against row 4 rather than assumed equivalent to
it: the committed tree reproduces the stubbed-search md5 list **exactly**,
all 38 cases.

## 4. The number

`tools/hevc_host_drift.sh` builds `hevc_host_repro.c` with a hand-rolled
`gcc -O2` line, which `docs/performance-measurement.md` says is wrong in
either direction and has twice been. So `hevc_host_repro` is now a CMake
target, sharing `cavlc_bench`'s flag policy: the shipped `-O2 → -O3 -DNDEBUG`
plus `-march=znver2 -mtune=znver2` and `-falign-functions=64
-falign-loops=32` (`-DCAVLC_BENCH_MARCH=off` disables both harnesses'
march). Verified in `flags.make`: `-O2 -O3 ... -march=znver2`.
`hevc_host_repro` now also prints `encode_ms_total`, wall time inside
`hevc_encoder_encode_raw()` only — not the pattern fill, not the fwrite,
not process startup.

A/B protocol: two binaries built from the two trees, run **ABBAABBA**
(plain alternation left the first side of each pair carrying ~2% with
identical code, per the same doc), `taskset -c 2`, one warm-up per side,
12 timed runs per side, median reported.

**A/A floor, eight comparisons of the baseline binary against a copy of
itself:** −0.73%, +0.27%, +2.05%, +0.06%, −0.56%, −0.08%, +1.74%, −0.18%.
Worst case ±2.1%; treat anything under ~2.5% as no result. (This is a WSL2
dev box, not the board.)

**A/B, baseline vs search removed:**

| workload (CQP 27) | baseline | removed | delta |
|---|---|---|---|
| 1280x720 pattern 2, 30 frames, gop 10 | 441.9 ms | 350.5 ms | **−20.7%** |
| 854x480 pattern 3, 40 frames, gop 8 | 641.3 ms | 592.5 ms | **−7.6%** |
| 640x360 pattern 3, 100 frames, gop 20 | 919.4 ms | 849.5 ms | **−7.6%** |
| 640x360 pattern 3, 20 frames, **gop 1** | 157.0 ms | 156.1 ms | −0.6% (control) |

The all-intra control is the specificity check: no P-frames, no search,
no change — it lands inside the floor. The spread between the other three
is the P-frame share and how often content falls back to intra: pattern 2
(diagonal ramp) skips more and codes less residual, so the search was a
bigger fraction of it than in pattern 3 (pseudo-random).

Mechanically the CPU path went from up to ~39 SAD evaluations per 8x8 CU
(zero-MV + GPU candidate + up to 5 spatial predictors + 3 diamond steps ×
≤2 iterations × 4 points + 8-point refinement), plus up to 5 more in the
candidate loop, to exactly **one**.

**This is a dev-box number for the CPU HEVC path.** It is the same code the
board runs, but the board's frame time also contains GPU work and a
different memory system; the shipped figure needs a board run
(`lab gate` / `lab compare`, and `lab qsweep --codec=hevc` for the quality
side, which C9 already lists as outstanding).

## 5. What was deliberately NOT removed — needs a board run

C9 also calls the P-frame GPU ME dispatch dead. It is unconsumed, but it is
not removable off-board:

- `motion_estimation.comp` is dispatched from
  `gpu_compute_dispatch_encode()` **unconditionally** — Stage 2 runs before
  the `if (!is_intra)` branch, for I-frames too — and that function is
  shared with the H.264 path. There is no "skip ME" switch to flip;
  `is_intra` selects the prediction path, not whether ME runs.
- The HEVC CPU path keeps the whole `begin_picture / dispatch_encode /
  end_picture / sync` sequence deliberately, "purely to preserve the exact
  same Vulkan image layout transitions and fence/staging-buffer
  bookkeeping" the rest of the driver depends on (`encoder_h265.c`'s top
  comment). Whether that survives removing the dispatch is a hardware
  question.

So the MV readback in `hevc_encoder_encode_frame()` stays with the dispatch
it belongs to, now carrying a comment saying it is unconsumed. It is one
memcpy per P-frame against a search that was ~39 SADs per CU, so keeping it
costs approximately nothing; it and `BC250_HEVC_FAKE_GPU_MV` are also the
seam a real MVD/AMVP path would reconnect to, and deleting the only
off-board source of a non-zero vector is exactly how the injection bug in
C9 stayed invisible the first time.

**Locking discipline (per `CLAUDE.md`): untouched.** No call site of
`gpu_compute_end_picture()` or `gpu_compute_submitted_slot()` was added,
removed or moved by this change. Note for whoever does remove the readback:
`gpu_compute_get_mv_staging_data()` calls `gpu_compute_submitted_slot()`
internally, so that readback *is* one of the accessors the external-locking
rule is about.

**Board-run items left behind:**

1. Remove the GPU ME dispatch + MV readback from the HEVC path and confirm
   the layout/fence contract survives, and that H.264 is unaffected.
2. Re-measure the CPU win on the board at 1080p/1440p (`lab compare`).
3. C9's own outstanding item: re-run `lab qsweep --codec=hevc` and
   `lab gate` after the two C9 fixes.

The six `BC250_HEVC_FAKE_GPU_MV` drift cases are now provably inert
duplicates of their non-MV siblings — proven by perturbations 1 and 2, not
argued. They are kept as the regression guard for the day a consumer is
reconnected; if the GPU MV path is ever removed, they should go with it.

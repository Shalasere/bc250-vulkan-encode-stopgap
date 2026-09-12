# AGENTS.md

Machine-readable onboarding for any AI agent working in this repo — Claude,
Codex, or otherwise. If you are an agent starting cold with no prior context
on this project, read this file fully before touching anything.

This file is **protocol and mechanics**. It does not duplicate:

- `CLAUDE.md` — the project's hard-won technical rules, each with a cause and
  a DEVLOG citation. Read it before asserting anything about performance or
  correctness.
- `docs/DEVLOG.md` — the authoritative historical record (~3700 lines).
  `grep -n -i '<subsystem>' docs/DEVLOG.md` before explaining any mechanism.
  Section numbers referenced below (`§NN`) live there.

If any instruction here conflicts with `CLAUDE.md`, `CLAUDE.md` wins for
technical claims; this file wins for operational mechanics (how to reach the
board, how to avoid racing other agents, how to verify a change).

## 0. What this repo is, in one paragraph

`bc250-vcn-driver`: a from-scratch Vulkan-compute H.264 encoder exposed as a
libva VA-API driver, for the AMD BC-250 APU, whose real hardware video block
(VCN) is fused off/dead. It bypasses that dead block by doing the whole
encode — motion estimation, transform, quantize, entropy coding — as Vulkan
compute shaders plus CPU-side CAVLC/CABAC. It ships as `bc250_drv_video.so`
loaded by libva, and is exercised in practice via ffmpeg's `h264_vaapi`
encoder, driven either directly or by Sunshine (game streaming) on the board.
v0.3.0 is shipped and user-confirmed working for real 1440p Moonlight
streaming.

## 1. Where truth lives, in priority order

1. `docs/DEVLOG.md` — grep it first. It is the record of what was actually
   measured, including retracted claims (marked `🚨 CORRECTION`). A claim
   that reads as verified three sections ago is not automatically still true
   — re-run the grep against current `HEAD`, don't cite an old result.
2. `CLAUDE.md` — the compressed rule set distilled from the DEVLOG. Read it
   before making any performance or correctness claim.
3. Code comments — this codebase comments *why*, not *what*, and the why is
   frequently load-bearing (documents a prior bug, a spec citation, a
   measured tradeoff). Read the comment above a line before changing it.
4. Anything else (your own prior session's memory, a summary, a habit from
   another project) — treat as unverified until checked against 1–3.

## 2. Environment mechanics (Windows host driving a remote Linux board)

- Primary dev machine: Windows, PowerShell, with WSL available.
  **PowerShell mangles inline `$vars`/quotes/pipes before WSL ever sees
  them.** Never pass an inline interpolated command. Always write a `.sh`
  file to a scratch directory and execute that file: `wsl bash
  /path/to/script.sh`.
- Board: `user@10.0.0.104`, password in your task context (ask if not
  provided — do not guess or invent one). SSH: `sshpass -p '<password>' ssh
  -n -o StrictHostKeyChecking=accept-new user@10.0.0.104 "..."`. **`-n` is
  mandatory** (ssh in a pipeline otherwise eats stdin) — and it means
  **heredocs will not reach the remote side**. Ship remote commands as script
  files (scp them over, then `ssh ... "bash /tmp/script.sh"`), never as
  inline heredocs.
- Compiling happens inside a container: `distrobox enter driver-build --
  bash -lc "..."`. **Running ffmpeg does not** — ffmpeg lives on the board's
  HOST, not inside `driver-build`. Use a plain `ssh` command for anything
  that invokes `ffmpeg`.
- **This board runs a live Sunshine streaming service for a real user.**
  Never kill unrelated processes, never reboot, never touch system
  Mesa/kernel/anything outside your own working directory. If you must build
  something heavy, use moderate parallelism (`-j6`, not `-j12`) especially if
  other agents may be active concurrently (see §4).
- Repeated/rapid SSH logins in a tight loop have crashed this board before
  (systemd-logind exhaustion). Launch a long-running remote command once
  (backgrounded if your tool supports it), wait, read the result once. Never
  poll in a loop.

## 3. Building and verifying — do not write a fresh ad-hoc script

`tools/lab` (driven from the dev machine) and `tools/bc250_lab.sh` (the
board-side harness it ships) already encode every verification rule below.
Prefer it:

```
tools/lab setup
tools/lab build work                    # ships your uncommitted tree
tools/lab build local:<ref>             # ships a specific local commit
tools/lab units <key>                   # 4 unit test binaries
tools/lab audit <key> [--env=K=V] [--md5]   # exact nonzero-mask oracle
tools/lab quality <key>                 # PSNR/SSIM gate
tools/lab exact <keyA> <keyB>           # byte-exactness (validity rules below)
tools/lab qsweep <key> [--env=K=V] [--no-ref]  # per-frame PSNR on moving content
tools/lab gate <key> [<baseKey>]        # units + audit + quality + exact
```

**Byte-exactness oracle — read this before trusting any md5 comparison:**
- Valid ONLY on `testsrc`, and ONLY all-intra (`-g 1`). Confirmed 2026-09-11:
  the same unmodified binary produced 4 different md5s across 4 runs of
  `testsrc` with P-frames (`-g 120`/`-g 10`) — GPU motion-estimation
  tie-breaking reaches P-frame `testsrc`, not just `testsrc2`.
- `testsrc2` (moving content) is NEVER a valid byte-exactness oracle, at any
  GOP. Use `qsweep`'s per-frame PSNR instead for anything that touches moving
  content.
- If a change is expected to legitimately alter output (e.g. a real
  correctness fix, not a refactor), byte-exactness is the wrong tool
  entirely — use decode-to-YUV PSNR/SSIM comparison instead, and say so
  explicitly rather than reporting a byte diff as if it were a failure.

**The mask audit (`BC250_NZ_AUDIT` / `tools/lab audit`) is an exact oracle,
but only for what it compares.** It checks `quant_levels` against
`nz_masks` — if a bug reads BOTH from the same wrong source (e.g. the wrong
double-buffer slot), they still agree and the audit reports zero mismatches
on genuinely corrupt output. This actually happened (§26.4/§24.3). Do not
treat "audit passed" as "the data came from the right frame" — check that
independently when working on anything involving the GPU double-buffer
(`current_buf`, `gpu_compute_sync_slot`, `gpu_compute_submitted_slot`).

**Never PSNR-compare a raw `.h264` directly against `-f lavfi`.** Decode both
streams to raw YUV first with forced identical `-f rawvideo -s WxH -r N`
framing on both sides. See `CLAUDE.md` and DEVLOG §22 for why the direct
comparison produces a false catastrophic-looking quality gap.

**Ship shaders with the `.so`.** New C compiled against old SPIR-V is silent
wrong output, not a load error. A build with fewer than 9 `.comp.spv` files
present is a partial/broken build — check the count.

## 4. Multi-agent / parallel work protocol

If you are one of several agents (or sessions) working on this repo
concurrently:

- **Never edit the same working directory two agents might also be using.**
  If you were not given a dedicated `git worktree` path, ask for one before
  editing anything. `git worktree add -b agent/<short-name>
  ../bc250-wt-<short-name> <starting-commit>` from the main checkout creates
  one.
- **Do not call `tools/lab build work` if other agents may be building
  concurrently.** It ships your tree to a single shared remote path
  (`/var/home/user/bc250-lab/work-src.tar.gz`) with no per-caller namespacing
  — two concurrent callers race and can cross-contaminate each other's
  builds. Instead, build in your own uniquely-named remote directory (e.g.
  `/var/home/user/agent-<short-name>/`): tar your worktree's
  `approach1-compute-encoder/`, scp it there, and run `cmake
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON .. && make -j6` directly inside
  `distrobox enter driver-build`.
- A shared, already-built, **read-only** baseline artifact may exist at
  `/var/home/user/bc250-lab/artifacts/<key>/` — ask what key represents
  current `HEAD` before starting, and diff against it rather than rebuilding
  it yourself.
- **Commit locally on your own branch. Do not push, ever, unless explicitly
  told to and told where.** `origin` (upstream `simpmix/bc250-vcn-driver`) is
  never pushed to under any circumstance. `fork`
  (`Shalasere/bc250-vulkan-encode-stopgap`) is pushed only when a human
  explicitly asks.
- End every commit message with the attribution line you were given for this
  session (ask if unclear — do not guess or omit it).
- Report back precisely: your worktree path, branch name, commit hash (or
  explicit statement that you made no commit and why), and your actual
  verification numbers — not "tested", the real figures. If a verification
  step failed, or you had to make a judgment call trading correctness for
  something else, say so explicitly. This project's culture is measured
  rigor; a confident-sounding false green has cost real time here more than
  once (§26.4).

## 5. Do-not-touch list (see `CLAUDE.md` for the full reasoning on each)

- Don't "fix" `qp_min = 12` (§18) — measured net loss.
- Don't install `tools/bc250_sunshine_shim.c` (§17) — costs ~40% frame rate.
- Don't gate health checks on a SEGV count — Sunshine SEGVs harmlessly in its
  own teardown path on nearly every stop (§20.5); use `tools/lab health`.
- Don't enable or extend HEVC/H.265 without reading `docs/hevc_scope_note.md`
  first — it is a deliberate non-functional stub.
- Don't assume single-threaded calling. **This driver currently has zero
  thread synchronization anywhere** and is called by ffmpeg from ≥2
  concurrent OS threads; TSan has confirmed real data races in
  `va_backend.c` and `gpu_compute.c` (§26.1.2). Any change touching shared
  driver state (`bc250_driver_data`, `gpu_context_t`) should be evaluated
  with this in mind — it is currently the single highest-priority open
  defect in the project, ahead of any performance work.

## 6. Current state pointer (may drift — DEVLOG is authoritative)

As of the last update to this file: v0.3.0 shipped and working. Several
compute/CPU perf levers (async compute queues, CPU/GPU frame pipelining,
sparse coefficient transport, cross-slice entropy threading) were tried and
measured as not worth adopting — see DEVLOG §24–§26 for the numbers and why.
The open, highest-priority item is the thread-safety defect in §5 above. See
`docs/DEVLOG.md`'s most recent sections for anything newer than this file.

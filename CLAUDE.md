# Working on this repo

A Vulkan-compute H.264 encoder exposed as a VA-API driver, for a board whose
hardware video engine is dead. Correctness and performance here are both
*measured*, never argued — this file exists because the expensive mistakes on
this project have all been measurement and process mistakes, not coding ones.

## 🚨🚨 TOP PRIORITY: this driver has zero thread synchronization, anywhere

`grep -rn 'pthread_mutex\|pthread_rwlock\|atomic_' src/` returns nothing.
ffmpeg calls into this driver from ≥2 of its own concurrent OS threads
(`encoder_thread`/`enc0:0:h264_vaa` and `filter_thread`/`vf#0:0`), and
ThreadSanitizer confirms real, reproducible (2/2) data races on shared driver
state — `va_backend.c`'s `bc250_CreateSurfaces`/`bc250_CreateBuffer` vs
`bc250_DestroyBuffer` (plausibly the mechanism behind a flaky SIGSEGV, see
DEVLOG §26.1.2), and `gpu_compute.c`'s double-buffer `current_buf` index
racing between `gpu_compute_end_picture()` and `gpu_compute_submitted_slot()`.
**This is not specific to any one code path (CAVLC, pipelining, etc.) — it is
the driver's default, always-on calling contract, so the default CABAC/
production path is exposed to the same race class.** ASan/UBSan cleanly
missed it (12/12 runs) because neither instruments cross-thread ordering at
all — only TSan can see this. Implemented on branch `simpmix`:
recursive driver mutex (`PTHREAD_MUTEX_RECURSIVE`), lock-free GPU fence wait (DEVLOG §26.6), and Rate Control improvements: CQP mode support, VAConfig rate control attribute negotiation, and real GPU motion SAD feeding (DEVLOG §26.7). Unit & concurrency tests added to test_va_api.c and test_encode.c.

## Before you assert a mechanism, grep the DEVLOG

`docs/DEVLOG.md` is ~2600 lines and is the authoritative record. **Search it
for the subsystem before explaining any behaviour.**

This is rule one because breaking it was the single worst error made here: a
crash was diagnosed as "a 512 MB VRAM heap shared with the display", and that
went into the README and a release note — while §10.3 already contained the
correct Vulkan heap sizes, live instrumentation proving the driver never
allocates from the VRAM heap, and an explicit warning against that exact
claim. Its title is "a wrong claim, caught and corrected before it shipped."
It shipped the second time. See §21.

`grep -n -i '<subsystem>' docs/DEVLOG.md` costs seconds.

## Measure with tools/lab, not with a fresh script

```bash
tools/lab setup                       # once
tools/lab build work                  # or: build local:<unpushed-ref>
tools/lab noise <key> --repeat=5      # establish the floor FIRST
tools/lab compare <keyA> <keyB>       # significance-tested A/B
tools/lab scoreboard <key>            # vs libx264, per load condition
tools/lab gate <key> [<baseKey>]      # units + mask audit + PSNR + byte-exactness
tools/lab deploy <key>                # health-checked, auto-rollback
```

The harness encodes validity rules that were learned the hard way. Writing a
one-off script bypasses them, and about a dozen such scripts are what produced
the errors below.

## Hard-won rules

- **Byte-exactness is only a valid oracle on `testsrc`, and only all-intra
  (`-g 1`).** This encoder is not bit-reproducible on moving content — three
  runs of one config give three different valid bitstreams. Using md5 on
  `testsrc2` made a *correct* change look broken and nearly got it reverted.
  §19.6. ⚠️ **The GPU motion-estimation non-determinism reaches plain
  `testsrc` too once P-frames are involved** (`-g 120`/`-g 10`) — confirmed
  2026-09-11 by running the SAME unmodified baseline binary against itself
  and getting different md5s on 4 separate runs. Only all-intra `testsrc` is
  a trustworthy byte-exact oracle now; treat any `-g >1` testsrc byte diff
  with the same suspicion §19.6 reserves for `testsrc2`. §26.5
- **Never gate health on a SEGV count.** Sunshine SEGVs in its own teardown
  path (`libevdev_uinput_destroy`, `_dl_fini`) on nearly every stop on this
  box. That signal fires for healthy and broken builds alike and rolled back a
  working driver. Use `tools/lab health`, which keys on the live pid. §20.5
- **No delta under ~2.5% of wall time is a result** from a single run. Noise
  floor at 1440p: `p_wall` sd 1.2%, `cavlc` 1.6%, `shadow` 3.1%,
  `gpu_total` 0.09%.
- **Idle numbers do not transfer, and name the load generator.** Every
  published throughput figure was taken on an idle GPU. Under `--load=gpu`
  (ffmpeg `nlmeans_vulkan`) 1440p goes **66.2 → 1.48 fps**, measured
  2026-09-11 — but that generator is a pathologically heavy compute filter,
  almost certainly harsher than a game, so it is a synthetic worst case and
  not a "what a game does" number. ⚠️ **There is no trustworthy real-game
  figure.** The often-repeated "a real game took 1440p from 60 to 11 fps" is
  unsourced and collides with a number §12.4 retracted as a debug-I/O
  artifact — see §21.4's correction box and §24.6. Always state the load
  condition *and* what produced it. §24.6
- **The goal is beating libx264, not beating the previous commit.** Software
  encoding doesn't touch the GPU, so it barely notices a game while GPU
  contention costs this encoder up to ~45× (66.2 → 1.48 fps under
  `--load=gpu`/`nlmeans_vulkan` — synthetic, see the load-condition rule
  above; the older bare "46×" claim had no recorded provenance at all).
  `tools/lab scoreboard` is the real scoreboard.
- **A working fix is not confirmation of the diagnosis that produced it.** If
  part of the evidence is still unexplained, the hypothesis is unfinished —
  two failing call sites were visible and read past because the fix worked. §21.5
- **Design an audit before depending on a new GPU→CPU data path.** The
  per-block nonzero mask was silently wrong on every I-frame because
  `intra_wavefront.comp` bypasses `quantize.comp`. `BC250_NZ_AUDIT=1`
  recomputes it on the CPU and caught it before anything relied on it. §19.4
- **Never PSNR-compare a raw `.h264` against a fresh `-f lavfi` source
  directly** (`ffmpeg -i ours.h264 -i lavfi... -lavfi psnr`). A raw,
  container-less stream has no reliable timing for `-lavfi psnr`'s frame
  alignment, and the resulting drift compounds every frame while being
  totally indifferent to IDR boundaries — which produced a false
  "catastrophic 21dB quality gap" that took several more measurements to
  catch (the tell: a fresh IDR should reset a real quality problem; this one
  didn't). Decode BOTH streams to raw YUV first, then compare with identical
  forced `-f rawvideo -s WxH -r N` framing on both sides — `tools/lab
  qsweep` and `scoreboard --quality` do this correctly now. §22
- **Ship shaders with the `.so`.** New C against old SPIR-V is silent wrong
  output, not a load error. Use `make -j12` (the `all` target);
  `make bc250_drv_video` does **not** rebuild `compile_shaders`.
  `load_spirv_shader()` now actually implements "beside the `.so`" - it
  resolves its own install directory with `dladdr()` + `realpath()` and looks
  there first (after `BC250_SHADER_DIR`). The `realpath()` is required, not
  cosmetic: libva opens this driver through the
  `radeonsi_drv_video.so` symlink, and `dladdr()` reports the path it was
  opened by, so the unresolved value points at the symlink's directory where
  no shaders live. Before this, a driver installed anywhere outside the fixed
  search list loaded, advertised H.264 encode, and could not encode - seen on
  the dev board with all nine `.spv` sitting right next to the `.so`.
  **Never put a second copy of the shaders in an earlier search path**
  (`/var/lib/bc250/shaders` et al) - it will outrank the real install and go
  stale on the next driver update, which is exactly the silent-wrong-output
  case above. Symlink those paths at the install dir instead.
- **Memory: ~7.95 GiB of GART/GTT** (Vulkan heaps 2.65 + 5.30 GiB), *not* the
  512 MB `mem_info_vram_total`. Unified-memory APU, no fast-VRAM tier, and the
  carve-out is neither raisable nor worth raising. Read `vulkaninfo` heaps, not
  sysfs. Sunshine's probe creates **20 GPU contexts**, so size per-context
  allocations accordingly. §21
- **Don't "fix" `qp_min = 12`** — lowering it was measured as +14% bits for
  −22% throughput and no visible change. §18
- **Don't install `tools/bc250_sunshine_shim.c`** — kept as a documented
  `LD_PRELOAD`-into-`AT_SECURE` technique only; it costs ~40% of frame rate.
  §17

## Boot safety — this project must never be able to brick a boot

This repo installs exactly one systemd unit
(`tools/bc250-vaapi-boot-redirect.service`) and touches **no** grub config,
kernel args, dracut/initramfs, `modprobe.d`, or `ld.so.conf` — deliberately.
Everything else it writes is `/etc/environment.d/99-bc250.conf` (plain env
vars) and one symlink in `/usr/lib64/dri`, neither of which is boot-critical.
**Keep it that way.** If a change would add a second unit or touch any of the
above, that is a different class of risk and needs to be justified explicitly.

The unit follows rules learned from a real incident on the dev board — a
sibling unit from the separate VCN-hardware project (`amdgpu-vcn-early.
service`) used `DefaultDependencies=no` + `Before=<early target>`, hung
`local-fs-pre.target` for 45s every boot, and needed a live USB to recover.
Our unit had the same shape and was masked alongside it. The rules:

- **Never `DefaultDependencies=no`** — it also drops `Before=shutdown.target`,
  so the unit can hang shutdown too.
- **Never order `Before=` an early target** (`sysinit.target`,
  `basic.target`, `local-fs-pre.target`, `systemd-udevd.service`). This unit's
  work calls `rpm-ostree`, which is a D-Bus client; ordering a unit that can
  block ahead of the target that brings up the thing it blocks on is how a
  machine becomes unbootable. `WantedBy=multi-user.target`, ordered only
  `Before=display-manager.service graphical.target`.
- **Always set `TimeoutStartSec=`**, and wrap any command that talks to a
  daemon in `timeout`. Unbounded blocking at boot is the actual failure mode;
  a failed unit is harmless, a hung one is not.
- **Validate before enabling.** `install_vaapi_boot_redirect.sh` runs
  `systemd-analyze verify` and test-starts the unit before `systemctl enable`,
  so a broken unit is an error message rather than a surprise at next boot.
- **`tools/uninstall_vaapi_boot_redirect.sh` is the documented escape hatch**;
  `systemctl mask bc250-vaapi-boot-redirect.service` is the fast one from a
  rescue shell.

Worst case by construction: the display manager starts up to 45s late, once,
with SSH available throughout. The machine always reaches
`multi-user.target`.

## Board and repo operations

- Board is `user@10.0.0.104`. Builds happen in `distrobox enter driver-build`.
- **This repo is independent as of 2026-09-17.** `origin` is
  `Shalasere/bc250-vulkan-encode-stopgap` — the only place work goes.
  `upstream` (`simpmix/bc250-encoding-decoding-fix`) is **fetch-only**: its
  push URL is set to `no_push`, so a stray `git push` cannot reach it. That
  is deliberate and should stay. Do not push there, open PRs there, or
  comment on its issues without being asked each time.

  History diverges at `c5a7942`, the last commit on the shared tree.

  ⚠️ **The fork point inherits the CPU-SIMD/governor layer *with* known
  defects.** `cpu_simd_me.c` and `dynamic_governor.c` landed in `62327c3`,
  which is *before* `c5a7942` — and all six of upstream's 2026-09-17 `fix(…)`
  commits touch files present here:

  | upstream commit | fixes, in code this tree has |
  |---|---|
  | `c35dd09` | buffer boundary overrun on non-16-multiple resolutions (`encoder_h264.c`, `encoder_h265.c`) — memory safety |
  | `de31caa` | Tier 3 latch, a `vkMapMemory` leak, SIMD ME spatial predictor |
  | `636e0d1` / `80d11f5` | `cpu_simd_me.c` early-exit and `_GNU_SOURCE`/`#ifdef _OPENMP` balance |
  | `5479947` | governor downward-hysteresis transition logic |
  | `0c20d49` | OpenMP passive wait policy, slice-thread cap |

  So this code is either **removed** (which is what "driver-level" most
  plausibly means — the bugs go with it, and nothing needs cherry-picking
  from a repo we've split from) or those six fixes get applied. Do not leave
  it sitting here unfixed and assume it works; upstream found real bugs in it
  within a day of writing it.

  - `git pull --ff-only` before a session is still worth it, but this is now
    a single-author tree: no other committer, no race, and CI runs on push.
  - GPL-3.0-only. A fork is squarely within the license; the obligations are
    to keep the license files and the copyright/attribution headers intact,
    and the preserved git history is the attribution record — **do not
    squash or rewrite it away.** `src/cabac.c` is adapted from x264
    (GPL-2.0-or-later); that notice must survive. `audio-fix/` is
    GPL-2.0-only and is a separate work from the GPL-3.0 driver — keep them
    separately licensed and separately built.
  - `main` has no branch protection. Never force-push it — with the history
    now being the provenance record, a rewrite costs more than it used to.
- **Repeatedly ssh'ing into the board during a long job crashes it**
  (systemd-logind exhaustion). Launch once, wait, read once.
- `ssh -n` is mandatory (ssh in a pipeline eats stdin), and `-n` nulls stdin
  so heredocs vanish — ship remote scripts as files.
- Driving this from Windows: PowerShell mangles inline quotes, pipes and
  `$vars` before WSL sees them. Always write a `.sh` and run that.
- **Disable screen blanking on the host.** Sunshine re-inits KMS capture on
  every app launch and reads a slept output as `0x0`, returning Error 503 —
  hours after starting fine. This is the most common "it broke" report. §14.4

## Scope

H.264 is real and validated. **H.265/HEVC is no longer a stub** — it now has
a real CABAC-coded encoder with GOP/P-frame prediction and zero-motion CU
skip, wired into the VA-API backend (`VAProfileHEVCMain` advertised,
config/rate-control attributes, CI decode-oracle test in
`.github/workflows/build.yml`; DEVLOG §27–§28). It is still **not verified
correct on generic content** — the last documented correctness pass found
real, busy, multi-directional luma content still mismatches ffmpeg's decoder
in ways not yet root-caused (`docs/hevc_scope_note.md`). Do not point real
streaming clients at it, and read `docs/hevc_scope_note.md` in full before
touching it further — it has the itemized bug list and the exact open
failure mode.

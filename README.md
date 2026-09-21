# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 (Vulkan compute accelerated) and H.265/HEVC (CABAC, IDR/P-frame GOPs) video encoders and a DisplayPort/HDMI audio clock fix for the AMD BC-250 on Linux.

> [!IMPORTANT]
> **H.264**: fully hardware-accelerated via Vulkan compute shaders with asynchronous pipelining, validated for locked 1080p60/1440p real-time game streaming in Sunshine/Moonlight and Steam Link. **H.265/HEVC**: Main profile encoder with periodic/forced IDR, inter-predicted P-frames, GPU compute motion estimation across 40 CUs, and SSE2 SIMD acceleration, verified against the FFmpeg reference decoder oracle. Because HEVC's DST/DCT transform and CABAC execute on the host CPU, H.264 remains the recommended choice for high-framerate real-time game streaming on lower-end host CPUs. See [Known Limitations](#known-limitations).

---

## Background

The BC-250 is a repurposed PS5 APU (Zen 2, up to 40 unlocked RDNA 2 CUs) whose hardware VCN video engine was permanently unprovisioned/eFused off at the factory — no driver or software patch can revive permanently fused silicon. Without a working VCN block, applications requiring hardware encode (Sunshine, OBS, Steam Link) fall back to software encoding. This project solves that by running video encoding as Vulkan compute shaders directly across the GPU's RDNA 2 Compute Units, exposed as a standard VA-API driver (`bc250_drv_video.so`), alongside an unrelated audio clock fix.

> [!NOTE]
> **40 CU vs 24 CU Unlock**: The physical chip has 40 CUs (20 WGPs). Stock mining board firmware often limits the APU to 24 CUs. Unlocking all 40 CUs requires an `amdgpu` kernel patch ([duggasco/bc250-40cu-unlock](https://github.com/duggasco/bc250-40cu-unlock)) or an APU-optimized distribution like **Bazzite** or **SkillFishOS**. This project does *not* bundle a kernel unlock patch, but automatically scales its compute shaders across all 40 CUs when unlocked (taking <3–5% of GPU resources).

---

## Status

Validated on physical hardware.

| | Synthetic content | Real content |
|---|---|---|
| PSNR (avg) | 59.6 dB @ 640x480, 61.7 dB @ 2560x1440 | 52.1 dB @ 2560x1440 |
| Source | `tools/quality_test.sh` | Real Sunshine/Moonlight session — `docs/DEVLOG.md` §10.7-10.8 |

**Performance**: 267 fps @ 640x480, 179 @ 720p, 100-134 @ 1080p, 67-80 @ 1440p — real-time or above throughout. GPU shaders are <1.5% of frame time; the remaining bottlenecks are CPU/memory-side.

> [!TIP]
> **Every throughput figure here is measured with an otherwise idle GPU.** Compute shader encoding shares the same CUs a running game uses, so under real GPU contention throughput drops — a heavy compute-bound title can cost this encoder up to ~45x (measured 66.2 → 1.48 fps at 1440p under a synthetic worst-case load generator). There is no dynamic CPU/GPU load-balancing in this driver by design — see [Known Limitations](#known-limitations) for why an earlier attempt at that was removed.

On moving 1440p content the encode ceiling is **67 fps, up 46% from 46 fps** (static content: 92 fps, up 42%), from two changes to what crosses the GPU→CPU boundary. The GPU now hands the CPU a per-4x4-block nonzero bitmask, so the ~90-96% of blocks that quantize to all-zero are never read out of the 22 MB coefficient buffer; and the pre-quantization coefficient buffer is no longer staged to the host at all, since all 13 CPU reads of it wanted only each block's DC term — the GPU writes those to a compact buffer 1/16th the size. Together that cut CAVLC time ~40% and dropped host-visible staging from 44.2 MB to 2.8 MB per encoder context. `docs/DEVLOG.md` §19–§20.

- `tools/setup_bazzite.sh` — verified end-to-end on real Bazzite (installs, persists, `vainfo` sees it).
- Test suite: all 5 test suites run and pass (`BitstreamTest`, `CavlcUnitTest`, `VaApiDriverTest`, `EncodeBitstreamTest`, `HevcEncodeBitstreamTest`).
- **CABAC** (`feature/h264-cabac`, ITU-T 9.3, adapted from x264, GPL-2.0-or-later): auto-selected for Main/High profile or via `BC250_USE_CABAC=1`. 10-13% smaller output than CAVLC at matched QP, ~28% more CPU, still well above real-time. Scope: I_16x16 intra / P_L0_16x16 inter only.

## Known Limitations

- **H.265/HEVC Architecture & Streaming Advice**: HEVC Main profile encoding is fully functional via VA-API (`VAProfileHEVCMain` / `VAEntrypointEncSlice`) with periodic/forced IDR I-slices, inter-predicted P-slices, 5-candidate spatial merge skip, rate control (CQP/VBR/CBR), quality presets (1–7), max frame size limits, Vulkan compute motion estimation, and SSE2 SIMD acceleration. Because HEVC's 4x4 DST-VII/DCT-II transform and CABAC entropy coding currently execute on the host CPU rather than compute shaders, real-time 1080p60 encoding incurs higher CPU load than H.264. For low-latency real-time game streaming (Sunshine/Moonlight), H.264 remains strongly recommended. **P-frame motion is still zero-motion SKIP plus intra fallback only** — no real motion compensation or inter residual yet — and the GPU intra path is all-intra only; neither is a real alternative to H.264 for streaming today. See `docs/backlog.md` items C7/C9 for the open work.
- **Packed Headers Warning**: When encoding via FFmpeg (`h264_vaapi` or `hevc_vaapi`), FFmpeg logs `Driver does not support some wanted packed headers (wanted 0xd, found 0)`. This is a harmless informational warning: the driver directly generates and embeds its own authoritative in-band AUD, SPS, PPS, and Slice headers rather than relying on external application-provided headers. See [Troubleshooting](docs/troubleshooting.md#8-ffmpeg-warning-driver-does-not-support-some-wanted-packed-headers-wanted-0xd-found-0).
- **Sunshine specifically** needs more than `LIBVA_DRIVER_NAME=bc250` — its binary's `cap_sys_admin` capability (needed for KMS capture) puts it in the kernel's secure-exec mode, where libva's `secure_getenv()`-based driver-name lookup can't see any environment variable at all, regardless of what's set. Run `sudo ./tools/install_vaapi_boot_redirect.sh` once (redirects the system `radeonsi` VA-API driver slot to this driver, persists across reboots). `docs/DEVLOG.md` §10.5/§10.6/§12.6.
- **Display must not be asleep** when Sunshine initializes capture, or it reads the output as `0x0` and fails with *"Failed to initialize video capture/encoding"* (Moonlight Error 503) — including on a client launching an app hours after Sunshine started, since each launch re-initializes capture. Disable screen blanking on the host (on KDE: PowerDevil "Screen Energy Saving" off). This is the single most likely reason a working install appears broken. `docs/DEVLOG.md` §14.4.
- **`qp_min=12` is deliberate, and lowering it is a measured net loss** — don't "fix" it. At 1440p the encoder settles at QP 12 spending ~15-19 of 31 Mbps, which looks like wasted bandwidth; taking the floor to 8 spent 14% more bits for **−22% encode throughput and no visible quality change**. QP 12 is past the point of visible return on desktop content. `docs/DEVLOG.md` §18.
- **Do not install `tools/bc250_sunshine_shim.c`.** It is kept only as a documented technique for `LD_PRELOAD`ing into an `AT_SECURE` binary. It was written to work around what turned out to be a rate-control bug (§16), was never load-bearing, and costs roughly 40% of your frame rate by forcing Sunshine off its zero-copy capture path. `docs/DEVLOG.md` §17.
- **There is no dynamic CPU/GPU load-balancing governor, and that is deliberate.** An earlier attempt shifted motion estimation to CPU SIMD threads under detected GPU contention, but it was removed: policy decisions like CPU-offload tiering are a system-management concern, not something a VA-API driver should decide for the calling application, and the implementation carried real defects (including a buffer overrun on non-16-multiple resolutions) that removing the feature retired outright rather than patched. Motion estimation always runs on the GPU now.
- **Rate control caveat**: `rc_estimate_base_qp()` saturates at `qp_min` for any target above roughly 31 Mbps at 1440p30, so it cannot differentiate high bitrate targets from each other.
- **Releases before `v0.3.1` can crash the host app on startup.** `bc250_gpu_init()` eagerly allocated every encoding buffer for 3840x2160 (~431 MB per encoder context) even at 1080p, and Sunshine's encoder probe creates **20 contexts** — ~8.6 GB against the ~7.95 GB of memory Vulkan exposes here (a 2.65 GiB host-visible heap plus a 5.30 GiB device-local one), with the host-visible heap exhausting first. Allocations failed, the failures were unchecked, and they surfaced as a SEGV rather than a clean fallback. Fixed by allocating lazily at the real resolution and checking every allocation (`Vulkan error -2` per Sunshine start: 9 → 0). If you are on an older build and Sunshine dies at startup with `status=11/SEGV` in `bc250_gpu_init`, this is it. `docs/DEVLOG.md` §19.7, corrected in §21.
- **The `512 MB` in `mem_info_vram_total` is not a limit worth chasing**, and don't try to raise it. This is a unified-memory APU: all 16 GB is one pool of GDDR6, the GPU reaches it through GART/GTT, and Vulkan reports ~7.95 GiB across two heaps. The 512 MB is only the slice amdgpu labels "VRAM" — there is no faster tier behind it, so enlarging it buys nothing. It also isn't settable: `amdgpu.vramlimit`/`vis_vramlimit` only restrict, and the BIOS/APCB route is a known dead end on this board. The genuine ceiling is the GART aperture, `amdgpu.gttsize` (auto = half of system RAM). `docs/DEVLOG.md` §21.
- **Output is not bit-reproducible on moving content.** Three runs of an identical configuration produce three different (all valid) bitstreams, differing ~0.02% in size — most likely GPU-side tie-breaking in motion estimation. It is only bit-reproducible on content that pins at `qp_min`. This matters if you are verifying a change: byte-exactness is a valid gate only on static/`qp_min` content, and anywhere else you need the PSNR gate plus repeated runs to separate your change from the encoder's own variance. `docs/DEVLOG.md` §19.6.
- **HEVC rounds odd frame sizes up to a multiple of 8.** Requesting 854x480 through `hevc_vaapi` produces 856x480; 1918x1078 produces 1920x1080. This is not fixable in the driver as it stands: ffmpeg rounds the dimensions up *before* calling `vaCreateContext`, and `VAEncSequenceParameterBufferHEVC` — unlike the H.264 one — has no conformance-window fields, so the real size is never communicated to us at all (verified by instrumenting both the context call and the sequence parameter buffer; an 854x480 request already reads 856x480 in both). Accepting packed headers would fix it by letting ffmpeg's own SPS through. **H.264 is exact at every resolution**, including non-multiple-of-16 ones. `tools/lab dims` checks this and distinguishes the two cases.
- **Two known spec-conformance gaps** (together ~3.7 dB of per-GOP drift, not visually significant): in-loop deblocking is **luma-only** while the bitstream signals `disable_deblocking_filter_idc=0`; and I-slice intra prediction reads *source* rather than reconstructed neighbours. `docs/DEVLOG.md` §14.3.
- **`build_and_install.sh`** doesn't work on immutable distros (wrote to read-only `/usr`, reported success anyway) — use `setup_bazzite.sh`/`setup_steamos.sh`.
- **Releases before `v0.2.1`** predate real-client validation and hit 3 now-fixed defects (bad QP field, dropped chroma residual, undersized bitstream buffer). Use `v0.2.1`+.
- **CI** previously gave false confidence: `-DNDEBUG` silently disabled all `assert()`-based tests, and a CMake issue meant 3 of 4 test binaries never ran. Both fixed.

The long-standing "corruption during on-screen motion" report is **fixed** as of `v0.3.0` — it was rate control, not the capture path: CBR filler bytes were being fed back into the bitrate feedback loop, which pinned QP at its 51 maximum for entire sessions while padding every frame to look like it was using the requested bitrate. Earlier releases attributed this upstream to Sunshine's KMS capture or the compositor; that was wrong, and the reasoning that produced the wrong answer is recorded in `docs/DEVLOG.md` §14–§16 alongside the fix. Remaining work is quality *tuning* (the QP floor above), not correctness.

---

## Installation

**A — Immutable/atomic distros** (Bazzite, SteamOS, HoloISO, ChimeraOS):
```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
sudo ./tools/setup_bazzite.sh   # or setup_steamos.sh
```

**B — Traditional distros** (Fedora, Ubuntu, Arch, openSUSE):
```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
./build_and_install.sh
```

**C — Pre-built release** (fastest — no compiling or dev packages needed):
Download `bc250-driver-linux-x86_64.tar.gz` from [Releases](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/releases):
```bash
tar -xzvf bc250-driver-linux-x86_64.tar.gz
cd bc250-driver
sudo ./build_and_install.sh
```

### 32-bit driver (Steam Link)

Steam Link's runtime is 32-bit and `dlopen()`s a 32-bit VA-API driver, so a 64-bit
`bc250_drv_video.so` is invisible to it and it silently falls back to software
encoding. Build an i386 driver **alongside** the 64-bit one — same sources, same
filename, different install directory, so both can coexist:

**The 64-bit installers do not build this** — it is opt-in, so that a multilib
toolchain isn't pulled onto every install to serve one kind of client. Run:

```bash
./tools/build_32bit.sh            # installs deps, builds, verifies, installs
./tools/build_32bit.sh --help     # --skip-deps / --deps-only / --no-install / --dry-run
```

It installs the 32-bit development libraries for your distro, builds,
checks the result really is `ELF32`/i386 with no `DT_TEXTREL`, and installs it
to the 32-bit DRI directory alongside the 64-bit driver. Equivalent by hand:

```bash
cmake -B build32 -S approach1-compute-encoder -DBUILD_32BIT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build32 --parallel
sudo cmake --install build32          # -> /usr/lib32/dri (or /usr/lib/i386-linux-gnu/dri)
```

Needs 32-bit development packages, **including a shared `libgomp`**:

| Distro | Packages |
|---|---|
| Arch / CachyOS | `lib32-libva lib32-libdrm lib32-vulkan-icd-loader lib32-gcc-libs` |
| Fedora | `libva-devel.i686 libdrm-devel.i686 vulkan-loader-devel.i686 glibc-devel.i686 libgomp.i686` |
| Debian / Ubuntu | `gcc-multilib libva-dev:i386 libdrm-dev:i386 libvulkan-dev:i386 libgomp1:i386` |

The shared `libgomp` matters: without it, `-fopenmp` statically links `libgomp.a`,
whose non-PIC objects produce a library with `DT_TEXTREL` — which SELinux's
`deny_execmod` refuses to `dlopen()`, i.e. it would build and install cleanly and
then fail to load in Steam Link specifically. The build links with `-Wl,-z,text`
so that becomes a hard error instead of a silent one; if it fires, install the
shared `libgomp` rather than removing the flag.

Shaders are architecture-independent SPIR-V and are shared with the 64-bit
install — no second copy, nothing to drift out of sync.

---

## What the installers change, and how to undo it

Worth reading before you install, so nothing is a surprise afterwards.

| What | Where | Why |
|---|---|---|
| `bc250_drv_video.so` | up to 8 DRI dirs (`/usr/lib64/dri`, `/usr/local/lib*/dri`, `/var/lib/bc250/dri`, `/usr/lib32/dri` …) | libva finds a driver by filename, and which dir it searches differs per distro |
| `*.spv` shaders | `/usr/share/bc250/shaders`, `/usr/local/share/bc250/shaders`, `/var/lib/bc250/shaders` | the driver is useless without them; arch-independent, so shared by both builds |
| `LIBVA_DRIVER_NAME=bc250`, `LIBVA_DRIVERS_PATH=…` | `/etc/environment.d/99-bc250.conf`, sometimes `/etc/profile.d/bc250.sh` or `/etc/environment` | how clients are told to use this driver at all |
| `bc250-vaapi-boot-redirect.service` + `/etc/bc250-vaapi-redirect-apply.sh` | `/etc/systemd/system/` | **optional.** Re-points the system radeonsi VA-API slot at this driver each boot, for clients whose environment libva ignores |

Nothing here touches GRUB, kernel arguments, the initramfs, `modprobe.d`, or
`ld.so.conf` — **this project cannot stop a machine from booting.** The worst
case from the optional boot unit is a display manager up to 45s late, with SSH
available throughout; it is ordered late, capped by `TimeoutStartSec`, and
wanted by `multi-user.target` rather than any early target.

### The one tradeoff to know about

`LIBVA_DRIVER_NAME=bc250` is written **system-wide**, and this driver
advertises **encode only** — no decode entrypoints at all. So every VA-API
client on the box (Firefox, Chromium, mpv, VLC) stops getting hardware video
*decode* and silently falls back to software. Nothing errors; video just gets
more expensive.

If you'd rather not pay that, skip the system-wide file and set
`LIBVA_DRIVER_NAME=bc250` only in the environment of the one app you want
encoding — e.g. `systemctl --user edit <sunshine-unit>` and an
`Environment=` line. You get the encoder without taking decode away from the
whole desktop.

### Undoing all of it

```bash
sudo ./tools/bc250_uninstall.sh --dry-run   # show exactly what would go, change nothing
sudo ./tools/bc250_uninstall.sh             # remove it
```

It searches for what is actually present rather than trusting a manifest, so
it also cleans up installs made before it existed, and it explains each item
as it goes. It only removes what the installers create — a driver you placed
by hand in `/opt/bc250-driver`, the `audio-fix` DKMS module (own uninstaller:
`sudo ./audio-fix/uninstall_dkms.sh`), and anything you configured yourself
are reported but left alone.

If something goes wrong mid-install and you just want the encoder to stop
being involved, the fastest single step is removing the env file and logging
out — `sudo rm /etc/environment.d/99-bc250.conf`. Environment changes only
affect newly started processes, so a re-login or reboot is needed either way.

---

## Verifying Setup

```bash
./tools/bc250_diagnose.sh          # hardware, CU count, driver load, benchmark
LIBVA_DRIVER_NAME=bc250 vainfo     # lists H.264 & HEVC profiles + VAEntrypointEncSlice
./tools/quality_test.sh            # PSNR/SSIM vs. ground truth, not just decode-without-error
```

---

## Application Setup

**Sunshine/Moonlight**: run `sudo ./tools/install_vaapi_boot_redirect.sh` once first — Sunshine's binary needs a real capability (`cap_sys_admin`, for KMS capture) that makes plain `LIBVA_DRIVER_NAME=bc250` unable to reach it at all (see [Known Limitations](#known-limitations)); this script fixes that persistently, across reboots. Then set Video Encoder to VA-API in the web UI (`https://localhost:47990`). No desktop-session change is needed — `capture=kms` works against the board's default session (Gamescope/Big-Picture included) via direct DRM enumeration; only leave `WAYLAND_DISPLAY` unset (don't force it to a specific compositor socket) so Sunshine can fall through to that path. A tuned preset is at `tools/sunshine_preset/sunshine.conf` — `apply_sunshine_preset.sh` overwrites your existing config, so back it up first.

> [!WARNING]
> **Turn off screen blanking on the host.** Sunshine re-initializes KMS capture on every app launch, and if the display has slept it reads the output as `0x0` and returns *"Failed to initialize video capture/encoding. Is a display connected and turned on?"* (Error 503) to the client — even though Sunshine itself started fine hours earlier. On KDE: System Settings → Power Management → turn off "Screen Energy Saving". Verify with `cat /sys/class/drm/card*-DP-1/enabled` (must read `enabled`, not `disabled`); `kscreen-doctor -o` is *not* a reliable check here. `docs/DEVLOG.md` §14.4.

> [!TIP]
> **Streaming a light game? Consider forcing GPU clocks up.** This encoder only occupies the GPU for roughly 25–30% of each frame, which is not enough load for a utilization-driven governor to raise clocks — so on a game that doesn't peg the GPU itself, the encoder can end up running at minimum clocks with nothing to blame. A heavy game raises clocks as a side effect and masks this; a light one doesn't.
>
> [filippor/cyan-skillfish-governor](https://github.com/filippor/cyan-skillfish-governor) provides `cyan-skillfish-performance-mode`, which wraps a command and restores the previous governor state when it exits. Add it to a game's Steam launch options:
>
> ```
> cyan-skillfish-performance-mode %command%
> ```
>
> It also takes `--fixed-frequency 1200` or `--range 500 1500` before `%command%`, and needs that project's `cyan-skillfish-governor-smu` service running with D-Bus enabled.
>
> **Attempted here, and inconclusive — do not read either way into it.** On an idle board, `cyan-skillfish-performance-mode --on` and `--fixed-frequency 2000` both produced no throughput change (94.0 vs 94.6 fps at 1080p, inside a 3.63% noise floor). But `pp_dpm_sclk` still reported the active level at 7–100 MHz *after* the pin, i.e. the request did not visibly take — the script drives the governor over D-Bus and wants root for that, and it was run unprivileged and reported success anyway. So this tests nothing: it cannot separate "no effect" from "never engaged". It also does not reproduce the case the tip is actually about, which is a *light game* occupying the GPU, not an idle one.
>
> Also on the record: DEVLOG §203 lists forcing performance mode as *refuted*, but that test predates the fix that removed an 81 ms uncached-read stall, so it ran when GPU clocks could not have mattered. Treat the question as open.
>
> `tools/lab bench` now records the DPM level seen during each run in an `sclk_mhz` column, so anyone retrying this can check the clock actually moved before believing the result.
>
> This is deliberately a user-side launch option rather than something the driver does: clock policy is a system-management concern, the same reasoning that removed `dynamic_governor.c` (see [Known Limitations](#known-limitations)).

**OBS**: `LIBVA_DRIVER_NAME=bc250 obs` → Output → Advanced → Video Encoder: FFmpeg VAAPI, Device: `/dev/dri/renderD128`.

**ffmpeg**:
```bash
export LIBVA_DRIVER_NAME=bc250

# H.264 encode (GPU compute accelerated):
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 8M output_h264.mp4

# H.265/HEVC encode. Opt-in: HEVC is NOT advertised by default, because
# Sunshine probes it first and would otherwise silently negotiate the
# ~5 fps CPU path over the 45-77 fps H.264 one. Without BC250_ENABLE_HEVC=1
# this fails with "No usable encoding profile found".
export BC250_ENABLE_HEVC=1

# ...CPU path, correct but not real-time. Both quality and speed moved
# since v0.3.x, board-measured at 2560x1440/gop=120: a deblocking-
# signalling fix took quality from 24.93 -> 44.56 dB (it was compounding
# unfiltered-reference drift across P-frames, not a tuning gap), and three
# unrelated perf changes took throughput 13.32 -> 15.6 fps at that res:
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v hevc_vaapi -b:v 6M output_hevc.mp4

# ...GPU intra path (~92 fps at 1080p all-intra). All-intra only. Byte-exact
# against ffmpeg's decode on both luma and chroma at every QP tested - the
# chroma-QP defect docs/hevc-gpu-intra.md used to warn about here is fixed:
BC250_HEVC_GPU=1 ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v hevc_vaapi -b:v 6M output_hevc_gpu.mp4
```

> [!NOTE]
> **Packed Headers Warning**: When encoding via FFmpeg, you may see `[h264_vaapi] Driver does not support some wanted packed headers (wanted 0xd, found 0)`. This is an expected, harmless informational message: the driver authors and embeds its own conforming in-band AUD, SPS, PPS, and Slice NAL headers directly in the bitstream. See [Troubleshooting](docs/troubleshooting.md#8-ffmpeg-warning-driver-does-not-support-some-wanted-packed-headers-wanted-0xd-found-0).

---

## Audio Fix (DisplayPort/HDMI)

Fixes stuttering audio via a DKMS module that survives kernel updates:
```bash
cd audio-fix && sudo ./install_dkms.sh
```
Carried over as-is; out of scope for this project's correctness work.

---

## Contributing / CI

`.github/workflows/build.yml` builds 64-bit and 32-bit drivers, runs all 5 automated test suites (`ctest`), and strictly validates both generated H.264 and H.265/HEVC bitstreams against the external FFmpeg reference decoder oracle.

**Measuring performance: use `tools/lab`** — it is the canonical tool, and
[docs/performance-measurement.md](docs/performance-measurement.md) explains why hand-rolling an `ffmpeg` command instead reliably produces numbers that are confidently wrong. Every published figure in this README came from that harness.

Troubleshooting: [docs/troubleshooting.md](docs/troubleshooting.md)

---

## License
- Driver, shaders, tools: **GPL-3.0-only**
- Audio module: **GPL-2.0-only** (inherited as-is; see `audio-fix/LICENSE`)

Copyleft: derivatives must ship source under the same terms; GPL-3.0's anti-tivoization clauses block shipping this inside a locked-down device that prevents installing a modified build.

<!-- bc250-vulkan-encode-stopgap -->

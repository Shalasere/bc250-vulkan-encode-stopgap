# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 encoder (Vulkan compute, not the VCN block) and a DisplayPort/HDMI audio clock fix for the AMD BC-250 on Linux.

> [!IMPORTANT]
> **H.264**: correct, real-time, validated in real Sunshine/Moonlight use at 1440p. **H.265/HEVC**: non-functional stub — do not enable it in any app pointed at this driver. See [Known Limitations](#known-limitations).

---

## Background

The BC-250 is a repurposed PS5 APU (Zen 2, up to 40 unlocked RDNA 2 CUs) whose VCN hardware video engine is not currently usable (likely a firmware/power-management block, not a fuse — a separate community effort targets this). Without it, apps needing hardware encode (Sunshine, OBS, Steam Link) have no good fallback. This project runs H.264 encoding as Vulkan compute shaders on the APU's CUs, exposed as a standard VA-API driver (`bc250_drv_video.so`).

Stopgap pending a working VCN unlock, not a replacement for it.

---

## Status

Validated on physical hardware.

| | Synthetic content | Real content |
|---|---|---|
| PSNR (avg) | 59.6 dB @ 640x480, 61.7 dB @ 2560x1440 | 52.1 dB @ 2560x1440 |
| Source | `tools/quality_test.sh` | Real Sunshine/Moonlight session — `docs/DEVLOG.md` §10.7-10.8 |

**Performance**: 267 fps @ 640x480, 179 @ 720p, 100-134 @ 1080p, 67-80 @ 1440p — real-time or above throughout. GPU shaders are <1.5% of frame time; the remaining bottlenecks are CPU/memory-side. GPU contention against a concurrent game: ~8.4%.

On moving 1440p content the encode ceiling is **67 fps, up 46% from 46 fps** (static content: 92 fps, up 42%), from two changes to what crosses the GPU→CPU boundary. The GPU now hands the CPU a per-4x4-block nonzero bitmask, so the ~90-96% of blocks that quantize to all-zero are never read out of the 22 MB coefficient buffer; and the pre-quantization coefficient buffer is no longer staged to the host at all, since all 13 CPU reads of it wanted only each block's DC term — the GPU writes those to a compact buffer 1/16th the size. Together that cut CAVLC time ~40% and dropped host-visible staging from 44.2 MB to 2.8 MB per encoder context. `docs/DEVLOG.md` §19–§20.

- `tools/setup_bazzite.sh` — verified end-to-end on real Bazzite (installs, persists, `vainfo` sees it).
- Test suite: all 4 binaries run and assert (not always true historically — see [Known Limitations](#known-limitations)).
- **CABAC** (`feature/h264-cabac`, ITU-T 9.3, adapted from x264, GPL-2.0-or-later): auto-selected for Main/High profile or via `BC250_USE_CABAC=1`. 10-13% smaller output than CAVLC at matched QP, ~28% more CPU, still well above real-time. Scope: I_16x16 intra / P_L0_16x16 inter only.

## Known Limitations

- **H.265/HEVC**: intra-only; correct on flat content (~56 dB PSNR), not on real high-frequency content; no inter-prediction/SAO/WPP. Leave `hevc_mode` off. See `docs/hevc_scope_note.md`.
- **Sunshine specifically** needs more than `LIBVA_DRIVER_NAME=bc250` — its binary's `cap_sys_admin` capability (needed for KMS capture) puts it in the kernel's secure-exec mode, where libva's `secure_getenv()`-based driver-name lookup can't see any environment variable at all, regardless of what's set. Run `sudo ./tools/install_vaapi_boot_redirect.sh` once (redirects the system `radeonsi` VA-API driver slot to this driver, persists across reboots). `docs/DEVLOG.md` §10.5/§10.6/§12.6.
- **Display must not be asleep** when Sunshine initializes capture, or it reads the output as `0x0` and fails with *"Failed to initialize video capture/encoding"* (Moonlight Error 503) — including on a client launching an app hours after Sunshine started, since each launch re-initializes capture. Disable screen blanking on the host (on KDE: PowerDevil "Screen Energy Saving" off). This is the single most likely reason a working install appears broken. `docs/DEVLOG.md` §14.4.
- **`qp_min=12` is deliberate, and lowering it is a measured net loss** — don't "fix" it. At 1440p the encoder settles at QP 12 spending ~15-19 of 31 Mbps, which looks like wasted bandwidth; taking the floor to 8 spent 14% more bits for **−22% encode throughput and no visible quality change**. QP 12 is past the point of visible return on desktop content. `docs/DEVLOG.md` §18.
- **Do not install `tools/bc250_sunshine_shim.c`.** It is kept only as a documented technique for `LD_PRELOAD`ing into an `AT_SECURE` binary. It was written to work around what turned out to be a rate-control bug (§16), was never load-bearing, and costs roughly 40% of your frame rate by forcing Sunshine off its zero-copy capture path. `docs/DEVLOG.md` §17.
- **Rate control caveat**: `rc_estimate_base_qp()` saturates at `qp_min` for any target above roughly 31 Mbps at 1440p30, so it cannot differentiate high bitrate targets from each other.
- **Releases before `v0.3.1` can crash the host app on startup.** `bc250_gpu_init()` eagerly allocated every encoding buffer for 3840x2160 (~440 MB per encoder context) even at 1080p, and this board exposes only a **512 MB VRAM heap** — which RADV also reports for the `HOST_VISIBLE` types the readback staging buffers use, so it is shared with the display. Sunshine's encoder probe creates 20 contexts, so allocations failed; the failures were unchecked and surfaced as a SEGV rather than a clean fallback. Fixed by allocating lazily at the real resolution and checking every allocation (`Vulkan error -2` per Sunshine start: 9 → 0). If you are on an older build and Sunshine dies at startup with `status=11/SEGV` in `bc250_gpu_init`, this is it. `docs/DEVLOG.md` §19.7.
- **Output is not bit-reproducible on moving content.** Three runs of an identical configuration produce three different (all valid) bitstreams, differing ~0.02% in size — most likely GPU-side tie-breaking in motion estimation. It is only bit-reproducible on content that pins at `qp_min`. This matters if you are verifying a change: byte-exactness is a valid gate only on static/`qp_min` content, and anywhere else you need the PSNR gate plus repeated runs to separate your change from the encoder's own variance. `docs/DEVLOG.md` §19.6.
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

**C — Pre-built release**: download `bc250_drv_video.so` + `shaders/*.spv` from [Releases](../../releases) (`v0.2.1`+), place both at the repo root, then run the A or B installer. The installers verify shaders actually landed and fail rather than silently reporting success.

---

## Verifying Setup

```bash
./tools/bc250_diagnose.sh          # hardware, CU count, driver load, benchmark
LIBVA_DRIVER_NAME=bc250 vainfo     # lists H.264 profiles + VAEntrypointEncSlice
./tools/quality_test.sh            # PSNR/SSIM vs. ground truth, not just decode-without-error
```

---

## Application Setup

**Sunshine/Moonlight**: run `sudo ./tools/install_vaapi_boot_redirect.sh` once first — Sunshine's binary needs a real capability (`cap_sys_admin`, for KMS capture) that makes plain `LIBVA_DRIVER_NAME=bc250` unable to reach it at all (see [Known Limitations](#known-limitations)); this script fixes that persistently, across reboots. Then set Video Encoder to VA-API in the web UI (`https://localhost:47990`). No desktop-session change is needed — `capture=kms` works against the board's default session (Gamescope/Big-Picture included) via direct DRM enumeration; only leave `WAYLAND_DISPLAY` unset (don't force it to a specific compositor socket) so Sunshine can fall through to that path. A tuned preset is at `tools/sunshine_preset/sunshine.conf` — `apply_sunshine_preset.sh` overwrites your existing config, so back it up first.

> [!WARNING]
> **Turn off screen blanking on the host.** Sunshine re-initializes KMS capture on every app launch, and if the display has slept it reads the output as `0x0` and returns *"Failed to initialize video capture/encoding. Is a display connected and turned on?"* (Error 503) to the client — even though Sunshine itself started fine hours earlier. On KDE: System Settings → Power Management → turn off "Screen Energy Saving". Verify with `cat /sys/class/drm/card*-DP-1/enabled` (must read `enabled`, not `disabled`); `kscreen-doctor -o` is *not* a reliable check here. `docs/DEVLOG.md` §14.4.

**OBS**: `LIBVA_DRIVER_NAME=bc250 obs` → Output → Advanced → Video Encoder: FFmpeg VAAPI, Device: `/dev/dri/renderD128`.

**ffmpeg**:
```bash
export LIBVA_DRIVER_NAME=bc250
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 8M output.mp4
```

---

## Audio Fix (DisplayPort/HDMI)

Fixes stuttering audio via a DKMS module that survives kernel updates:
```bash
cd audio-fix && sudo ./install_dkms.sh
```
Carried over as-is; out of scope for this project's correctness work.

---

## Contributing / CI

`.github/workflows/build.yml` builds, runs the full test suite, and runs quality verification. Test failures currently only warn (`continue-on-error`); the encode-verification step checks decode-without-error, not pixel correctness — `tools/quality_test.sh` covers that and isn't wired into CI yet.

Troubleshooting: [docs/troubleshooting.md](docs/troubleshooting.md)

---

## License
- Driver, shaders, tools: **GPL-3.0-only**
- Audio module: **GPL-2.0-only** (inherited as-is; see `audio-fix/LICENSE`)

Copyleft: derivatives must ship source under the same terms; GPL-3.0's anti-tivoization clauses block shipping this inside a locked-down device that prevents installing a modified build.

<!-- bc250-vcn-driver -->

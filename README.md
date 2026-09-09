# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 encoder (Vulkan compute, not the VCN block) and a DisplayPort/HDMI audio clock fix for the AMD BC-250 on Linux.

> [!IMPORTANT]
> **H.264**: correct, real-time, one open rate-control issue. **H.265/HEVC**: non-functional stub — do not enable it in any app pointed at this driver. See [Known Limitations](#known-limitations).

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

- `tools/setup_bazzite.sh` — verified end-to-end on real Bazzite (installs, persists, `vainfo` sees it).
- Test suite: all 4 binaries run and assert (not always true historically — see [Known Limitations](#known-limitations)).
- **CABAC** (`feature/h264-cabac`, ITU-T 9.3, adapted from x264, GPL-2.0-or-later): auto-selected for Main/High profile or via `BC250_USE_CABAC=1`. 10-13% smaller output than CAVLC at matched QP, ~28% more CPU, still well above real-time. Scope: I_16x16 intra / P_L0_16x16 inter only.

## Known Limitations

- **H.265/HEVC**: intra-only; correct on flat content (~56 dB PSNR), not on real high-frequency content; no inter-prediction/SAO/WPP. Leave `hevc_mode` off. See `docs/hevc_scope_note.md`.
- **Rate control** doesn't recover quickly from a bitrate spike — `RC_CBR`'s 1-second buffer, not the already-implemented `RC_LOW_LATENCY`, is what's actually selected (`rc_init()` hardcodes `RC_CBR`). Root-caused, not fixed. `docs/DEVLOG.md` §10.8.
- **`build_and_install.sh`** doesn't work on immutable distros (wrote to read-only `/usr`, reported success anyway) — use `setup_bazzite.sh`/`setup_steamos.sh`.
- **Releases before `v0.2.1`** predate real-client validation and hit 3 now-fixed defects (bad QP field, dropped chroma residual, undersized bitstream buffer). Use `v0.2.1`+.
- **CI** previously gave false confidence: `-DNDEBUG` silently disabled all `assert()`-based tests, and a CMake issue meant 3 of 4 test binaries never ran. Both fixed.

Remaining work is concentrated in the CPU-side bitstream writer and rate control; the GPU compute path itself is correctness-verified.

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

**Sunshine/Moonlight**: `export LIBVA_DRIVER_NAME=bc250`, set Video Encoder to VA-API in the web UI (`https://localhost:47990`). A tuned preset is at `tools/sunshine_preset/sunshine.conf` — `apply_sunshine_preset.sh` overwrites your existing config, so back it up first.

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

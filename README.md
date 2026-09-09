# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 video encoding (Vulkan compute, not the hardware VCN block) and a DisplayPort/HDMI audio clock fix for the AMD BC-250 ("Cyan Skillfish" / PS5 "Oberon" APU) on Linux (Bazzite, SteamOS, Fedora, Ubuntu, Arch).

> [!IMPORTANT]
> ### Status: H.264 correct and real-time; one open rate-control issue. H.265/HEVC non-functional.
> H.264 output is correct against synthetic test content (PSNR/SSIM, all resolutions tested) and runs above real-time up to 1440p60. As of `v0.2.1`, validated against real live Sunshine/Moonlight sessions on real captured desktop content, which surfaced three defects synthetic content did not exercise (fixed; see [Known Limitations](#known-limitations) and `docs/DEVLOG.md`). One issue remains open: a rate-control gap producing a sustained quality drop after a large content-complexity spike. H.265/HEVC is a non-functional stub — do not select it in any application pointed at this driver.

---

## Background

The BC-250 is a repurposed PlayStation 5 APU: Zen 2 CPU, up to 40 RDNA 2 Compute Units with the community CU unlock applied. Its VCN hardware video engine is not currently usable (reverse-engineering indicates the block is physically present but blocked by an unresolved power-management/firmware initialization issue, not permanently fused off; the hardware-unlock effort is a separate project).

Without a working VCN, applications that need hardware video encode (Sunshine, OBS, Steam Link) fall back to CPU-only software encoding or have no path at all. This project implements a software H.264 encoder as Vulkan compute shaders running on the APU's RDNA 2 Compute Units, exposed as a standard VA-API driver (`bc250_drv_video.so`), so any VA-API-aware application can use it as if it were a hardware encoder.

This is a stopgap pending a working VCN unlock, not a replacement for it.

---

## Status

Validated on physical BC-250 hardware.

- **Correctness (H.264, synthetic content)**: `tools/quality_test.sh` (independent PSNR/SSIM harness — captures ground-truth input, encodes through the actual VA-API pipeline, decodes with a separate software decoder, scores the result) passes at 61.65 dB average (2560x1440) and 59.60 dB average (640x480, default resolution) as of `v0.2.1`. Per-resolution numbers change as fixes land; re-run the script for a current number against a given build.
- **Correctness (H.264, real content)**: as of `v0.2.1`, also validated against real captured frames from a live Sunshine/Moonlight session (2560x1440, real desktop/UI content) — 52.1 dB average against real ground truth, zero decode errors. Three of the four `v0.2.1` fixes were found only by this class of test, not the synthetic gate above; see `docs/DEVLOG.md` §10.7-10.8.
- **Performance (H.264)**: board-measured throughput, real-time or above at every tested resolution — 267 fps at 640x480, 179 fps at 1280x720, 100-134 fps at 1920x1080, 67-80 fps at 2560x1440. GPU compute shaders account for under 1.5% of frame time; the binding constraints are CPU/memory-side (see `docs/DEVLOG.md`). GPU contention against a concurrently running game dropped from ~29% to ~8.4% as a result.
- **Installation on immutable distros**: `tools/setup_bazzite.sh` installs, persists across reboots, and is found by `vainfo` with correct H.264 profiles listed, on real Bazzite (ostree/Kinoite).
- **Test suite**: all four unit test binaries run and assert (see [Known Limitations](#known-limitations) — this was not always true).
- **CABAC entropy coding (H.264, `feature/h264-cabac`)**: ITU-T 9.3 CABAC alongside the existing CAVLC (9.2) path, adapted from x264 (GPL-2.0-or-later, license-compatible with this project's GPL-3.0-only). Selected via `BC250_USE_CABAC=1`/`0`, or automatically for a negotiated Main/High-profile VA-API config (Baseline stays CAVLC-only per the H.264 spec's own profile restriction). Zero ffmpeg decode errors/warnings across 640x480 and 1280x720, QP 20/26/32/40, single- and multi-slice, I- and P-frames; decoded content verified numerically and visually. File-size reduction vs. CAVLC at matched QP: 10.3-12.7%, consistent across tested resolutions and QP range. CPU cost: ~28% more wall-clock than CAVLC at 1280x720 (5.75ms vs 4.50ms/frame), still well above real-time (174 fps implied vs. 222 fps for CAVLC). Scope: I_16x16 intra and P_L0_16x16 inter only, matching this encoder's existing macroblock coverage — no B-slices, 8x8 transform, or multi-reference contexts.

## Known Limitations

- **H.265/HEVC is incomplete.** `encoder_h265.c` is a genuine intra-only HEVC Main-profile encoder with a real CABAC entropy coder (adapted from x265, GPL-2.0-or-later) and real prediction/transform/quantization, validated against an independent CABAC decoder cross-check and ffmpeg's header parser. It encodes flat/low-detail content correctly (~56 dB PSNR on near-flat test frames) but does not yet correctly encode real, high-frequency, directional-mode-heavy content. No inter-prediction, SAO, or WPP; every frame is an independent IDR. Do not enable HEVC in any application pointed at this driver (e.g. Sunshine's `hevc_mode`). See `docs/hevc_scope_note.md`. Use H.264 (`encoder=vaapi`, not `hevc_mode=1`).
- **GPU contention while a game runs concurrently is nonzero.** ~8.4%, down from ~29% in earlier measurements; both figures supersede an earlier "under 3-5%" estimate that was not board-measured.
- **Rate control does not recover quickly from a large content-complexity spike.** A single frame requiring far more bits than the target (e.g. a large real screen change) saturates the current `RC_CBR` mode's 1-second leaky-bucket buffer, holding QP elevated for up to a full GOP interval. `RC_LOW_LATENCY` (a 2-frame buffer, documented in `rate_control.h` as intended for this use case) exists in the code but is never selected — `rc_init()` hardcodes `RC_CBR`. Root-caused and reproduced offline; not fixed as of `v0.2.1`. See `docs/DEVLOG.md` §10.8 and the `v0.2.1` release notes.
- **`build_and_install.sh` does not work on immutable/atomic distros** (Bazzite, SteamOS/HoloISO, ChimeraOS) without this branch's fix — it wrote to read-only `/usr` paths and reported success regardless. Use `tools/setup_bazzite.sh` or `tools/setup_steamos.sh` on those distros.
- **Releases before `v0.2.1` predate real-client validation.** `v0.2.0` and earlier were tested only against synthetic content; real desktop/UI content hits at least three now-fixed defects (an out-of-range QP field that broke the first client connection, a P-slice skip-decision bug that dropped chroma corrections, an undersized bitstream buffer that silently truncated frames). Use `v0.2.1` or later, or current `main`.
- **CI's pass/fail signal was previously unreliable.** `assert()`-based tests were compiled with `-DNDEBUG` (making every `assert()` a no-op), and a separate CMake configuration issue meant `ctest` never discovered 3 of the project's 4 test binaries. Both fixed and verified (a reintroduced known bug now fails the relevant test). CI history predating this fix does not reflect actual test coverage.

The remaining work is concentrated in the CPU-side bitstream writer and rate control; the GPU compute path itself is correctness-verified.

---

## Installation

### Option A: Immutable / Atomic Distros (Bazzite, SteamOS, HoloISO, ChimeraOS)

```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
sudo ./tools/setup_bazzite.sh   # or ./tools/setup_steamos.sh on SteamOS/HoloISO
```

Installs to a path that persists across an ostree/atomic deployment update, and configures SELinux contexts where relevant.

### Option B: Traditional Distros (Fedora, Ubuntu, Arch, openSUSE)

```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
chmod +x build_and_install.sh tools/*.sh
./build_and_install.sh
```

### Option C: Pre-Built Release

Download `bc250_drv_video.so` and its matching `shaders/*.spv` from the [Releases page](../../releases) (`v0.2.1` or later — see [Known Limitations](#known-limitations)), place both at the repository root, then run the Option A or B installer for your distro. Both installers verify that compiled shaders were actually installed and exit with an error rather than reporting success if they were not.

---

## Verifying Your Setup

```bash
./tools/bc250_diagnose.sh
```

Checks hardware identification, active Compute Unit count, the audio fix module, and whether the VA-API driver loads. Reports a board-specific fps figure via its built-in benchmark; expect it in the ranges given under [Status](#status).

```bash
export LIBVA_DRIVER_NAME=bc250
vainfo
```

Should list `VAProfileH264ConstrainedBaseline`, `VAProfileH264Baseline`, `VAProfileH264Main`, and `VAProfileH264High` with `VAEntrypointEncSlice` support.

```bash
./tools/quality_test.sh
```

Verifies decoded pixel content against ground truth (PSNR/SSIM), not just that a bitstream decodes without error.

---

## Application Setup

### Sunshine / Moonlight

```bash
export LIBVA_DRIVER_NAME=bc250
```

Set **Video Encoder** to **VA-API** in Sunshine's web configuration (`https://localhost:47990` → Configuration → Audio/Video). A tuned config template is at `tools/sunshine_preset/sunshine.conf`. `apply_sunshine_preset.sh` overwrites the existing `sunshine.conf` in full; back up or manually merge any existing hand-tuned settings (e.g. `capture`, CSRF origin allowlist) before running it.

### OBS Studio

```bash
LIBVA_DRIVER_NAME=bc250 obs
```

Settings → Output → Output Mode: Advanced → **Video Encoder**: FFmpeg VAAPI, **VAAPI Device**: `/dev/dri/renderD128`.

### FFmpeg Command Line

```bash
export LIBVA_DRIVER_NAME=bc250
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 8M output.mp4
```

---

## Audio Fix (DisplayPort & HDMI)

The BC-250 has a known audio clock issue causing stuttering audio over DisplayPort or HDMI. A DKMS kernel module fixes it and persists across kernel updates:

```bash
cd audio-fix
sudo ./install_dkms.sh
```

Out of scope for the correctness/performance work described above; carried over as-is.

---

## Contributing / CI

`.github/workflows/build.yml` builds the driver, runs the test suite (all four binaries; see [Known Limitations](#known-limitations)), and runs `tools/quality_test.sh`-style content verification. The test-suite step is configured with `continue-on-error`, so a test failure currently shows as a warning rather than blocking the job. The encode-verification step checks that ffmpeg decodes the output without a hard error, not pixel-level correctness — that is `tools/quality_test.sh`, which is not yet part of the automated CI step.

Troubleshooting: [docs/troubleshooting.md](docs/troubleshooting.md)

---

## License
* Userspace compute driver, shaders, and tools: **GPL-3.0-only**
* Audio fix kernel module: **GPL-2.0-only** (inherited from its original source; see `audio-fix/LICENSE`)

Copyleft, not permissive: distributing a modified or combined version requires releasing source under the same terms; GPL-3.0's anti-tivoization provisions preclude shipping this code inside a locked-down device that blocks the end user from installing their own modified build.

<!-- bc250-vcn-driver -->

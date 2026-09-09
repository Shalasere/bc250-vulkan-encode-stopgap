# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 video encoding (via Vulkan compute, not the hardware VCN block) and a DisplayPort/HDMI audio clock fix for the **AMD BC-250 ("Cyan Skillfish" / PS5 "Oberon" APU)** on Linux (Bazzite, SteamOS, Fedora, Ubuntu, Arch).

> [!IMPORTANT]
> ### Current status: correct and real-time on synthetic content; real-client-validated with one open issue (H.264 only — H.265/HEVC is not yet real)
> The H.264 encoder produces correct output against synthetic test content (PSNR/SSIM against ground truth, all resolutions tested) and runs well above real-time up to and including 1440p60. As of `v0.2.1`, it has also been validated against real live Sunshine/Moonlight sessions on real captured desktop content, which surfaced and fixed three real defects synthetic content never exercised (see [Known Limitations](#known-limitations) and `docs/DEVLOG.md`). One defect remains open: a rate-control gap that produces a visible, sustained quality drop after a large content-complexity spike. **H.265/HEVC support is currently a non-functional stub** — do not select HEVC in any app pointed at this driver yet. See [Known Limitations](#known-limitations) for the full, current picture before you plan around this project.

---

## Why Does This Project Exist?

The AMD BC-250 is a repurposed PlayStation 5 APU with a Zen 2 CPU and up to 40 RDNA 2-class Compute Units (with the community-discovered CU unlock applied). That makes it an unusually cheap, powerful platform for a living-room gaming PC — except its VCN hardware video engine, needed for hardware-accelerated encode/decode, cannot currently be used. (Recent reverse-engineering suggests the block is physically present but stuck behind an unresolved power-management/firmware initialization problem, not permanently fused off — see the community's ongoing hardware-unlock effort, which is a separate project from this one.)

Without a working VCN, applications like Sunshine, OBS, and Steam Link fall back to CPU-only software encoding, which is either slow or absent depending on the app. This project's approach: emulate a hardware video encoder in software, using Vulkan compute shaders that run on the APU's own unlocked RDNA 2 Compute Units, and expose it to the rest of the system as a standard VA-API driver (`bc250_drv_video.so`) — so, in principle, any VA-API-aware application can use it exactly like it would a real hardware encoder.

This is a stopgap, not a replacement for real hardware acceleration. If and when the community's VCN-unlock effort succeeds, that will be the better answer for this chip. Until then, this project's goal is to make the software path as close to genuinely usable as it can get.

---

## What Actually Works Today

Validated on physical BC-250 hardware, not just in CI or in theory:

- **Correctness (H.264, synthetic content)**: `tools/quality_test.sh` (an independent PSNR/SSIM harness — captures real ground-truth input, encodes through the actual VA-API pipeline, decodes with a separate software decoder, and scores the result) currently passes at 61.65 dB average (2560x1440) and 59.60 dB average (640x480, default resolution) as of `v0.2.1`; per-resolution numbers move as fixes land, so treat any single figure here as a snapshot, not a guarantee — re-run the script for a current number on your own build.
- **Correctness (H.264, real content)**: as of `v0.2.1`, also validated against real captured frames from a live Sunshine/Moonlight session (2560x1440, real desktop/UI content, not synthetic test patterns) — 52.1 dB average against real ground truth, zero decode errors. This is a materially different and harder test than the synthetic gate above; three of the four `v0.2.1` fixes were found only by this kind of test (see `docs/DEVLOG.md` §10.7-10.8).
- **Performance (H.264)**: real, board-measured throughput now well exceeds real-time at every tested resolution — roughly **267 fps at 640x480, 179 fps at 1280x720, 100-134 fps at 1920x1080, and 67-80 fps at 2560x1440** (the last comfortably clears even a 60fps target, not just 30fps). The GPU compute shaders were never the bottleneck (under 1.5% of frame time); the real fixes were all CPU/memory-side — see `docs/DEVLOG.md` for the full investigation. A byproduct: the earlier-measured GPU-contention-while-gaming hit dropped from ~29% to **~8.4%** once the encoder stopped occupying the GPU for as much wall-clock time per frame.
- **Installation on immutable distros**: `tools/setup_bazzite.sh` is verified working end-to-end on a real Bazzite (ostree/Kinoite) console — driver installs, persists, and is found by `vainfo` with the right H.264 profiles listed.
- **Test suite**: all four unit test binaries actually run and actually assert something (see [Known Limitations](#known-limitations) for why that's worth calling out explicitly).
- **CABAC entropy coding (H.264, `feature/h264-cabac`)**: a real CABAC (ITU-T 9.3) path now runs alongside the original CAVLC (9.2) path, adapted from x264 (GPL-2.0-or-later, verified compatible with this project's GPL-3.0-only license). Selectable via `BC250_USE_CABAC=1`/`0`, or automatically for a negotiated Main/High-profile VA-API config (Baseline stays CAVLC-only, matching the H.264 spec's own profile restriction). Board-validated: zero ffmpeg decode errors/warnings across 640x480 and 1280x720, QP 20/26/32/40, single- and multi-slice, I- and P-frames; decoded pixel content confirmed correct both numerically (PSNR/SSIM) and visually (extracted frames). Real measured file-size reduction vs CAVLC **at matched QP** (the correct apples-to-apples comparison — a CBR-target comparison undersells this, since rate control converges both coders toward the same target size): **10.3-12.7% smaller**, consistent across both tested resolutions and the full QP range — in line with the textbook 10-15% figure. CPU cost is higher than CAVLC (~28% more wall-clock at 1280x720, 5.75ms vs 4.50ms/frame) but stays far above real-time (174 fps implied vs CAVLC's 222 fps, both well over any 30-60fps target). Known scope limits: I_16x16 intra and P_L0_16x16 inter only (matches this project's existing macroblock coverage) — no B-slices, 8x8 transform, or multi-reference CABAC contexts exist yet, since this project's encoder doesn't emit those macroblock types either way.

## Known Limitations

Being direct about this rather than burying it, since it materially affects whether this project is useful for you yet:

- **H.265/HEVC is real but incomplete — do not enable it for real use yet.** `encoder_h265.c` was a total non-functional stub earlier in this project's history; it's now a genuine intra-only HEVC Main-profile encoder with a real CABAC entropy coder (adapted from x265, GPL-2.0-or-later, verified license-compatible with this project's GPL-3.0-only) and real prediction/transform/quantization, validated with an independent from-scratch CABAC decoder cross-check and ffmpeg's own header parser. It correctly encodes and decodes **flat/low-detail content** (~56 dB PSNR on near-flat test frames). **It does not yet correctly encode real, high-frequency, directional-mode-heavy content** — the failure is far smaller than the old stub's "zero picture content" and is not a full bitstream desync, but it is real and unresolved. No inter-prediction (P-frames), SAO, or WPP yet either — every frame is coded as an independent IDR. **Do not enable HEVC anywhere pointed at this driver for real video yet** (Sunshine's config has an `hevc_mode` setting, and many streaming clients prefer HEVC automatically for bandwidth — leave it off). See `docs/hevc_scope_note.md` for the detailed status and what's left. Stick to H.264 (`encoder=vaapi`, not `hevc_mode=1`) for real use.
- **GPU contention while gaming, while much improved, is not zero.** See the ~8.4% figure above — real, if far smaller than the ~29% earlier documentation implied was already the honest number, and much smaller than that same documentation's own "under 3-5%" original claim.
- **Rate control does not recover quickly from a large content-complexity spike.** A single frame that legitimately requires far more bits than the target (e.g. a large real screen change) saturates the current `RC_CBR` mode's 1-second leaky-bucket buffer, holding QP elevated for up to a full GOP interval rather than the couple of frames a tighter buffer would need. `RC_LOW_LATENCY` (a 2-frame buffer, documented in `rate_control.h` as intended for exactly this streaming use case) already exists in the code but is never selected — `rc_init()` hardcodes `RC_CBR` unconditionally. Root-caused and reproduced offline; not fixed as of `v0.2.1`. See `docs/DEVLOG.md` §10.8 and the `v0.2.1` release notes.
- **`build_and_install.sh` (the generic installer) doesn't work on immutable/atomic distros** (Bazzite, SteamOS/HoloISO, ChimeraOS) without the fix in this branch — it wrote to read-only `/usr` paths and silently reported success anyway. If you're on one of those distros, use `tools/setup_bazzite.sh` or `tools/setup_steamos.sh` instead, which write to paths that actually persist.
- **Releases before `v0.2.1` predate real-client validation.** `v0.2.0` and earlier were only ever tested against synthetic content; on real desktop/UI content they hit at least three now-fixed defects (an out-of-range QP field that broke the first client connection outright, a P-slice skip-decision bug that dropped chroma corrections, and an undersized bitstream buffer that silently truncated frames under real content). Use `v0.2.1` or later, or current `main`.
- **CI's green checkmark historically meant less than it looked like.** Two independent issues meant most of the test suite silently validated nothing for an unknown period: `assert()`-based tests were compiled with `-DNDEBUG` (which turns every `assert()` into a no-op), and a separate CMake configuration issue meant `ctest` never discovered 3 of the project's 4 test binaries in the first place. Both are now fixed and verified (reintroducing a known bug into the source causes the relevant test to actually fail loudly). If you're relying on this project's CI history from before that fix, treat it with the same skepticism you'd apply to an untested claim.

None of this means the underlying approach is a dead end — the GPU compute path is proven correct and cheap; the work left is squarely in the CPU-side bitstream writer, which is a much more tractable problem than a GPU architecture rework would have been.

---

## Installation

### Option A: Immutable / Atomic Distros (Bazzite, SteamOS, HoloISO, ChimeraOS) — Recommended, Verified Working

```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
sudo ./tools/setup_bazzite.sh   # or ./tools/setup_steamos.sh on SteamOS/HoloISO
```

Installs to a path that actually survives an ostree/atomic deployment update, and configures SELinux contexts where relevant.

### Option B: Traditional Distros (Fedora, Ubuntu, Arch, openSUSE)

```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
chmod +x build_and_install.sh tools/*.sh
./build_and_install.sh
```

### Option C: Pre-Built Release

Download `bc250_drv_video.so` and its matching `shaders/*.spv` from the [Releases page](../../releases) (`v0.2.1` or later — see [Known Limitations](#known-limitations) for why earlier releases are not recommended), place both at the repository root, then run the Option A or B installer for your distro. Both installers verify that compiled shaders actually landed and fail rather than silently reporting success if they didn't.

---

## Verifying Your Setup

```bash
./tools/bc250_diagnose.sh
```

Checks hardware identification, active Compute Unit count, the audio fix module, and whether the VA-API driver loads. Its built-in benchmark reports a real fps number for your specific board — expect it to land in the ranges given in [Known Limitations](#known-limitations), not at 60 fps yet.

```bash
export LIBVA_DRIVER_NAME=bc250
vainfo
```

Should list `VAProfileH264ConstrainedBaseline`, `VAProfileH264Baseline`, `VAProfileH264Main`, and `VAProfileH264High` with `VAEntrypointEncSlice` support.

For a real correctness check (not just "did it not crash"), run:

```bash
./tools/quality_test.sh
```

This independently verifies actual pixel content, not just that a bitstream decodes.

---

## Application Setup

### Sunshine / Moonlight

```bash
export LIBVA_DRIVER_NAME=bc250
```

Set **Video Encoder** to **VA-API** in Sunshine's web configuration (`https://localhost:47990` → Configuration → Audio/Video). A tuned config template is at `tools/sunshine_preset/sunshine.conf` — **read it before applying**: `apply_sunshine_preset.sh` fully overwrites your existing `sunshine.conf`, so if you've already hand-tuned settings like `capture` or a CSRF origin allowlist, back those up first or merge by hand instead of running the script blind.

Given the current performance ceiling above, expect this to be usable at lower resolutions/framerates sooner than at 1080p60.

### OBS Studio

```bash
LIBVA_DRIVER_NAME=bc250 obs
```

Settings → Output → Output Mode: Advanced → set **Video Encoder** to **FFmpeg VAAPI**, **VAAPI Device** to `/dev/dri/renderD128`.

### FFmpeg Command Line

```bash
export LIBVA_DRIVER_NAME=bc250
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 8M output.mp4
```

---

## Audio Fix (DisplayPort & HDMI)

The BC-250 has a known audio clock issue causing "drunk"/stuttering audio over DisplayPort or HDMI. A DKMS kernel module fixes it and persists across kernel updates:

```bash
cd audio-fix
sudo ./install_dkms.sh
```

This part of the project wasn't in scope for the correctness/performance verification work described above — it's carried over as-is from earlier work.

---

## Contributing / CI

`.github/workflows/build.yml` builds the driver, runs the full test suite (now actually exercising all four test binaries, see [Known Limitations](#known-limitations)), and runs `tools/quality_test.sh`-style content verification. A green check on a PR is a meaningfully stronger signal than it used to be, but two things are still worth knowing if you're relying on it: the test-suite step is configured with `continue-on-error`, so a genuine test failure currently shows as a warning rather than blocking the job, and the encode-verification step checks that ffmpeg decodes without a hard error, not pixel-level correctness (that's what `tools/quality_test.sh` is for, and it isn't part of the automated CI step yet).

Troubleshooting: [docs/troubleshooting.md](docs/troubleshooting.md)

---

## License
* Userspace compute driver, shaders, and tools: **GPL-3.0-only**
* Audio fix kernel module: **GPL-2.0-only** (inherited as-is from its original source; see `audio-fix/LICENSE`)

Chosen deliberately as copyleft, not a permissive license: anyone distributing a modified or combined version must release their source under the same terms, and GPL-3.0's anti-tivoization provisions specifically prevent shipping this code inside a locked-down device that blocks the end user from installing their own modified build.

<!-- bc250-vcn-driver -->

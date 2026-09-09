# AMD BC-250 Custom Driver & VA-API Video Encoder

[![Build & Release BC-250 Drivers](https://github.com/simpmix/bc250-vcn-driver/actions/workflows/build.yml/badge.svg)](https://github.com/simpmix/bc250-vcn-driver/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/Driver%20License-MIT-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

Software H.264 video encoding (via Vulkan compute, not the hardware VCN block) and a DisplayPort/HDMI audio clock fix for the **AMD BC-250 ("Cyan Skillfish" / PS5 "Oberon" APU)** on Linux (Bazzite, SteamOS, Fedora, Ubuntu, Arch).

> [!IMPORTANT]
> ### Current status: correct, but not yet real-time
> The encoder now produces genuinely correct H.264 output — verified numerically (PSNR/SSIM against ground truth) and visually, on real hardware, across multiple resolutions and clip lengths. **It is not yet fast enough for real-time streaming or recording at typical resolutions.** See [Known Limitations](#known-limitations) below before you plan around this project. If you're evaluating this for a "just works" streaming setup today, it isn't there yet — track that section for where the gap actually is and how it's closing.

---

## Why Does This Project Exist?

The AMD BC-250 is a repurposed PlayStation 5 APU with a Zen 2 CPU and up to 40 RDNA 2-class Compute Units (with the community-discovered CU unlock applied). That makes it an unusually cheap, powerful platform for a living-room gaming PC — except its VCN hardware video engine, needed for hardware-accelerated encode/decode, cannot currently be used. (Recent reverse-engineering suggests the block is physically present but stuck behind an unresolved power-management/firmware initialization problem, not permanently fused off — see the community's ongoing hardware-unlock effort, which is a separate project from this one.)

Without a working VCN, applications like Sunshine, OBS, and Steam Link fall back to CPU-only software encoding, which is either slow or absent depending on the app. This project's approach: emulate a hardware video encoder in software, using Vulkan compute shaders that run on the APU's own unlocked RDNA 2 Compute Units, and expose it to the rest of the system as a standard VA-API driver (`bc250_drv_video.so`) — so, in principle, any VA-API-aware application can use it exactly like it would a real hardware encoder.

This is a stopgap, not a replacement for real hardware acceleration. If and when the community's VCN-unlock effort succeeds, that will be the better answer for this chip. Until then, this project's goal is to make the software path as close to genuinely usable as it can get.

---

## What Actually Works Today

Validated on physical BC-250 hardware, not just in CI or in theory:

- **Correctness**: `tools/quality_test.sh` (an independent PSNR/SSIM harness — captures real ground-truth input, encodes through the actual VA-API pipeline, decodes with a separate software decoder, and scores the result) passes cleanly: **37.7 dB average PSNR, SSIM 0.99 at 640x480**; also passes at 1280x720 (30.9 dB) and across a 125-frame clip with no drift or accumulating error (worst single frame still 28.5 dB). This covers I-frames, P-frames/motion content, and multiple resolutions.
- **Installation on immutable distros**: `tools/setup_bazzite.sh` is verified working end-to-end on a real Bazzite (ostree/Kinoite) console — driver installs, persists, and is found by `vainfo` with the right H.264 profiles listed.
- **Test suite**: all four unit test binaries actually run and actually assert something (see [Known Limitations](#known-limitations) for why that's worth calling out explicitly).

## Known Limitations

Being direct about this rather than burying it, since it materially affects whether this project is useful for you yet:

- **Not real-time.** Measured throughput through the real pipeline: roughly **18 fps at 640x480, 6 fps at 1280x720, 3 fps at 1920x1080** — well below the 30-60 fps a streaming/recording use case needs, worse at higher resolution. The bottleneck is not the GPU compute shaders (they account for under 1.5% of frame time at every resolution tested) — it's the CPU-side CAVLC entropy-coding/bitstream-writing stage, which is measurably far slower than it should be. This is an active optimization target; it is not considered a project goal that's been abandoned.
- **GPU contention while gaming is real, not negligible.** Earlier documentation for this project claimed encoder overhead stays "under 3-5%" of GPU compute while a game runs concurrently. Direct measurement (a synthetic GPU load running alongside an active encode) shows roughly a **29% throughput hit** instead. Treat "stream while you game with no impact" as not yet true.
- **`build_and_install.sh` (the generic installer) doesn't work on immutable/atomic distros** (Bazzite, SteamOS/HoloISO, ChimeraOS) without the fix in this branch — it wrote to read-only `/usr` paths and silently reported success anyway. If you're on one of those distros, use `tools/setup_bazzite.sh` or `tools/setup_steamos.sh` instead, which write to paths that actually persist.
- **The `v0.2.0` tagged release predates the correctness fixes above.** If you downloaded a pre-built release before this work landed, it very likely produces visibly corrupted video on real content (a large flat region turning into a blotchy gray mess is the signature symptom) despite passing this project's CI at the time — see the next point for why CI didn't catch it.
- **CI's green checkmark historically meant less than it looked like.** Two independent issues meant most of the test suite silently validated nothing for an unknown period: `assert()`-based tests were compiled with `-DNDEBUG` (which turns every `assert()` into a no-op), and a separate CMake configuration issue meant `ctest` never discovered 3 of the project's 4 test binaries in the first place. Both are now fixed and verified (reintroducing a known bug into the source causes the relevant test to actually fail loudly). If you're relying on this project's CI history from before that fix, treat it with the same skepticism you'd apply to an untested claim.

None of this means the underlying approach is a dead end — the GPU compute path is proven correct and cheap; the work left is squarely in the CPU-side bitstream writer, which is a much more tractable problem than a GPU architecture rework would have been.

---

## Installation

### Option A: Immutable / Atomic Distros (Bazzite, SteamOS, HoloISO, ChimeraOS) — Recommended, Verified Working

```bash
git clone https://github.com/simpmix/bc250-vcn-driver.git
cd bc250-vcn-driver
sudo ./tools/setup_bazzite.sh   # or ./tools/setup_steamos.sh on SteamOS/HoloISO
```

Installs to a path that actually survives an ostree/atomic deployment update, and configures SELinux contexts where relevant.

### Option B: Traditional Distros (Fedora, Ubuntu, Arch, openSUSE)

```bash
git clone https://github.com/simpmix/bc250-vcn-driver.git
cd bc250-vcn-driver
chmod +x build_and_install.sh tools/*.sh
./build_and_install.sh
```

### Option C: Pre-Built Release

Check the [Actions tab](../../actions/workflows/build.yml) for the latest build artifact rather than the tagged Releases page for now, until a new release is cut that includes the correctness fixes described above.

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
* Userspace compute driver, shaders, and tools: **MIT**
* Audio fix kernel module: **GPL-2.0**

<!-- bc250-vcn-driver -->

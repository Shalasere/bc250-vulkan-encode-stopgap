# AMD BC-250 VA-API Video Encoder & Audio Fix

[![Build & Release BC-250 Drivers](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml/badge.svg)](https://github.com/Shalasere/bc250-vulkan-encode-stopgap/actions/workflows/build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/Driver%20License-GPL--3.0-blue.svg)](LICENSE)
[![Kernel Module: GPL-2.0](https://img.shields.io/badge/Audio%20Module-GPL--2.0-green.svg)](audio-fix/README.md)

A VA-API driver that implements video encoding with Vulkan compute shaders on
the **AMD BC-250** ("Cyan Skillfish", PCI `1002:13fe`), plus a DKMS kernel
module for DisplayPort/HDMI audio.

The BC-250's VCN hardware video engine is unavailable — the block is present
but cannot be initialised (see [docs/vcn-registers.md](docs/vcn-registers.md)).
This driver bypasses it by doing the encode on the RDNA 2 compute units, so
applications that speak VA-API — Sunshine, OBS, ffmpeg — see a normal hardware
encoder.

Tested on Bazzite; also targets CachyOS, Fedora, Ubuntu, Arch and ChimeraOS.

---

## What it supports

| | |
|---|---|
| **H.264** | Intra + inter, CABAC or CAVLC, multi-slice. Prediction, transform and reconstruction on the GPU. |
| **H.265 / HEVC** | Intra only. Off by default — see below. |
| **Decode** | Not supported. Encode only. |

Advertised VA-API profiles, all with `VAEntrypointEncSlice`:

```
VAProfileH264ConstrainedBaseline
VAProfileH264Main
VAProfileH264High
VAProfileHEVCMain              (only when BC250_ENABLE_HEVC=1)
```

### Measured performance

1080p on a physical BC-250, encoding through VA-API:

| Codec | Throughput |
|---|---|
| H.264, mixed I/P | 82 fps |
| H.264, all-intra | 54 fps |
| H.265, GPU reconstruction (`BC250_HEVC_GPU=1`) | ~46 fps |
| H.265, CPU reconstruction | ~7 fps |

**H.265 is not advertised by default.** Sunshine probes HEVC before H.264, so
advertising it would let a client negotiate the slower encoder. Enable it
deliberately with `BC250_ENABLE_HEVC=1`. Details, architecture and the full
measurement record: **[docs/hevc-encoder.md](docs/hevc-encoder.md)**.

---

## Install

### From a release

```bash
tar -xzf bc250-driver-linux-x86_64.tar.gz
cd bc250-driver
sudo ./build_and_install.sh
```

Pre-built archives are on the [Releases](../../releases) page, or as artifacts
of any [green CI run](../../actions/workflows/build.yml).

### From source

```bash
git clone https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git
cd bc250-vulkan-encode-stopgap
chmod +x build_and_install.sh tools/*.sh
./build_and_install.sh
```

The script installs missing build dependencies on Ubuntu, Fedora, Arch and
openSUSE.

### Immutable distributions

| Distribution | Command | Installs to |
|---|---|---|
| SteamOS 3.x / HoloISO | `sudo ./tools/setup_steamos.sh` | `/var/lib/bc250/` — survives A/B root updates |
| Bazzite / Fedora Silverblue | `sudo ./tools/setup_bazzite.sh` | `/usr/local/lib64/dri` — restores SELinux contexts |

### Audio fix

```bash
cd audio-fix
sudo ./install_dkms.sh      # ./uninstall_dkms.sh to remove
```

Corrects the BC-250's audio clock over DisplayPort and HDMI, which otherwise
produces stuttering or "drunk" playback. DKMS rebuilds it across kernel
updates.

---

## Configuration

All configuration is by environment variable.

`build_and_install.sh` writes these to `/etc/environment.d/99-bc250.conf`, so
an installed system starts with fast mode and 4 slices already on, not with
the code defaults below:

```
LIBVA_DRIVER_NAME=bc250
BC250_FAST_MODE=1
BC250_SLICES_PER_FRAME=4
```

### Required

| Variable | Purpose |
|---|---|
| `LIBVA_DRIVER_NAME=bc250` | Selects this driver |

### General

| Variable | Code default | Effect |
|---|---|---|
| `BC250_FAST_MODE` | off (installer sets `1`) | 2:1 checkerboard motion estimation, early diamond termination, skips the deblocking pass. Lowers GPU load at some quality cost. |
| `BC250_SLICES_PER_FRAME` | 1 (installer sets `4`) | Slices per frame, 1–16. More slices help network resilience and CPU-side entropy parallelism. |
| `BC250_USE_CABAC` | on for non-Baseline profiles | `0` forces CAVLC. |
| `BC250_PERF_STATS` | off | Per-stage GPU timings to stderr. |

### H.265

| Variable | Default | Effect |
|---|---|---|
| `BC250_ENABLE_HEVC` | off | Advertise HEVC through VA-API at all |
| `BC250_HEVC_GPU` | off | Run reconstruction on the GPU; CPU keeps only entropy coding |
| `BC250_HEVC_QP` | 27 | Constant QP, 1–51 |
| `BC250_HEVC_MAX_TB` | 16 | MaxTbLog2SizeY in pixels: 4, 8 or 16 |
| `BC250_HEVC_FLAT` | 32 | CU-merge threshold, in eighths of the quantizer step |

---

## Verify

```bash
./tools/bc250_diagnose.sh
```

Checks hardware identification, the audio subsystem, VA-API driver loading,
and runs an encode benchmark.

Directly:

```bash
export LIBVA_DRIVER_NAME=bc250
vainfo
```

---

## Application setup

### Sunshine

1. `LIBVA_DRIVER_NAME=bc250` in the service environment.
2. Web UI (`https://localhost:47990`) → **Configuration → Audio/Video**.
3. **Video Encoder**: VA-API.

### OBS Studio

```bash
LIBVA_DRIVER_NAME=bc250 obs
```

**Settings → Output → Output Mode: Advanced**, encoder **FFmpeg VAAPI**,
device `/dev/dri/renderD128`.

### ffmpeg

```bash
export LIBVA_DRIVER_NAME=bc250
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
       -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 8M output.mp4
```

---

## Documentation

| | |
|---|---|
| [docs/hevc-encoder.md](docs/hevc-encoder.md) | H.265 encoder: usage, architecture, measurements, and optimisations that were tried and measured as worthless |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Troubleshooting guide |
| [docs/testing-guide.md](docs/testing-guide.md) | Test suite and validation |
| [docs/hardware-notes.md](docs/hardware-notes.md) | BC-250 hardware notes |
| [docs/vcn-registers.md](docs/vcn-registers.md) | VCN register map and why the block is unavailable |

### Common problems

| Symptom | Fix |
|---|---|
| `Permission denied` on `/dev/dri/renderD128` | `sudo usermod -a -G video,render $USER`, then log out and back in |
| `Driver bc250 not found` | `export LIBVA_DRIVER_NAME=bc250` |
| Audio still stuttering | `lsmod \| grep bc250_audio_fix`; if absent, `sudo modprobe bc250_audio_fix` |
| HEVC not offered to a client | Expected. Set `BC250_ENABLE_HEVC=1` — but read the performance table first |

---

## Releases

Tagging triggers CI to build, test, package and publish:

```bash
git tag v0.3.3
git push origin v0.3.3
```

---

## License

| Component | License |
|---|---|
| Userspace driver, shaders, tools | GPL-3.0-only |
| Audio fix kernel module | GPL-2.0 |

Relicensed from MIT. The H.264/H.265 CABAC entropy coders adapt code from
x264/x265, both GPL.

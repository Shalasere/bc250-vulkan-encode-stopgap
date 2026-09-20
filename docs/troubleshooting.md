# AMD BC-250 Driver Troubleshooting & Diagnostic Guide

This guide covers solutions to all known issues when using the AMD BC-250 (Cyan Skillfish) custom VA-API compute driver and audio fix on Linux distributions (Bazzite, Fedora, Ubuntu, Arch, CachyOS, ChimeraOS).

---

## Quick Diagnostic Check

Before troubleshooting individual issues, run the built-in diagnostic tool from your terminal:

```bash
cd /path/to/bc250-vulkan-encode-stopgap
./tools/bc250_diagnose.sh
```

This automated script checks your APU identification, active Compute Units (CUs), audio driver status, VA-API profile queries, and runs a live 100-frame encode benchmark.

---

## 1. DisplayPort / HDMI Audio Issues ("Drunk" or Stuttering Audio)

### Symptoms
* Audio over DisplayPort or HDMI sounds distorted, robotic, pitched down, or stutters ("drunk audio").
* Audio drops out completely when switching resolutions.

### Cause
The Cyan Skillfish display controller uses a non-standard clock divisor in the `amdgpu` display engine (`dc`), calculating the wrong sample clock for standard 48kHz audio.

### Solution
Install the DKMS audio fix module, which dynamically applies the correct clock configuration:

```bash
cd audio-fix
sudo ./install_dkms.sh
```

**Verify the fix is active:**
```bash
lsmod | grep bc250_audio_fix
```
If active, you will see `bc250_audio_fix` listed. Because this is registered with **DKMS**, it will automatically rebuild and persist whenever you update your Linux kernel!

---

## 2. "vaInitialize failed: driver bc250 not found"

### Symptoms
* Running `vainfo` or launching OBS reports:
  ```
  vaInitialize failed with error code -1 (unknown libva error),exit
  ```
  or
  ```
  vaGetDriverNameByIndex() failed with unknown libva error, driver_name = (null)
  ```

### Cause
The VA-API loader (`libva`) looks for driver libraries in specific DRI directories depending on your Linux distribution (`/usr/lib64/dri/`, `/usr/lib/x86_64-linux-gnu/dri/`, or `/usr/lib/dri/`). If `bc250_drv_video.so` is missing from the directory expected by your distro, it will fail to load.

### Solution
1. Ensure `LIBVA_DRIVER_NAME=bc250` is set in your environment:
   ```bash
   export LIBVA_DRIVER_NAME=bc250
   ```
2. Re-link the driver into all common system DRI paths:
   ```bash
   sudo mkdir -p /usr/lib/x86_64-linux-gnu/dri /usr/lib64/dri /usr/lib/dri
   sudo cp -f approach1-compute-encoder/build/bc250_drv_video.so /usr/lib/x86_64-linux-gnu/dri/
   sudo cp -f approach1-compute-encoder/build/bc250_drv_video.so /usr/lib64/dri/
   sudo cp -f approach1-compute-encoder/build/bc250_drv_video.so /usr/lib/dri/
   ```
3. Test again:
   ```bash
   LIBVA_DRIVER_NAME=bc250 vainfo
   ```

---

## 3. Permission Denied on `/dev/dri/renderD128`

### Symptoms
* `vainfo` or FFmpeg fails with:
  ```
  Failed to open /dev/dri/renderD128: Permission denied
  ```
* Sunshine reports encoder initialization error when launched as a non-root user.

### Cause
Your user account does not have read/write access to the GPU render node.

### Solution
Add your user account to the `video` and `render` groups:
```bash
sudo usermod -a -G video,render $USER
```
Then log out and log back in (or reboot) for the permissions to take effect. Verify with:
```bash
groups
```
Ensure `render` is listed in the output.

---

## 4. Sunshine / Moonlight Streaming Issues

### Symptom A: Moonlight shows black screen or immediate disconnect
* **Solution 1:** In the Sunshine Web UI (**Configuration -> Audio/Video**):
  * Set **Video Encoder** to `VA-API`.
  * Ensure **Resolution** matches a standard 16:9 ratio (1280x720, 1920x1080, or 2560x1440).
* **Solution 2:** Enable the low-overhead gaming mode in your environment:
  ```bash
  export BC250_FAST_MODE=1
  ```
  This bypasses the deblock filter and uses 2:1 subsampled motion estimation, keeping frame latency below 5ms!

### Symptom B: Stream drops frames when game graphics are demanding
* The BC-250 driver automatically prioritizes the GPU's dedicated **Async Compute Engine (ACE)** queues so encoding does not stall graphics rendering.
* Ensure you are running in fast mode:
  ```bash
  export BC250_FAST_MODE=1
  ```
  This keeps encoder GPU load under **3–5% of the 40 CUs**, leaving the rest of the APU for the game.

---

## 5. OBS Studio: "Failed to open video codec"

### Symptoms
* Clicking **Start Recording** or **Start Streaming** in OBS results in:
  ```
  Starting the output failed. Please check the log for details.
  Error: Failed to open video codec: Function not implemented (-40)
  ```

### Solution
1. Launch OBS from the terminal with the driver specified:
   ```bash
   LIBVA_DRIVER_NAME=bc250 obs
   ```
2. In OBS:
   * Go to **Settings -> Output -> Output Mode: Advanced**.
   * Under the **Streaming** or **Recording** tab, select **FFmpeg VAAPI**.
   * Set **VAAPI Device** to `/dev/dri/renderD128`.
   * Under **Profile**, select **Main** or **High**.

---

## 6. Verifying 40 Compute Units (CUs) vs 24 CUs

### Check Status
Run:
```bash
cat /sys/class/drm/card0/device/current_compute_units 2>/dev/null || dmesg | grep -i "compute units"
```
* **Expected:** 40 active CUs.
* **If it reports 24 CUs:** Your kernel or BIOS is limiting the APU to its stock crypto-mining default (24 CUs / 12 WGPs). This project does *not* bundle a kernel unlock patch because modifying compute unit allocation requires an `amdgpu` kernel driver patch. To unlock all 40 CUs (20 WGPs):
  1. Install an APU-optimized distribution like **Bazzite** (BC-250 / Deck image) or **SkillFishOS**, which pre-integrates the 40 CU unlock out of the box.
  2. Or apply the community kernel patch from **[duggasco/bc250-40cu-unlock](https://github.com/duggasco/bc250-40cu-unlock)** using module parameter `amdgpu.bc250_cc_write_mode=3`.
  * Note: Once unlocked, this driver's compute shaders automatically dispatch across all 40 CUs with <3–5% overhead!

---

## 7. SteamOS & Bazzite (Immutable Distribution) Considerations

### SteamOS / HoloISO
* **Why `/var/lib/bc250`?** SteamOS uses an A/B partition layout where the root partition (`/` and `/usr`) is completely overwritten during system upgrades (e.g. SteamOS 3.5 → 3.6). The `/var` and `/etc` partitions are persistent state partitions. By installing the driver to `/var/lib/bc250/dri` and configuring `/etc/environment.d/99-bc250.conf`, your driver survives all future OS updates.
* **Kernel headers for Audio Fix:** If `setup_steamos.sh` notes missing kernel headers for DKMS, run:
  ```bash
  sudo steamos-readonly disable
  sudo pacman -S --needed linux-neptune-headers dkms
  sudo ./tools/setup_steamos.sh
  ```

### Bazzite / Fedora Silverblue
* **SELinux context denials:** On Fedora Silverblue and Bazzite, SELinux is set to Enforcing. If Gamescope or Sunshine fails to load the driver from `/usr/local/lib64/dri/`, restore SELinux file contexts:
  ```bash
  sudo restorecon -Rv /usr/local/lib64/dri /usr/local/share/bc250
  ```
* **Kernel headers on Bazzite:** If DKMS fails to compile the audio fix:
  ```bash
  ujust install-kernel-headers
  sudo ./tools/setup_bazzite.sh
  ```

---

## 8. FFmpeg Warning: "Driver does not support some wanted packed headers (wanted 0xd, found 0)"

### Symptoms
When encoding with FFmpeg's `h264_vaapi` or `hevc_vaapi`, the console prints:
```text
[h264_vaapi @ 0x560ddcc95dc0] Driver does not support some wanted packed headers (wanted 0xd, found 0).
```

### Cause
* In VA-API, "packed headers" refers to the client application (FFmpeg) generating raw NAL headers (SPS, PPS, Slice, SEI) and asking the hardware driver to splice them into the stream.
* FFmpeg requests packed headers bitmask `0xd` (`VA_ENC_PACKED_HEADER_SEQUENCE (0x1) | VA_ENC_PACKED_HEADER_SLICE (0x4) | VA_ENC_PACKED_HEADER_MISC (0x8)`).
* The BC-250 driver reports `0` (`VA_ENC_PACKED_HEADER_NONE`) because it is a self-contained Vulkan compute encoder that **authors and embeds its own conforming in-band AUD, SPS, PPS, and Slice NAL units** directly into the bitstream.
* Previously, the driver advertised `0x7`, which caused FFmpeg to generate external `extradata` (avcC) that could desync from the driver's actual in-band headers during MP4/MKV muxing. Reporting `NONE` guarantees that container muxers preserve the driver's authoritative in-band headers.

### Action Needed
* **None — this is a harmless informational warning, not an error.**
* Encoding completes normally, and all output streams contain valid in-band headers that decoders (including FFmpeg itself) parse cleanly.

---

## 9. 32-bit VA-API Clients (Steam Link)

### Symptoms
* Steam Link silently falls back to software encoding or fails to initialize hardware acceleration.
* Log reports that `bc250_drv_video.so` cannot be loaded or is the wrong ELF class.

### Cause
* Steam Link's client runtime is a 32-bit process and cannot load 64-bit `.so` drivers from `/usr/lib64/dri` or `/usr/lib/x86_64-linux-gnu/dri`.

### Solution
1. Download the companion 32-bit driver archive `bc250-driver-linux-i386.tar.gz` from Releases.
2. Install it to your system's 32-bit DRI directory (leaving the 64-bit driver in place for 64-bit apps):
   ```bash
   tar -xzvf bc250-driver-linux-i386.tar.gz
   sudo install -Dm755 bc250-driver-i386/bc250_drv_video.so /usr/lib32/dri/bc250_drv_video.so
   # Or on Debian/Ubuntu multiarch:
   # sudo install -Dm755 bc250-driver-i386/bc250_drv_video.so /usr/lib/i386-linux-gnu/dri/bc250_drv_video.so
   ```
3. Shaders are shared at `/usr/share/bc250/shaders` from the 64-bit install — no extra shaders needed.

---

## 10. How to Completely Uninstall the Driver

Use the automated uninstaller script:

```bash
# Preview what will be removed without changing anything:
sudo ./tools/bc250_uninstall.sh --dry-run

# Perform full cleanup:
sudo ./tools/bc250_uninstall.sh

# Remove the DKMS audio fix:
cd audio-fix && sudo ./uninstall_dkms.sh
```

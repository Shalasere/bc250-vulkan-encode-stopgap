# BC-250 Hardware Notes

## APU Specifications
- **Architecture**: Zen 2 CPU (8 cores / 16 threads) + RDNA 2 GPU (gfx1013)
- **Compute Units**: 40 CUs (20 WGPs, physically present on the PS5-derived die).
  - *Stock Mining Board State*: Often ships software-limited to 24 CUs (12 WGPs).
  - *40 CU Unlock*: Re-enabled via the community `amdgpu` kernel patch ([duggasco/bc250-40cu-unlock](https://github.com/duggasco/bc250-40cu-unlock)) or distributions like Bazzite/SkillFishOS.
- **Hardware Video Block**: VCN 2.0.3 (Video Core Next, `UVD_VERSION = 0x0002001B`) — not VCN 3.0 as earlier notes here said; corrected against this project's own separate VCN-enablement research, which instantiates it on this exact board as `vcn_v2_0`. Physically present on the die but permanently unprovisioned and unmanaged by SMU/VBIOS/PSP for this SKU.
- **Codename**: Cyan Skillfish (Device ID: `1002:13fe`)

## Memory Map & Architecture
- **Unified GDDR6 Pool**: All 16 GB is a single unified pool of high-bandwidth GDDR6 shared by CPU and GPU.
- **VRAM vs GTT**: The `512 MB` reported in `mem_info_vram_total` is merely a kernel-level label for the initial aperture slice; the GPU accesses the unified memory via GART/GTT (`amdgpu.gttsize`). Vulkan RADV exposes ~7.95 GiB across two heaps. Attempting to force larger "dedicated VRAM" in BIOS/APCB is ineffective and unnecessary.

## Known Hardware Realities
1. **Physical VCN is Unusable & Permanently Fused**:
   - Factory eFuses permanently disable the hardware VCN block. Upstream Mesa 25.1 added native `gfx1013` RADV Vulkan support, but does not provide hardware VCN decoding/encoding. Physical VCN cannot be unlocked.
   - This driver (`bc250-vulkan-encode-stopgap`, formerly `bc250-encoding-decoding-fix`, formerly `bc250-vcn-driver`) provides the community's sole hardware-accelerated encoding solution by translating VA-API calls into Vulkan Compute shaders executed on the RDNA2 CUs.
2. **DisplayPort / HDMI Audio Clock Divisor**:
   - The display controller (`dc`) calculates an incorrect audio sample clock divisor for 44.1/48 kHz audio. The included `bc250_audio_fix` DKMS module writes the proper clock ratios directly to APU DCCG registers (`0x05E0`, `0x05E4`, `0x05E8`).

<!-- bc250-vulkan-encode-stopgap v0.4.0 -->

/*
 * bc250_sunshine_shim.c
 *
 * LD_PRELOAD shim that reproduces the kmsgrab.cpp source patch without
 * requiring a Sunshine rebuild.
 *
 * Sunshine's zero-copy VAAPI capture path (kms::display_vram_t) samples
 * the captured KMS plane's imported dma-buf texture through a GLSL
 * shader (egl::sws_t) that assumes an 8-bit-per-channel source. When
 * the desktop compositor scans out at higher bit depth (e.g. KWin
 * defaulting to 10-bit/ARGB2101010 whenever HDR capability is merely
 * advertised, even with HDR disabled), that shader path produces
 * corrupted color. Sunshine's own RAM capture path (kms::display_ram_t)
 * reads the identical imported texture back via a plain
 * glGetTextureSubImage() call, which correctly normalizes any source
 * depth -- and it already supports real VAAPI hardware encoding too
 * (kms_display() falls back to it whenever display_vram_t::init()
 * fails), so the fix is simply to make that fallback trigger.
 *
 * display_vram_t::init() gates zero-copy eligibility on
 * va::validate(), whose first call is vaInitialize(). This shim:
 *
 *   1. Hooks drmModeGetFB2() (libdrm) -- the call display_t::init()
 *      makes to read the target monitor's plane framebuffer -- and
 *      records whether its pixel_format is 8bpc-safe.
 *   2. Hooks vaInitialize() (libva) and forces failure on exactly the
 *      first call (which is always va::validate()'s probe, always
 *      before any real capture/encode has started) when the recorded
 *      format was unsafe. Every later call -- the real encode session
 *      set up once Sunshine has fallen back to display_ram_t+vaapi --
 *      passes through untouched.
 *
 * Deploy via the existing systemd user drop-in:
 *   Environment=LD_PRELOAD=/opt/bc250-driver/bc250_sunshine_shim.so
 *
 * Build (no libdrm-devel/libva-devel needed -- all types below are
 * hand-declared to the stable, documented ABI so this has zero build
 * dependencies beyond a plain C compiler):
 *   gcc -shared -fPIC -O2 -o bc250_sunshine_shim.so bc250_sunshine_shim.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

/* --- drm_fourcc.h values (stable kernel UAPI, never change) --- */
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34325241u

/* --- xf86drmMode.h: exact layout of drmModeFB2, verified against
 * libdrm 2.4.125 (Ubuntu 24.04 libdrm-dev) --- */
typedef struct _drmModeFB2 {
  uint32_t fb_id;
  uint32_t width, height;
  uint32_t pixel_format; /* fourcc code from drm_fourcc.h */
  uint64_t modifier;
  uint32_t flags;
  uint32_t handles[4];
  uint32_t pitches[4];
  uint32_t offsets[4];
} drmModeFB2, *drmModeFB2Ptr;

/* --- va.h: vaInitialize's signature is part of libva's stable,
 * documented ABI --- */
typedef void *VADisplay;
typedef int VAStatus;
#define VA_STATUS_SUCCESS 0x00000000
#define VA_STATUS_ERROR_OPERATION_FAILED 0x00000008

static volatile int g_last_fb_unsafe = 0;
static volatile int g_va_probe_consumed = 0;

drmModeFB2Ptr drmModeGetFB2(int fd, uint32_t bufferId) {
  static drmModeFB2Ptr (*real)(int, uint32_t) = NULL;
  if (!real) {
    real = (drmModeFB2Ptr (*) (int, uint32_t)) dlsym(RTLD_NEXT, "drmModeGetFB2");
  }

  drmModeFB2Ptr fb = real(fd, bufferId);
  if (fb) {
    int unsafe = (fb->pixel_format != DRM_FORMAT_XRGB8888 &&
                  fb->pixel_format != DRM_FORMAT_ARGB8888);
    g_last_fb_unsafe = unsafe;
    fprintf(stderr,
            "[bc250-shim] drmModeGetFB2: fb_id=%u fourcc=0x%08x unsafe=%d\n",
            fb->fb_id, fb->pixel_format, unsafe);
  }
  return fb;
}

VAStatus vaInitialize(VADisplay dpy, int *major_version, int *minor_version) {
  static VAStatus (*real)(VADisplay, int *, int *) = NULL;
  if (!real) {
    real = (VAStatus (*) (VADisplay, int *, int *)) dlsym(RTLD_NEXT, "vaInitialize");
  }

  if (!g_va_probe_consumed) {
    g_va_probe_consumed = 1;
    if (g_last_fb_unsafe) {
      fprintf(stderr,
              "[bc250-shim] vaInitialize: failing the display_vram_t "
              "validate() probe (unsafe source pixel format) to force "
              "the display_ram_t+vaapi fallback\n");
      return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    fprintf(stderr,
            "[bc250-shim] vaInitialize: validate() probe passed through "
            "(safe 8bpc source format)\n");
  }
  return real(dpy, major_version, minor_version);
}

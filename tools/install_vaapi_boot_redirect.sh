#!/usr/bin/env bash
# Installs bc250-vaapi-boot-redirect.service so the radeonsi VA-API driver
# slot is redirected to bc250_drv_video.so on every boot, not just the
# current one. Needed specifically for Sunshine (and anything else running
# with a `cap_sys_admin`-style file capability): the kernel's secure-exec
# mode makes glibc's secure_getenv() - which libva uses for
# LIBVA_DRIVER_NAME/LIBVA_DRIVERS_PATH precisely because those variables
# control which shared library gets loaded into a privileged process -
# return nothing, regardless of what the shell environment actually holds.
# A plain-old unprivileged consumer (vainfo, ffmpeg, OBS) never hits this;
# only a capability-holding one does. See docs/DEVLOG.md for the full
# root-cause writeup and how this was confirmed.
#
# Run as: sudo ./tools/install_vaapi_boot_redirect.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root (sudo)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_SRC="$SCRIPT_DIR/bc250-vaapi-boot-redirect.service"
UNIT_DST=/etc/systemd/system/bc250-vaapi-boot-redirect.service

if [ ! -f "$UNIT_SRC" ]; then
    echo "Can't find $UNIT_SRC" >&2
    exit 1
fi

cp "$UNIT_SRC" "$UNIT_DST"
systemctl daemon-reload
systemctl enable bc250-vaapi-boot-redirect.service
systemctl start bc250-vaapi-boot-redirect.service

echo "=== verifying ==="
systemctl is-active bc250-vaapi-boot-redirect.service
ls -la /usr/lib64/dri/radeonsi_drv_video.so

echo
echo "Installed and enabled. This will now reapply itself on every boot,"
echo "before sddm starts, so Sunshine's own service (which starts later,"
echo "after the graphical session) always sees the redirect already in place."

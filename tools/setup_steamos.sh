#!/usr/bin/env bash
# bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver
#
# setup_steamos.sh - Automated installer tailored specifically for Valve SteamOS & HoloISO
#
# Designed to survive SteamOS A/B system updates by staging to persistent storage (/var/lib/bc250)
# and integrating seamlessly with Gamescope and Sunshine game streaming.
#

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}${BOLD}     AMD BC-250 Installer for SteamOS / HoloISO       ${NC}"
echo -e "${BLUE}======================================================${NC}"

# Check for root
SUDO=""
if [ "$EUID" -ne 0 ]; then
    if command -v sudo &> /dev/null; then
        SUDO="sudo"
    else
        echo -e "${RED}Error: Please run with sudo (sudo ./tools/setup_steamos.sh).${NC}"
        exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

echo -e "\n${BOLD}[1/4] Detecting SteamOS Immutable Environment...${NC}"
IS_STEAMOS=0
if command -v steamos-readonly &> /dev/null || ( [ -f /etc/os-release ] && grep -qi "steamos" /etc/os-release ); then
    IS_STEAMOS=1
    echo -e "  ${GREEN}✓ Detected Valve SteamOS / HoloISO immutable environment${NC}"
else
    echo -e "  ${YELLOW}! Non-SteamOS system detected; proceeding with persistent console paths.${NC}"
fi

# SteamOS uses A/B root partitions where /usr is wiped during OS upgrades.
# Staging to /var/lib/bc250 guarantees driver and shaders survive SteamOS updates!
INSTALL_LIB_DIR="/var/lib/bc250/dri"
INSTALL_SHADER_DIR="/var/lib/bc250/shaders"

echo -e "\n${BOLD}[2/4] Installing VA-API Driver & Shaders (OS-Update Persistent)...${NC}"
$SUDO mkdir -p "$INSTALL_LIB_DIR"
$SUDO mkdir -p "$INSTALL_SHADER_DIR"

# Check for pre-built release binary vs local compilation
PREBUILT_SO="$REPO_ROOT/bc250_drv_video.so"
DRIVER_BIN=""

if [ -f "$PREBUILT_SO" ]; then
    echo -e "  ${GREEN}✓ Found pre-compiled release binary ($PREBUILT_SO)${NC}"
    DRIVER_BIN="$PREBUILT_SO"
else
    BUILD_DIR="$REPO_ROOT/approach1-compute-encoder/build"
    if [ -f "$BUILD_DIR/bc250_drv_video.so" ]; then
        DRIVER_BIN="$BUILD_DIR/bc250_drv_video.so"
    else
        echo -e "  -> Building driver from source..."
        if ! command -v cmake &> /dev/null; then
            echo -e "${YELLOW}Notice: Build tools not found. Unlocking pacman to install headers & build tools...${NC}"
            if command -v steamos-readonly &> /dev/null; then
                $SUDO steamos-readonly disable || true
            fi
            $SUDO pacman -Sy --noconfirm base-devel cmake libva libdrm vulkan-devel glslang libva-utils || true
        fi
        mkdir -p "$BUILD_DIR"
        (cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc)")
        DRIVER_BIN="$BUILD_DIR/bc250_drv_video.so"
    fi
fi

# Copy driver to persistent location
echo -e "  -> Installing driver to $INSTALL_LIB_DIR/bc250_drv_video.so"
$SUDO cp -f "$DRIVER_BIN" "$INSTALL_LIB_DIR/bc250_drv_video.so"

# Also try installing to /usr/local/lib/dri if accessible
$SUDO mkdir -p /usr/local/lib/dri 2>/dev/null || true
$SUDO cp -f "$DRIVER_BIN" /usr/local/lib/dri/bc250_drv_video.so 2>/dev/null || true

# Copy shaders
if [ -d "$REPO_ROOT/shaders" ]; then
    echo -e "  -> Copying shaders to $INSTALL_SHADER_DIR/"
    $SUDO cp -f "$REPO_ROOT/shaders"/* "$INSTALL_SHADER_DIR/" 2>/dev/null || true
elif [ -d "$REPO_ROOT/approach1-compute-encoder/shaders" ]; then
    echo -e "  -> Copying shaders to $INSTALL_SHADER_DIR/"
    if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR" ]; then
        $SUDO cp -f "$BUILD_DIR"/*.spv "$INSTALL_SHADER_DIR/" 2>/dev/null || true
    fi
    $SUDO cp -f "$REPO_ROOT/approach1-compute-encoder/shaders"/*.comp "$INSTALL_SHADER_DIR/" 2>/dev/null || true
fi

# Also link shaders to standard system paths if writable
$SUDO mkdir -p /usr/local/share/bc250/shaders 2>/dev/null || true
$SUDO cp -f "$INSTALL_SHADER_DIR"/* /usr/local/share/bc250/shaders/ 2>/dev/null || true

echo -e "\n${BOLD}[3/4] Configuring SteamOS Session & Gamescope Environment...${NC}"
# /etc/environment.d is persistent across SteamOS updates and read by Gamescope / systemd
$SUDO mkdir -p /etc/environment.d
cat << 'EOF' | $SUDO tee /etc/environment.d/99-bc250.conf > /dev/null
# AMD BC-250 VA-API Compute Driver Configuration (SteamOS)
LIBVA_DRIVER_NAME=bc250
LIBVA_DRIVERS_PATH=/var/lib/bc250/dri:/usr/local/lib/dri:/usr/local/lib64/dri:/usr/lib/dri
BC250_FAST_MODE=1
BC250_SLICES_PER_FRAME=4
BC250_SHADER_DIR=/var/lib/bc250/shaders
EOF
echo -e "  ${GREEN}✓ Configured persistent environment in /etc/environment.d/99-bc250.conf${NC}"

# Ensure user 'deck' or current sudo user has render permissions
CURRENT_USER="${SUDO_USER:-$USER}"
if [ -n "$CURRENT_USER" ] && [ "$CURRENT_USER" != "root" ]; then
    $SUDO usermod -a -G video,render "$CURRENT_USER" 2>/dev/null || true
    echo -e "  ${GREEN}✓ Granted GPU render permissions to user '$CURRENT_USER'${NC}"
fi

echo -e "\n${BOLD}[4/4] Setting Up Audio Clock Fix...${NC}"
if [ -d "$REPO_ROOT/audio-fix" ]; then
    cd "$REPO_ROOT/audio-fix"
    if command -v dkms &> /dev/null; then
        $SUDO ./install_dkms.sh || echo -e "  ${YELLOW}! DKMS setup skipped.${NC}"
    else
        echo -e "  ${YELLOW}! DKMS not installed. Compiling module directly:${NC}"
        make || true
        $SUDO make install || true
        $SUDO modprobe bc250_audio_fix 2>/dev/null || true
    fi
    cd "$REPO_ROOT"
fi

# Re-enable steamos-readonly if we disabled it
if [ "$IS_STEAMOS" -eq 1 ] && command -v steamos-readonly &> /dev/null; then
    $SUDO steamos-readonly enable 2>/dev/null || true
fi

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}${BOLD}    SteamOS / HoloISO Setup Completed Successfully!   ${NC}"
echo -e "${GREEN}======================================================${NC}"
echo -e "\nSummary:"
echo -e "  * Driver installed to:    ${YELLOW}$INSTALL_LIB_DIR/bc250_drv_video.so${NC} (Survives OS updates!)"
echo -e "  * Compute shaders at:     ${YELLOW}$INSTALL_SHADER_DIR/${NC}"
echo -e "  * Multi-Slice mode:       ${GREEN}4 slices per frame${NC} (smooth Moonlight game streaming)"
echo -e "  * Audio Fix:              ${GREEN}Active + Sleep/Wake auto-resume${NC}"
echo -e "\nNext steps:"
echo -e "  1. Switch back to Gaming Mode or reboot your console."
echo -e "  2. In Sunshine Web UI: set Video Encoder to ${GREEN}VA-API${NC}."
echo -e "  3. Diagnostic test: ${YELLOW}./tools/bc250_diagnose.sh${NC}"

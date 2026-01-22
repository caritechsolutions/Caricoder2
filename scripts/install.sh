#!/bin/bash
#
# CariTranscoder Installation Script
# Copyright (c) 2024 CariTech Solutions
#
# Usage: curl -sSL https://raw.githubusercontent.com/caritechsolutions/Caricoder2/main/scripts/install.sh | sudo bash
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_VERSION="1.0.3"
GSTREAMER_VERSION="1.26.1"
INSTALL_DIR="/opt/caritrans"
CONFIG_DIR="/etc/caritrans"
WEB_DIR="/var/www/caritrans"
LOG_DIR="/var/log/caritrans"
RUN_DIR="/run/caritrans"
DATA_DIR="/var/lib/caritrans"
REPO_URL="https://github.com/caritechsolutions/Caricoder2"
BRANCH="claude/implement-transcoder-output-8LaWa"
SERVICE_USER="caritrans"
WEB_USER="www-data"

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

# Check OS
check_os() {
    log_step "Checking operating system..."

    if [[ ! -f /etc/os-release ]]; then
        log_error "Cannot detect OS. /etc/os-release not found."
        exit 1
    fi

    source /etc/os-release

    if [[ "$ID" != "ubuntu" && "$ID" != "debian" ]]; then
        log_error "This script is designed for Ubuntu/Debian. Detected: $ID"
        exit 1
    fi

    log_info "Detected OS: $PRETTY_NAME"
}

# Install dependencies
install_dependencies() {
    log_step "Installing dependencies..."

    apt-get update

    # Build tools
    apt-get install -y \
        build-essential \
        pkg-config \
        git \
        curl \
        wget

    # GStreamer core and plugins
    apt-get install -y \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-tools

    # GStreamer libav (provides avdec_h264, avdec_ac3, avdec_eac3, avenc_* etc.)
    apt-get install -y gstreamer1.0-libav || true

    # Additional GStreamer codec plugins
    # Note: x264 encoder is already in gstreamer1.0-plugins-ugly
    # Note: AAC encoder is in gstreamer1.0-libav (avenc_aac)
    apt-get install -y gstreamer1.0-vaapi || true

    # GStreamer video processing plugins (for deinterlacing, scaling, etc.)
    apt-get install -y \
        libgstreamer-plugins-bad1.0-dev || true

    # FFmpeg (for stream analysis and fallback transcoding)
    apt-get install -y ffmpeg

    # SRT support - package names vary by Ubuntu version
    # Ubuntu 24.04: libsrt-openssl-dev, Ubuntu 22.04: libsrt-openssl-dev, Ubuntu 20.04: libsrt-dev
    apt-get install -y libsrt-openssl-dev 2>/dev/null || apt-get install -y libsrt-gnutls-dev 2>/dev/null || apt-get install -y libsrt-dev 2>/dev/null || true

    # SRT tools (srt-live-transmit for stream reception)
    apt-get install -y srt-tools 2>/dev/null || true

    # Build tools for librist
    apt-get install -y meson ninja-build cmake || true

    # libmicrohttpd (HTTP server library, required by librist)
    apt-get install -y libmicrohttpd-dev || true

    # SSL/Crypto
    apt-get install -y libssl-dev

    # PHP for web interface
    apt-get install -y \
        php-cli \
        php-fpm \
        php-json \
        php-mbstring \
        php-curl

    # Python for privileged API service
    apt-get install -y \
        python3 \
        python3-pip \
        python3-venv

    # Nginx (optional, for production)
    apt-get install -y nginx || true

    log_info "Dependencies installed successfully"
}

# Install TSDuck from pre-built package
install_tsduck() {
    log_step "Installing TSDuck..."

    # Check if TSDuck is installed AND working (not just present)
    if command -v tsp &> /dev/null; then
        if tsp --version &> /dev/null; then
            log_info "TSDuck already installed: $(tsp --version 2>&1 | head -1)"
            return 0
        else
            # TSDuck exists but is broken (library issues) - remove it
            log_warn "TSDuck is installed but broken, removing..."
            dpkg --purge tsduck 2>/dev/null || true
            apt-get remove --purge tsduck -y 2>/dev/null || true
        fi
    fi

    # Detect architecture and OS version
    local ARCH=$(dpkg --print-architecture)
    source /etc/os-release
    log_info "Detected: $ARCH on $PRETTY_NAME ($VERSION_CODENAME)"

    # Determine TSDuck version and package name based on Ubuntu version
    # Note: Ubuntu 24 packages use different ABI (libssl3t64, libcurl4t64) and won't work on older Ubuntu
    # Available packages in repo: ubuntu20 (3.26-2349), ubuntu24 (3.43-4524)
    # Ubuntu 22 must be downloaded from GitHub
    local TSDUCK_VERSION=""
    local UBUNTU_TAG=""
    local DOWNLOAD_ONLY=false

    case "$VERSION_CODENAME" in
        noble|plucky|oracular)
            # Ubuntu 24.04+ - use latest TSDuck
            TSDUCK_VERSION="3.43-4524"
            UBUNTU_TAG="ubuntu24"
            ;;
        jammy)
            # Ubuntu 22.04 - download ubuntu22 package from GitHub (ubuntu23 has incompatible deps)
            TSDUCK_VERSION="3.33-3139"
            UBUNTU_TAG="ubuntu22"
            DOWNLOAD_ONLY=true
            ;;
        focal)
            # Ubuntu 20.04 - use version that supports focal
            TSDUCK_VERSION="3.26-2349"
            UBUNTU_TAG="ubuntu20"
            ;;
        *)
            # Unknown - try ubuntu24 package
            log_warn "Unknown Ubuntu version, trying ubuntu24 package"
            TSDUCK_VERSION="3.43-4524"
            UBUNTU_TAG="ubuntu24"
            ;;
    esac

    local PACKAGE_NAME="tsduck_${TSDUCK_VERSION}.${UBUNTU_TAG}_${ARCH}.deb"
    log_info "Looking for TSDuck package: $PACKAGE_NAME"

    local DEB_FILE=""
    local FOUND_LOCAL=false

    # STEP 1: Check for local package matching the OS version (preferred method)
    # Skip if DOWNLOAD_ONLY is set (e.g., Ubuntu 22 where we don't have local package)
    if [[ "$DOWNLOAD_ONLY" = false ]] && [[ -d "$INSTALL_DIR/packages" ]]; then
        # First try exact match for this Ubuntu version
        local LOCAL_DEB=$(find "$INSTALL_DIR/packages" -name "tsduck*${UBUNTU_TAG}*${ARCH}.deb" 2>/dev/null | head -1)
        if [[ -f "$LOCAL_DEB" ]]; then
            log_info "Found local TSDuck package: $(basename "$LOCAL_DEB")"
            DEB_FILE="$LOCAL_DEB"
            FOUND_LOCAL=true
        fi
    fi

    # STEP 2: If no local package, try to download from GitHub releases
    if [[ "$FOUND_LOCAL" = false ]]; then
        log_info "No local package found, attempting download..."

        local GITHUB_URL="https://github.com/tsduck/tsduck/releases/download/v${TSDUCK_VERSION}/${PACKAGE_NAME}"

        DEB_FILE="/tmp/tsduck.deb"
        rm -f "$DEB_FILE"

        log_info "Downloading TSDuck v${TSDUCK_VERSION} from GitHub..."

        # Try download (with retries)
        local DOWNLOAD_SUCCESS=false
        for attempt in 1 2 3; do
            if command -v wget &> /dev/null; then
                if wget --no-check-certificate -q -O "$DEB_FILE" "$GITHUB_URL" 2>/dev/null; then
                    DOWNLOAD_SUCCESS=true
                    break
                fi
            else
                if curl -k -L -f -o "$DEB_FILE" "$GITHUB_URL" 2>/dev/null; then
                    DOWNLOAD_SUCCESS=true
                    break
                fi
            fi
            log_warn "Download attempt $attempt failed, retrying..."
            sleep 2
        done

        if [[ "$DOWNLOAD_SUCCESS" = false ]] || [[ ! -f "$DEB_FILE" ]] || [[ ! -s "$DEB_FILE" ]]; then
            log_error "Failed to download TSDuck package"
            log_error ""
            log_error "Please download TSDuck manually and place it in packages/ directory:"
            log_error "  1. Download from: https://github.com/tsduck/tsduck/releases/download/v${TSDUCK_VERSION}/${PACKAGE_NAME}"
            log_error "  2. Place in: $INSTALL_DIR/packages/"
            log_error "  3. Re-run installer"
            log_error ""
            return 1
        fi

        # Verify it's actually a .deb file
        local FILE_TYPE=$(file "$DEB_FILE" 2>/dev/null || echo "unknown")
        if ! echo "$FILE_TYPE" | grep -qi "debian\|archive"; then
            log_error "Downloaded file is not a valid Debian package"
            rm -f "$DEB_FILE"
            return 1
        fi

        log_info "Download successful ($(du -h "$DEB_FILE" | cut -f1))"
    fi

    # STEP 3: Install the package
    log_info "Installing TSDuck runtime dependencies..."
    apt-get install -y libcurl4 libpcsclite1 libedit2 || true

    log_info "Installing TSDuck package..."
    if dpkg -i "$DEB_FILE"; then
        log_info "TSDuck package installed"
    else
        log_warn "dpkg install had issues, attempting to fix dependencies..."
    fi

    # Fix any missing dependencies
    apt-get install -f -y

    # Cleanup temp file (but not local package)
    if [[ "$FOUND_LOCAL" = false ]] && [[ -f "/tmp/tsduck.deb" ]]; then
        rm -f "/tmp/tsduck.deb"
    fi

    # Verify installation
    if command -v tsp &> /dev/null; then
        log_info "TSDuck installed successfully: $(tsp --version 2>&1 | head -1)"
    else
        log_warn "TSDuck installation may have failed."
        log_warn "Stream scanning will use ffprobe as fallback."
        return 1
    fi
}

# Install librist from local source (for ristreceiver)
install_librist() {
    log_step "Installing librist (RIST library)..."

    # Check if already installed
    if command -v ristreceiver &> /dev/null || [[ -f /usr/local/bin/ristreceiver ]]; then
        log_info "librist already installed"
        return 0
    fi

    cd /tmp

    # Clean up any existing librist build directory
    if [[ -d "librist-build" ]]; then
        log_info "Removing existing librist build directory..."
        rm -rf librist-build
    fi

    # Check for local source first, otherwise download
    if [[ -d "$INSTALL_DIR/librist-master" ]]; then
        log_info "Using local librist source..."
        cp -r "$INSTALL_DIR/librist-master" librist-build
    else
        log_info "Downloading librist from code.videolan.org..."
        if ! git clone --depth 1 https://code.videolan.org/rist/librist.git librist-build; then
            log_warn "Failed to download librist source"
            log_warn "RIST support will not be available"
            return 1
        fi
    fi

    cd librist-build

    # Clean any previous build artifacts
    rm -rf build

    log_info "Building librist with meson/ninja..."

    # Configure with meson
    if ! meson setup build; then
        log_warn "Meson setup failed"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Build
    cd build
    if ! ninja; then
        log_warn "Ninja build failed"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Install
    if ! ninja install; then
        log_warn "Ninja install failed"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Update library cache
    ldconfig

    # Cleanup
    cd /tmp
    rm -rf librist-build

    # Verify installation
    if command -v ristreceiver &> /dev/null; then
        log_info "librist installed successfully"
    elif [[ -f /usr/local/bin/ristreceiver ]]; then
        log_info "librist installed to /usr/local/bin"
    else
        log_warn "ristreceiver not found in PATH after install"
        log_warn "RIST support may not work"
    fi
}

# Install GStreamer from source (for mpegtsmux bitrate property support)
install_gstreamer() {
    log_step "Installing GStreamer ${GSTREAMER_VERSION} from source..."

    # Check if already installed
    if command -v gst-launch-1.0 &> /dev/null; then
        local CURRENT_VERSION=$(gst-launch-1.0 --version 2>&1 | grep -oP 'GStreamer \K[0-9.]+' | head -1)
        if [[ "$CURRENT_VERSION" == "$GSTREAMER_VERSION" ]]; then
            log_info "GStreamer ${GSTREAMER_VERSION} already installed"
            return 0
        else
            log_info "Current GStreamer version: ${CURRENT_VERSION}, upgrading to ${GSTREAMER_VERSION}"
        fi
    fi

    local BUILD_DIR="/tmp/gstreamer-build"
    local JOBS=$(nproc)

    # Install build dependencies
    log_info "Installing GStreamer build dependencies..."

    # Update CA certificates first to fix SSL issues
    apt-get install -y ca-certificates
    update-ca-certificates

    apt-get install -y \
        build-essential \
        ninja-build \
        pkg-config \
        flex \
        bison \
        python3 \
        python3-pip \
        python3-gi \
        python3-certifi \
        libglib2.0-dev \
        libgudev-1.0-dev \
        liborc-0.4-dev \
        libpango1.0-dev \
        libcairo2-dev \
        libasound2-dev \
        libpulse-dev \
        libx264-dev \
        libx265-dev \
        libvpx-dev \
        libopus-dev \
        libmp3lame-dev \
        libfaad-dev \
        libvorbis-dev \
        libtheora-dev \
        libflac-dev \
        libspeex-dev \
        libwebp-dev \
        libjpeg-dev \
        libpng-dev \
        libsoup2.4-dev \
        libssl-dev \
        libsrtp2-dev \
        libnice-dev \
        libtag1-dev \
        libdv4-dev \
        libmpeg2-4-dev \
        libv4l-dev \
        libxv-dev \
        libxt-dev \
        libxext-dev \
        libgl-dev \
        libegl-dev \
        libdrm-dev \
        libgbm-dev \
        wayland-protocols \
        libwayland-dev \
        libgtk-3-dev \
        libcurl4-openssl-dev \
        libjson-glib-dev \
        libsbc-dev \
        libopencore-amrnb-dev \
        libopencore-amrwb-dev \
        libtwolame-dev \
        libwavpack-dev \
        libbs2b-dev \
        libsndfile1-dev \
        libass-dev \
        libzbar-dev \
        libchromaprint-dev \
        librtmp-dev \
        nasm \
        yasm \
        git \
        cmake || true

    # Install FFmpeg dev libraries for gst-libav plugin
    apt-get install -y \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libavfilter-dev \
        libswresample-dev \
        libswscale-dev || true

    # Install graphene library (prevents meson from downloading it)
    apt-get install -y libgraphene-1.0-dev 2>/dev/null || true

    # Optional packages that may not be available on all systems
    apt-get install -y libfaac-dev libusrsctp-dev libwebrtc-audio-processing-dev \
        liba52-0.7.4-dev libcdio-dev libdvdread-dev libdvdnav-dev \
        libraw1394-dev libavc1394-dev libiec61883-dev libldac-dev libfdk-aac-dev 2>/dev/null || true

    # Install newer Meson via pip (Ubuntu 20.04's meson is too old for GStreamer 1.26)
    log_info "Installing Meson build system via pip..."
    # Try standard pip upgrade first, then with --break-system-packages for newer systems
    pip3 install --upgrade meson || pip3 install --break-system-packages --upgrade meson
    # Ensure pip-installed meson is in PATH (installed to /usr/local/bin by pip as root)
    export PATH="/usr/local/bin:$PATH"
    hash -r  # Clear bash command cache
    log_info "Using Meson version: $(meson --version)"

    # Create build directory
    log_info "Setting up build directory..."
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    # Download GStreamer monorepo
    log_info "Downloading GStreamer ${GSTREAMER_VERSION}..."
    if ! git clone --depth 1 --branch ${GSTREAMER_VERSION} \
            https://gitlab.freedesktop.org/gstreamer/gstreamer.git; then
        log_error "Failed to download GStreamer source"
        rm -rf "${BUILD_DIR}"
        return 1
    fi
    cd gstreamer

    # Configure with meson
    log_info "Configuring build with Meson..."
    meson setup builddir \
        --prefix=/usr/local \
        --buildtype=release \
        --strip \
        -Dgpl=enabled \
        -Dugly=enabled \
        -Dbad=enabled \
        -Dlibav=enabled \
        -Ddevtools=disabled \
        -Ddoc=disabled \
        -Dexamples=disabled \
        -Dtests=disabled \
        -Dintrospection=disabled \
        -Dnls=disabled \
        -Dqt5=disabled \
        -Dqt6=disabled \
        -Dpython=disabled \
        -Dvaapi=disabled \
        -Dges=disabled \
        -Drtsp_server=disabled \
        -Dgst-examples=disabled \
        -Dsharp=disabled

    # Build
    log_info "Building GStreamer (this may take a while)..."
    ninja -C builddir -j${JOBS}

    # Install
    log_info "Installing GStreamer..."
    ninja -C builddir install

    # Update library cache
    ldconfig

    # Update pkg-config path
    cat > /etc/profile.d/gstreamer.sh << 'GSTENV'
export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export PATH=/usr/local/bin:$PATH
export GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0
GSTENV

    # Source the environment for this session
    export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
    export LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
    export PATH=/usr/local/bin:$PATH
    export GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0

    # Cleanup
    cd /
    rm -rf "${BUILD_DIR}"

    # Verify installation
    if command -v /usr/local/bin/gst-launch-1.0 &> /dev/null; then
        log_info "GStreamer ${GSTREAMER_VERSION} installed successfully"
        /usr/local/bin/gst-launch-1.0 --version

        # Check for mpegtsmux bitrate property
        if /usr/local/bin/gst-inspect-1.0 mpegtsmux 2>/dev/null | grep -q "bitrate"; then
            log_info "mpegtsmux bitrate property available"
        else
            log_warn "mpegtsmux bitrate property not found (may still work)"
        fi
    else
        log_error "GStreamer installation failed"
        return 1
    fi
}

# Create system user
create_user() {
    log_step "Creating system user..."

    if id "$SERVICE_USER" &>/dev/null; then
        log_info "User $SERVICE_USER already exists"
    else
        useradd -r -s /bin/false -d "$INSTALL_DIR" -c "CariTranscoder Service" "$SERVICE_USER"
        log_info "Created user: $SERVICE_USER"
    fi

    # Add to video group for GPU access
    usermod -a -G video "$SERVICE_USER" 2>/dev/null || true
    usermod -a -G render "$SERVICE_USER" 2>/dev/null || true
}

# Create directories
create_directories() {
    log_step "Creating directories..."

    mkdir -p "$INSTALL_DIR"
    mkdir -p "$CONFIG_DIR"/{inputs,transcoders,muxers,outputs,ssl}
    mkdir -p "$WEB_DIR"
    mkdir -p "$LOG_DIR"
    mkdir -p "$RUN_DIR"
    mkdir -p "$DATA_DIR"

    # Set permissions
    chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$LOG_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$RUN_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"

    # Config directory permissions:
    # - Main config dir needs www-data group access so web can traverse into subdirs
    # - Subdirs (inputs, transcoders, muxers, outputs) need www-data write access for web GUI
    # - ssl dir should be secure (no web access)
    chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR"
    chmod 750 "$CONFIG_DIR"

    # Web-writable config subdirectories (for web GUI to create/edit configs)
    for subdir in inputs transcoders muxers outputs; do
        chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/$subdir"
        chmod 775 "$CONFIG_DIR/$subdir"
    done

    # Secure ssl directory (no web access) - only service user can access
    chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/ssl"
    chmod 700 "$CONFIG_DIR/ssl"

    log_info "Directories created"
}

# Download repository (using tarball to avoid git ownership issues)
download_repo() {
    log_step "Downloading CariTranscoder..."

    local TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    # Download as tarball (no git required, avoids ownership issues)
    log_info "Fetching from branch: $BRANCH"
    local TARBALL_URL="${REPO_URL}/archive/refs/heads/${BRANCH}.tar.gz"
    log_info "Downloading from: $TARBALL_URL"

    local DOWNLOAD_OK=false

    # Try curl first (better redirect handling), then wget
    for attempt in 1 2 3; do
        if command -v curl &> /dev/null; then
            log_info "Download attempt $attempt using curl..."
            if curl -k -L -f --connect-timeout 30 --max-time 300 -o repo.tar.gz "$TARBALL_URL" 2>&1; then
                DOWNLOAD_OK=true
                break
            fi
        elif command -v wget &> /dev/null; then
            log_info "Download attempt $attempt using wget..."
            if wget --no-check-certificate --timeout=30 -q -O repo.tar.gz "$TARBALL_URL" 2>&1; then
                DOWNLOAD_OK=true
                break
            fi
        fi
        log_warn "Attempt $attempt failed, retrying in 3 seconds..."
        sleep 3
    done

    if [[ "$DOWNLOAD_OK" = false ]]; then
        log_error "Download failed after 3 attempts"
        log_error "URL: $TARBALL_URL"
        log_error "Please check network connectivity to github.com"
        rm -rf "$TEMP_DIR"
        exit 1
    fi

    if [[ ! -f repo.tar.gz || ! -s repo.tar.gz ]]; then
        log_error "Failed to download repository"
        rm -rf "$TEMP_DIR"
        exit 1
    fi

    # Extract
    tar -xzf repo.tar.gz
    local EXTRACTED_DIR=$(ls -d Caricoder2-* 2>/dev/null | head -1)

    if [[ -z "$EXTRACTED_DIR" || ! -d "$EXTRACTED_DIR" ]]; then
        log_error "Failed to extract repository"
        rm -rf "$TEMP_DIR"
        exit 1
    fi

    # Clear and copy to install dir
    rm -rf "$INSTALL_DIR"/*
    cp -r "$EXTRACTED_DIR"/* "$INSTALL_DIR/"

    # Cleanup temp
    rm -rf "$TEMP_DIR"

    chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
    log_info "Repository downloaded to $INSTALL_DIR"
}

# Build applications
build_apps() {
    log_step "Building CariTranscoder applications..."

    cd "$INSTALL_DIR"

    # Clean previous build
    make clean 2>/dev/null || true

    # Build
    make all

    log_info "Build completed successfully"
}

# Build tools (udp_input, etc.)
build_tools() {
    log_step "Building CariTranscoder tools..."

    # Build common library first (required by src/* applications)
    if [[ -d "$INSTALL_DIR/src/common" ]]; then
        cd "$INSTALL_DIR/src/common"
        log_info "Building common library..."
        make clean 2>/dev/null || true
        if make; then
            log_info "Common library built successfully"
        else
            log_warn "Failed to build common library"
        fi
    fi

    # Build udp_input
    if [[ -d "$INSTALL_DIR/tools/udp_input" ]]; then
        cd "$INSTALL_DIR/tools/udp_input"
        log_info "Building udp_input..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "udp_input installed to /usr/local/bin/"
        else
            log_warn "Failed to build udp_input"
        fi
    fi

    # Build player_preview
    if [[ -d "$INSTALL_DIR/tools/player_preview" ]]; then
        cd "$INSTALL_DIR/tools/player_preview"
        log_info "Building player_preview..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "player_preview installed to /usr/local/bin/"
        else
            log_warn "Failed to build player_preview"
        fi
    fi

    # Build cari-avsync
    if [[ -d "$INSTALL_DIR/tools/cari-avsync" ]]; then
        cd "$INSTALL_DIR/tools/cari-avsync"
        log_info "Building cari-avsync..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-avsync installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-avsync"
        fi
    fi

    # Build srt_input
    if [[ -d "$INSTALL_DIR/tools/srt_input" ]]; then
        cd "$INSTALL_DIR/tools/srt_input"
        log_info "Building srt_input..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "srt_input installed to /usr/local/bin/"
        else
            log_warn "Failed to build srt_input"
        fi
    fi

    # Build hls_input
    if [[ -d "$INSTALL_DIR/tools/hls_input" ]]; then
        cd "$INSTALL_DIR/tools/hls_input"
        log_info "Building hls_input..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "hls_input installed to /usr/local/bin/"
        else
            log_warn "Failed to build hls_input"
        fi
    fi

    # Build http_input
    if [[ -d "$INSTALL_DIR/tools/http_input" ]]; then
        cd "$INSTALL_DIR/tools/http_input"
        log_info "Building http_input..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "http_input installed to /usr/local/bin/"
        else
            log_warn "Failed to build http_input"
        fi
    fi

    # Build rist_input
    if [[ -d "$INSTALL_DIR/tools/rist_input" ]]; then
        cd "$INSTALL_DIR/tools/rist_input"
        log_info "Building rist_input..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "rist_input installed to /usr/local/bin/"
        else
            log_warn "Failed to build rist_input"
        fi
    fi

    # Build cari-transcoder (GStreamer-based transcoder)
    if [[ -d "$INSTALL_DIR/src/cari-transcoder" ]]; then
        cd "$INSTALL_DIR/src/cari-transcoder"
        log_info "Building cari-transcoder..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-transcoder installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-transcoder (GStreamer dev packages may be missing)"
        fi
    fi

    # Build cari-transcoder-abr (Multi-bitrate ABR transcoder)
    if [[ -d "$INSTALL_DIR/src/cari-transcoder-abr" ]]; then
        cd "$INSTALL_DIR/src/cari-transcoder-abr"
        log_info "Building cari-transcoder-abr..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-transcoder-abr installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-transcoder-abr (GStreamer dev packages may be missing)"
        fi
    fi

    # Build cari-mux (MPEG-TS multiplexer)
    if [[ -d "$INSTALL_DIR/src/cari-mux" ]]; then
        cd "$INSTALL_DIR/src/cari-mux"
        log_info "Building cari-mux..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-mux installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-mux (GStreamer dev packages may be missing)"
        fi
    fi

    # Build cari-output (Output with SRT one-to-many support)
    if [[ -d "$INSTALL_DIR/src/cari-output" ]]; then
        cd "$INSTALL_DIR/src/cari-output"
        log_info "Building cari-output..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-output installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-output (SRT dev packages may be missing)"
        fi
    fi

    # Build http_ts_server (HTTP MPEG-TS pull output server)
    if [[ -d "$INSTALL_DIR/tools/http_ts_server" ]]; then
        cd "$INSTALL_DIR/tools/http_ts_server"
        log_info "Building http_ts_server..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "http_ts_server installed to /usr/local/bin/"
        else
            log_warn "Failed to build http_ts_server (libmicrohttpd may be missing)"
        fi
    fi

    # Build hls_output (HLS output server with client tracking)
    if [[ -d "$INSTALL_DIR/tools/hls_output" ]]; then
        cd "$INSTALL_DIR/tools/hls_output"
        log_info "Building hls_output..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "hls_output installed to /usr/local/bin/"
        else
            log_warn "Failed to build hls_output (libmicrohttpd may be missing)"
        fi
    fi

    log_info "Tools build completed"
}

# Install binaries
install_binaries() {
    log_step "Installing binaries..."

    cd "$INSTALL_DIR"

    # Install to /usr/local/bin
    for app in cari-input cari-transcoder cari-mux cari-output cari-stats cari-ha; do
        if [[ -f "src/$app/$app" ]]; then
            install -m 755 "src/$app/$app" /usr/local/bin/
            log_info "Installed: $app"
        fi
    done
}

# Install configuration files (clean install - removes old configs)
install_config() {
    log_step "Installing configuration files (clean install)..."

    cd "$INSTALL_DIR"

    # Clean install: remove old config files
    log_info "Removing old configuration files..."
    rm -f "$CONFIG_DIR/caritrans.conf" 2>/dev/null || true
    rm -f "$CONFIG_DIR/users.conf" 2>/dev/null || true
    rm -rf "$CONFIG_DIR/inputs" 2>/dev/null || true
    rm -rf "$CONFIG_DIR/transcoders" 2>/dev/null || true
    rm -rf "$CONFIG_DIR/muxers" 2>/dev/null || true
    rm -rf "$CONFIG_DIR/outputs" 2>/dev/null || true

    # Recreate config subdirectories
    mkdir -p "$CONFIG_DIR"/{inputs,transcoders,muxers,outputs,ssl}

    # Main config - fresh install
    cp config/caritrans.conf "$CONFIG_DIR/"
    # Update paths in config
    sed -i "s|config_dir = .*|config_dir = $CONFIG_DIR|" "$CONFIG_DIR/caritrans.conf"
    sed -i "s|run_dir = .*|run_dir = $RUN_DIR|" "$CONFIG_DIR/caritrans.conf"
    sed -i "s|log_dir = .*|log_dir = $LOG_DIR|" "$CONFIG_DIR/caritrans.conf"
    sed -i "s|data_dir = .*|data_dir = $DATA_DIR|" "$CONFIG_DIR/caritrans.conf"
    log_info "Created: caritrans.conf"

    # Users config - fresh install
    cp config/users.conf "$CONFIG_DIR/"
    log_info "Created: users.conf"

    # Set permissions
    chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/users.conf"
    chmod 640 "$CONFIG_DIR/users.conf"
    chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/caritrans.conf"

    # Config subdirectories permissions (www-data writable for web GUI)
    for subdir in inputs transcoders muxers outputs; do
        chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/$subdir"
        chmod 775 "$CONFIG_DIR/$subdir"
    done

    # Secure ssl directory
    chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/ssl"
    chmod 700 "$CONFIG_DIR/ssl"

    log_info "Configuration files installed"
}

# Install web interface (clean install - removes old files)
install_web() {
    log_step "Installing web interface (clean install)..."

    cd "$INSTALL_DIR"

    # Clean install: remove old web files
    log_info "Removing old web files..."
    rm -rf "$WEB_DIR"/* 2>/dev/null || true

    # Copy fresh web files
    cp -r web/* "$WEB_DIR/"

    # Update config path in PHP
    sed -i "s|define('CONFIG_DIR', '.*')|define('CONFIG_DIR', '$CONFIG_DIR')|" "$WEB_DIR/includes/config.php" 2>/dev/null || true

    # Set permissions
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    find "$WEB_DIR" -type d -exec chmod 755 {} \;
    find "$WEB_DIR" -type f -exec chmod 644 {} \;

    # Create preview directory for HLS player
    mkdir -p "$WEB_DIR/public/preview"
    chown "$WEB_USER:$WEB_USER" "$WEB_DIR/public/preview"
    chmod 755 "$WEB_DIR/public/preview"

    log_info "Web interface installed to $WEB_DIR"
}

# Install systemd services (clean install - stops and removes old services)
install_services() {
    log_step "Installing systemd services (clean install)..."

    cd "$INSTALL_DIR"

    # Clean install: stop and remove old services
    log_info "Stopping and removing old services..."
    for svc in /etc/systemd/system/cari-*.service; do
        if [[ -f "$svc" ]]; then
            svc_name=$(basename "$svc")
            systemctl stop "$svc_name" 2>/dev/null || true
            systemctl disable "$svc_name" 2>/dev/null || true
            rm -f "$svc"
        fi
    done

    # Copy fresh service files
    cp systemd/*.service /etc/systemd/system/

    # Update paths in service files
    for svc in /etc/systemd/system/cari-*.service; do
        sed -i "s|/usr/local/bin|/usr/local/bin|g" "$svc"
        sed -i "s|CONFIG_DIR=/etc/caritrans|CONFIG_DIR=$CONFIG_DIR|g" "$svc"
        sed -i "s|RUN_DIR=/run/caritrans|RUN_DIR=$RUN_DIR|g" "$svc"
        sed -i "s|LOG_DIR=/var/log/caritrans|LOG_DIR=$LOG_DIR|g" "$svc"
    done

    # Reload systemd
    systemctl daemon-reload

    log_info "Systemd services installed"
}

# Install and configure the privileged API service
install_api() {
    log_step "Installing CariTranscoder API service..."

    # Create API directory
    mkdir -p "$INSTALL_DIR/api"

    # Copy API files
    if [[ -d "$INSTALL_DIR/api" ]] && [[ -f "$INSTALL_DIR/api/main.py" ]]; then
        log_info "API files already in place"
    else
        # Copy from source if available
        if [[ -f "/tmp/caritrans_src/api/main.py" ]]; then
            cp -r /tmp/caritrans_src/api/* "$INSTALL_DIR/api/"
        fi
    fi

    # Install Python dependencies
    log_info "Installing Python dependencies for API..."
    pip3 install --break-system-packages fastapi uvicorn pydantic 2>/dev/null || \
    pip3 install fastapi uvicorn pydantic

    # Copy systemd service for API
    cp "$INSTALL_DIR/systemd/cari-api.service" /etc/systemd/system/

    # Reload systemd and enable service
    systemctl daemon-reload
    systemctl enable cari-api
    systemctl start cari-api || systemctl restart cari-api

    # Verify API is running
    sleep 2
    if systemctl is-active --quiet cari-api; then
        log_info "CariTranscoder API service is running"
    else
        log_warn "CariTranscoder API service may not be running properly"
        log_warn "Check with: journalctl -u cari-api -f"
    fi

    # Remove old sudoers file if it exists (no longer needed with API)
    if [[ -f /etc/sudoers.d/caritrans ]]; then
        rm -f /etc/sudoers.d/caritrans
        log_info "Removed old sudoers file (API replaces sudo approach)"
    fi

    log_info "API service installed"
}

# Install A/V Sync Monitor service
install_avsync_service() {
    log_step "Installing A/V Sync Monitor service..."

    # Copy systemd service
    if [[ -f "$INSTALL_DIR/systemd/cari-avsync.service" ]]; then
        cp "$INSTALL_DIR/systemd/cari-avsync.service" /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable cari-avsync
        systemctl start cari-avsync || systemctl restart cari-avsync

        sleep 2
        if systemctl is-active --quiet cari-avsync; then
            log_info "A/V Sync Monitor service is running"
        else
            log_warn "A/V Sync Monitor service may not be running properly"
            log_warn "Check with: journalctl -u cari-avsync -f"
        fi
    else
        log_warn "cari-avsync.service not found, skipping"
    fi

    log_info "A/V Sync Monitor service installed"
}

# Configure Nginx (optional)
configure_nginx() {
    log_step "Configuring Nginx..."

    if ! command -v nginx &> /dev/null; then
        log_warn "Nginx not installed, skipping configuration"
        return
    fi

    # Detect PHP-FPM socket path
    local PHP_FPM_SOCK=""
    for ver in 8.4 8.3 8.2 8.1 8.0 7.4; do
        if [[ -S "/var/run/php/php${ver}-fpm.sock" ]]; then
            PHP_FPM_SOCK="/var/run/php/php${ver}-fpm.sock"
            break
        fi
    done
    if [[ -z "$PHP_FPM_SOCK" ]]; then
        PHP_FPM_SOCK=$(find /var/run/php -name "php*-fpm.sock" 2>/dev/null | head -1)
        [[ -z "$PHP_FPM_SOCK" ]] && PHP_FPM_SOCK="/var/run/php/php-fpm.sock"
    fi

    log_info "Using PHP-FPM socket: $PHP_FPM_SOCK"

    # Create Nginx config
    cat > /etc/nginx/sites-available/caritrans << NGINX
server {
    listen 8080;
    server_name _;

    root /var/www/caritrans/public;
    index index.php index.html;

    location / {
        try_files \$uri \$uri/ /index.php?\$query_string;
    }

    location ~ \\.php\$ {
        include snippets/fastcgi-php.conf;
        fastcgi_pass unix:$PHP_FPM_SOCK;
        fastcgi_param SCRIPT_FILENAME \$document_root\$fastcgi_script_name;
        include fastcgi_params;
    }

    location ~ /\\.(ht|git) {
        deny all;
    }

    # HLS preview streams - serve directly without PHP auth
    location /preview/ {
        alias /var/www/caritrans/public/preview/;
        add_header Access-Control-Allow-Origin *;
        add_header Cache-Control "no-cache, no-store, must-revalidate";
        types {
            application/vnd.apple.mpegurl m3u8;
            video/mp2t ts;
        }
    }

    # WebSocket proxy for stats
    location /ws {
        proxy_pass http://127.0.0.1:8081;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host \$host;
    }
}
NGINX

    # Remove default site if it conflicts
    rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true

    # Enable site
    ln -sf /etc/nginx/sites-available/caritrans /etc/nginx/sites-enabled/

    # Test config
    if nginx -t; then
        log_info "Nginx configuration valid"
    else
        log_error "Nginx configuration invalid"
        return 1
    fi

    log_info "Nginx configured on port 8080"
}

# Start web services
start_services() {
    log_step "Starting web services..."

    # Detect and start PHP-FPM
    local PHP_FPM_SERVICE=""
    for ver in 8.4 8.3 8.2 8.1 8.0 7.4; do
        if systemctl list-unit-files | grep -q "php${ver}-fpm"; then
            PHP_FPM_SERVICE="php${ver}-fpm"
            break
        fi
    done

    if [[ -z "$PHP_FPM_SERVICE" ]]; then
        # Try generic php-fpm
        if systemctl list-unit-files | grep -q "php-fpm"; then
            PHP_FPM_SERVICE="php-fpm"
        fi
    fi

    if [[ -n "$PHP_FPM_SERVICE" ]]; then
        log_info "Starting $PHP_FPM_SERVICE..."
        systemctl enable "$PHP_FPM_SERVICE" 2>/dev/null || true
        systemctl start "$PHP_FPM_SERVICE" || systemctl restart "$PHP_FPM_SERVICE"
    else
        log_warn "PHP-FPM service not found"
    fi

    # Start Nginx (always restart to pick up new config)
    if command -v nginx &> /dev/null; then
        log_info "Restarting nginx..."
        systemctl enable nginx 2>/dev/null || true
        systemctl restart nginx
    fi

    # Verify services are running
    sleep 2
    if systemctl is-active --quiet nginx; then
        log_info "Nginx is running"
    else
        log_warn "Nginx may not be running properly"
    fi

    if [[ -n "$PHP_FPM_SERVICE" ]] && systemctl is-active --quiet "$PHP_FPM_SERVICE"; then
        log_info "$PHP_FPM_SERVICE is running"
    else
        log_warn "PHP-FPM may not be running properly"
    fi
}

# Create tmpfiles.d entry for /run directory
create_tmpfiles() {
    log_step "Creating tmpfiles.d configuration..."

    cat > /etc/tmpfiles.d/caritrans.conf << TMPFILES
d $RUN_DIR 0755 $SERVICE_USER $SERVICE_USER -
TMPFILES

    # Create directory now
    systemd-tmpfiles --create

    log_info "tmpfiles.d configured"
}

# Print completion message
print_completion() {
    # Get server IP
    local SERVER_IP=$(hostname -I | awk '{print $1}')

    echo ""
    echo -e "${GREEN}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║       CariTranscoder Installation Complete!            ║${NC}"
    echo -e "${GREEN}╚════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${BLUE}Installation Summary:${NC}"
    echo "  Binaries:    /usr/local/bin/cari-*"
    echo "  Config:      $CONFIG_DIR"
    echo "  Web:         $WEB_DIR"
    echo "  Logs:        $LOG_DIR"
    echo "  Data:        $DATA_DIR"
    echo ""
    echo -e "${BLUE}Services Status:${NC}"
    if systemctl is-active --quiet nginx; then
        echo -e "  Nginx:       ${GREEN}Running${NC}"
    else
        echo -e "  Nginx:       ${RED}Not Running${NC}"
    fi
    for ver in 8.4 8.3 8.2 8.1 8.0 7.4; do
        if systemctl is-active --quiet "php${ver}-fpm" 2>/dev/null; then
            echo -e "  PHP-FPM:     ${GREEN}Running (PHP $ver)${NC}"
            break
        fi
    done
    if systemctl is-active --quiet cari-api; then
        echo -e "  Cari-API:    ${GREEN}Running${NC}"
    else
        echo -e "  Cari-API:    ${RED}Not Running${NC}"
    fi
    if command -v tsp &> /dev/null; then
        echo -e "  TSDuck:      ${GREEN}Installed${NC}"
    else
        echo -e "  TSDuck:      ${YELLOW}Not Installed${NC}"
    fi
    if command -v ffprobe &> /dev/null; then
        echo -e "  FFmpeg:      ${GREEN}Installed${NC}"
    else
        echo -e "  FFmpeg:      ${YELLOW}Not Installed${NC}"
    fi
    if command -v srt-live-transmit &> /dev/null; then
        echo -e "  SRT Tools:   ${GREEN}Installed${NC}"
    else
        echo -e "  SRT Tools:   ${YELLOW}Not Installed${NC}"
    fi
    if command -v ristreceiver &> /dev/null || [[ -f /usr/local/bin/ristreceiver ]]; then
        echo -e "  librist:     ${GREEN}Installed${NC}"
    else
        echo -e "  librist:     ${YELLOW}Not Installed${NC}"
    fi
    if command -v gst-launch-1.0 &> /dev/null; then
        local GST_VER=$(gst-launch-1.0 --version 2>&1 | grep -oP 'GStreamer \K[0-9.]+' | head -1)
        echo -e "  GStreamer:   ${GREEN}${GST_VER}${NC}"
        if gst-inspect-1.0 mpegtsmux 2>/dev/null | grep -q "bitrate"; then
            echo -e "  mpegtsmux:   ${GREEN}CBR bitrate available${NC}"
        else
            echo -e "  mpegtsmux:   ${YELLOW}CBR bitrate not available${NC}"
        fi
    else
        echo -e "  GStreamer:   ${YELLOW}Not Installed${NC}"
    fi
    echo ""
    echo -e "${BLUE}Web Interface:${NC}"
    echo -e "  URL:         ${GREEN}http://${SERVER_IP}:8080${NC}"
    echo "  Username:    admin"
    echo "  Password:    admin"
    echo ""
    echo -e "${BLUE}Quick Commands:${NC}"
    echo "  Start transcoder:  systemctl start cari-input@input-001"
    echo "  View logs:         journalctl -u cari-input@input-001 -f"
    echo "  Restart nginx:     systemctl restart nginx"
    echo ""
    echo -e "${BLUE}Update CariTranscoder:${NC}"
    echo "  curl -sSL \"https://raw.githubusercontent.com/caritechsolutions/Caricoder2/$BRANCH/scripts/update.sh?\$(date +%s)\" | sudo bash"
    echo ""
    echo -e "${YELLOW}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${YELLOW}║  IMPORTANT: Change the default admin password now!     ║${NC}"
    echo -e "${YELLOW}╚════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

# Main installation flow
main() {
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║   CariTranscoder Installation Script   ║${NC}"
    echo -e "${BLUE}║            Version $SCRIPT_VERSION                 ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    echo ""

    check_root
    check_os
    install_dependencies
    install_gstreamer
    create_user
    create_directories
    download_repo
    install_tsduck
    install_librist
    build_apps
    build_tools
    install_binaries
    install_config
    install_web
    install_services
    install_api
    install_avsync_service
    configure_nginx
    create_tmpfiles
    start_services
    print_completion
}

# Run main
main "$@"

#!/bin/bash
#
# GStreamer 1.26.1 Build Script for Caricoder
# This script builds and installs GStreamer from source
#

set -e

GSTREAMER_VERSION="1.26.1"
INSTALL_PREFIX="/usr/local"
BUILD_DIR="/tmp/gstreamer-build"
JOBS=$(nproc)

echo "=============================================="
echo "GStreamer ${GSTREAMER_VERSION} Build Script"
echo "=============================================="
echo "Install prefix: ${INSTALL_PREFIX}"
echo "Build jobs: ${JOBS}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo)"
    exit 1
fi

# Install build dependencies
echo "[1/6] Installing build dependencies..."
apt-get update
apt-get install -y \
    build-essential \
    ninja-build \
    pkg-config \
    flex \
    bison \
    python3 \
    python3-pip \
    python3-gi \
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
    libfaac-dev \
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
    libusrsctp-dev \
    libwebrtc-audio-processing-dev \
    libzbar-dev \
    libtag1-dev \
    libdv4-dev \
    libmpeg2-4-dev \
    liba52-0.7.4-dev \
    libcdio-dev \
    libdvdread-dev \
    libdvdnav-dev \
    libraw1394-dev \
    libavc1394-dev \
    libiec61883-dev \
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
    libldac-dev 2>/dev/null || true \
    libopencore-amrnb-dev \
    libopencore-amrwb-dev \
    libvo-amrwbenc-dev \
    libtwolame-dev \
    libwavpack-dev \
    libbs2b-dev \
    libsndfile1-dev \
    libass-dev \
    libzbar-dev \
    libchromaprint-dev \
    libfdk-aac-dev 2>/dev/null || true \
    librtmp-dev \
    nasm \
    yasm \
    git \
    cmake

# Install newer Meson via pip (Ubuntu 20.04's meson is too old for GStreamer 1.26)
echo "Installing Meson build system via pip..."
pip3 install --break-system-packages --upgrade meson 2>/dev/null || pip3 install --upgrade meson
# Ensure pip-installed meson is in PATH (installed to /usr/local/bin by pip as root)
export PATH="/usr/local/bin:$PATH"
hash -r  # Clear bash command cache
echo "Using Meson version: $(meson --version)"

# Create build directory
echo "[2/6] Setting up build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Download GStreamer monorepo
echo "[3/6] Downloading GStreamer ${GSTREAMER_VERSION}..."
if [ ! -d "gstreamer" ]; then
    git clone --depth 1 --branch ${GSTREAMER_VERSION} \
        https://gitlab.freedesktop.org/gstreamer/gstreamer.git
fi
cd gstreamer

# Configure with meson
echo "[4/6] Configuring build with Meson..."
meson setup builddir \
    --prefix=${INSTALL_PREFIX} \
    --buildtype=release \
    --strip \
    -Dgpl=enabled \
    -Dugly=enabled \
    -Dbad=enabled \
    -Dlibav=disabled \
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
echo "[5/6] Building GStreamer (this may take a while)..."
ninja -C builddir -j${JOBS}

# Install
echo "[6/6] Installing GStreamer..."
ninja -C builddir install

# Update library cache
ldconfig

# Update pkg-config path
echo "export PKG_CONFIG_PATH=${INSTALL_PREFIX}/lib/x86_64-linux-gnu/pkgconfig:\$PKG_CONFIG_PATH" > /etc/profile.d/gstreamer.sh
echo "export LD_LIBRARY_PATH=${INSTALL_PREFIX}/lib/x86_64-linux-gnu:\$LD_LIBRARY_PATH" >> /etc/profile.d/gstreamer.sh
echo "export PATH=${INSTALL_PREFIX}/bin:\$PATH" >> /etc/profile.d/gstreamer.sh
echo "export GST_PLUGIN_PATH=${INSTALL_PREFIX}/lib/x86_64-linux-gnu/gstreamer-1.0" >> /etc/profile.d/gstreamer.sh

# Source the environment
source /etc/profile.d/gstreamer.sh

# Verify installation
echo ""
echo "=============================================="
echo "Installation Complete!"
echo "=============================================="
echo ""
${INSTALL_PREFIX}/bin/gst-launch-1.0 --version
echo ""
echo "Checking mpegtsmux bitrate property..."
${INSTALL_PREFIX}/bin/gst-inspect-1.0 mpegtsmux | grep -i bitrate || echo "Note: bitrate property check"
echo ""
echo "Environment variables have been set in /etc/profile.d/gstreamer.sh"
echo "Run 'source /etc/profile.d/gstreamer.sh' or log out/in to use new GStreamer"
echo ""
echo "To rebuild cari-transcoder with new GStreamer:"
echo "  cd /path/to/Caricoder2/src/cari-transcoder"
echo "  make clean && make"
echo ""

# Cleanup option
read -p "Remove build directory (${BUILD_DIR})? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -rf "${BUILD_DIR}"
    echo "Build directory removed."
fi

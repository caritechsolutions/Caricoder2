#!/bin/bash
#
# CariTranscoder Update Script
# This script updates only the code files (API, web interface)
# It preserves: configs, services, and channel data
#
# Usage:
#   Interactive:  ./update.sh
#   Auto-confirm: ./update.sh -y
#   Via curl:     curl -sSL "https://raw.githubusercontent.com/.../update.sh?$(date +%s)" | sudo bash -s -- -y
#
# Copyright (c) 2024 CariTech Solutions

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
INSTALL_DIR="/opt/caritrans"
CONFIG_DIR="/etc/caritrans"
WEB_DIR="/var/www/caritrans"
SERVICE_USER="caritrans"
WEB_USER="www-data"
REPO_URL="https://github.com/caritechsolutions/Caricoder2"
# Updated: 2024-12-26
BRANCH="claude/av-sync-monitor-Y57VM"

# Parse arguments
AUTO_CONFIRM=false
while getopts "y" opt; do
    case $opt in
        y) AUTO_CONFIRM=true ;;
        *) ;;
    esac
done

# Logging functions
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

echo ""
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║     CariTranscoder Update Script       ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# Check root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

# Check installation exists
check_installation() {
    if [[ ! -d "$WEB_DIR" ]]; then
        log_error "CariTranscoder not found at $WEB_DIR"
        log_error "Please run the install script first."
        exit 1
    fi
}

# Show what will be updated
show_plan() {
    echo -e "${YELLOW}This script will update:${NC}"
    echo -e "  - /var/www/caritrans/* (web interface)"
    echo -e "  - /usr/local/bin/cari-* (binaries, if rebuilding)"
    echo ""
    echo -e "${GREEN}This script will NOT touch:${NC}"
    echo -e "  - /etc/caritrans/* (your configurations)"
    echo -e "  - /var/log/caritrans/* (log files)"
    echo -e "  - /var/lib/caritrans/* (data files)"
    echo -e "  - Running service states"
    echo ""
}

# Backup current code
backup_current() {
    log_step "Creating backup of current code..."

    BACKUP_DIR="/tmp/caritrans_backup_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$BACKUP_DIR"

    # Backup web files
    if [[ -d "$WEB_DIR" ]]; then
        cp -r "$WEB_DIR" "$BACKUP_DIR/web_backup"
    fi

    log_info "Backup created at: $BACKUP_DIR"
}

# Download latest code
download_latest() {
    log_step "Downloading latest code from repository..."

    TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    # Download the repository as a tarball (no git needed)
    local TARBALL_URL="${REPO_URL}/archive/refs/heads/${BRANCH}.tar.gz"
    log_info "Fetching from branch: $BRANCH"
    log_info "URL: $TARBALL_URL"

    local DOWNLOAD_OK=false
    if command -v wget &> /dev/null; then
        if wget --no-check-certificate -q -O repo.tar.gz "$TARBALL_URL"; then
            DOWNLOAD_OK=true
        fi
    else
        if curl -k -L -f -o repo.tar.gz "$TARBALL_URL" 2>/dev/null; then
            DOWNLOAD_OK=true
        fi
    fi

    if [[ "$DOWNLOAD_OK" = false ]] || [[ ! -f repo.tar.gz ]] || [[ ! -s repo.tar.gz ]]; then
        log_error "Failed to download repository"
        exit 1
    fi

    # Extract
    tar -xzf repo.tar.gz
    EXTRACTED_DIR=$(ls -d Caricoder2-* 2>/dev/null | head -1)

    if [[ -z "$EXTRACTED_DIR" || ! -d "$EXTRACTED_DIR" ]]; then
        log_error "Failed to extract repository"
        exit 1
    fi

    mv "$EXTRACTED_DIR" caritrans_latest

    log_info "Download complete"
}

# Update web interface
update_web() {
    log_step "Updating web interface..."

    # List of directories/files to update
    if [[ -d "$TEMP_DIR/caritrans_latest/web" ]]; then
        # Update all web files
        cp -r "$TEMP_DIR/caritrans_latest/web/"* "$WEB_DIR/"
        log_info "Updated web files"
    fi

    # Update packages directory (contains TSDuck .deb files)
    if [[ -d "$TEMP_DIR/caritrans_latest/packages" ]]; then
        mkdir -p "$INSTALL_DIR/packages"
        cp -r "$TEMP_DIR/caritrans_latest/packages/"* "$INSTALL_DIR/packages/" 2>/dev/null || true
        log_info "Updated packages directory"
    fi

    # Set permissions
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    find "$WEB_DIR" -type d -exec chmod 755 {} \;
    find "$WEB_DIR" -type f -exec chmod 644 {} \;

    log_info "Web interface updated"
}

# Update PHP API files (if any separate API)
update_php_api() {
    log_step "Updating PHP API files..."

    if [[ -d "$TEMP_DIR/caritrans_latest/web/api" ]]; then
        # Create api directory if it doesn't exist
        mkdir -p "$WEB_DIR/api"
        cp -r "$TEMP_DIR/caritrans_latest/web/api/"* "$WEB_DIR/api/"
        chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR/api"
        log_info "PHP API files updated"
    fi
}

# Install/update GStreamer plugins
update_gstreamer_plugins() {
    log_step "Updating GStreamer plugins..."

    apt-get update -qq

    # GStreamer libav (provides avdec_h264, avdec_ac3, avdec_eac3, avenc_* etc.)
    apt-get install -y gstreamer1.0-libav || true

    # Additional GStreamer codec plugins
    # Note: x264 encoder is already in gstreamer1.0-plugins-ugly
    # Note: AAC encoder is in gstreamer1.0-libav (avenc_aac)
    apt-get install -y gstreamer1.0-vaapi || true

    # GStreamer video processing plugins (for deinterlacing, scaling, etc.)
    apt-get install -y \
        libgstreamer-plugins-bad1.0-dev || true

    log_info "GStreamer plugins updated"
}

# Optionally rebuild C applications
rebuild_apps() {
    log_step "Checking if rebuild is needed..."

    # Check if user wants to rebuild
    if [[ "$AUTO_CONFIRM" = true ]]; then
        REBUILD="n"
    else
        if [[ -t 0 ]]; then
            read -p "Do you want to rebuild the C applications? (y/N): " REBUILD
        else
            REBUILD="n"
        fi
    fi

    if [[ "$REBUILD" =~ ^[Yy]$ ]]; then
        log_info "Rebuilding applications..."

        cd "$TEMP_DIR/caritrans_latest"

        # Build
        if make all; then
            # Install binaries
            for app in cari-input cari-transcoder cari-mux cari-output cari-stats cari-ha; do
                if [[ -f "src/$app/$app" ]]; then
                    install -m 755 "src/$app/$app" /usr/local/bin/
                    log_info "Updated: $app"
                fi
            done
            log_info "Build completed"
        else
            log_warn "Build failed, keeping existing binaries"
        fi
    else
        log_info "Skipping rebuild (using existing binaries)"
    fi
}

# Build and install tools (always runs)
build_tools() {
    log_step "Building tools..."

    # Build udp_input
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/udp_input" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/udp_input"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/player_preview" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/player_preview"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/cari-avsync" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/cari-avsync"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/srt_input" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/srt_input"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/hls_input" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/hls_input"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/http_input" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/http_input"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/tools/rist_input" ]]; then
        cd "$TEMP_DIR/caritrans_latest/tools/rist_input"
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
    if [[ -d "$TEMP_DIR/caritrans_latest/src/cari-transcoder" ]]; then
        cd "$TEMP_DIR/caritrans_latest/src/cari-transcoder"
        log_info "Building cari-transcoder..."
        make clean 2>/dev/null || true
        if make; then
            make install
            log_info "cari-transcoder installed to /usr/local/bin/"
        else
            log_warn "Failed to build cari-transcoder (GStreamer dev packages may be missing)"
        fi
    fi

    log_info "Tools build completed"
}

# Rebuild librist from local source (if needed)
rebuild_librist() {
    log_step "Checking librist installation..."

    # Check if librist source exists in the update
    if [[ ! -d "$TEMP_DIR/caritrans_latest/librist-master" ]]; then
        log_info "librist source not found in update, skipping"
        return 0
    fi

    # Check if ristreceiver needs rebuild
    local REBUILD_RIST="n"
    local NEEDS_REBUILD=false

    # Check if ristreceiver exists and supports stdout:// output (required for rist_input)
    if ! command -v ristreceiver &> /dev/null && [[ ! -f /usr/local/bin/ristreceiver ]]; then
        NEEDS_REBUILD=true
        log_info "ristreceiver not found, rebuild required"
    elif ! ristreceiver --help 2>&1 | grep -q "stdout://"; then
        NEEDS_REBUILD=true
        log_info "ristreceiver missing stdout:// output support, rebuild required"
    fi

    if [[ "$AUTO_CONFIRM" = false ]]; then
        if [[ -t 0 ]]; then
            if [[ "$NEEDS_REBUILD" = true ]]; then
                log_info "librist rebuild is recommended"
            fi
            read -p "Do you want to rebuild librist/ristreceiver? (y/N): " REBUILD_RIST
        fi
    else
        # In auto mode, rebuild if needed
        if [[ "$NEEDS_REBUILD" = true ]]; then
            REBUILD_RIST="y"
        fi
    fi

    if [[ ! "$REBUILD_RIST" =~ ^[Yy]$ ]]; then
        log_info "Skipping librist rebuild"
        return 0
    fi

    log_info "Rebuilding librist from local source..."

    cd /tmp

    # Clean up any existing librist build directory
    if [[ -d "librist-build" ]]; then
        rm -rf librist-build
    fi

    # Copy source to temp build directory
    cp -r "$TEMP_DIR/caritrans_latest/librist-master" librist-build

    cd librist-build

    # Clean any previous build artifacts
    rm -rf build

    # Configure with meson
    if ! meson setup build; then
        log_warn "Meson setup failed for librist"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Build
    cd build
    if ! ninja; then
        log_warn "Ninja build failed for librist"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Install
    if ! ninja install; then
        log_warn "Ninja install failed for librist"
        cd /tmp && rm -rf librist-build
        return 1
    fi

    # Update library cache
    ldconfig

    # Cleanup
    cd /tmp
    rm -rf librist-build

    # Verify installation
    if command -v ristreceiver &> /dev/null || [[ -f /usr/local/bin/ristreceiver ]]; then
        log_info "librist rebuilt and installed successfully"
    else
        log_warn "ristreceiver not found after rebuild"
    fi
}

# Update and restart the API service
update_api() {
    log_step "Updating CariTranscoder API service..."

    # Update API files
    if [[ -d "$TEMP_DIR/caritrans_latest/api" ]]; then
        mkdir -p "$INSTALL_DIR/api"
        cp -r "$TEMP_DIR/caritrans_latest/api/"* "$INSTALL_DIR/api/"
        log_info "Updated API files"
    fi

    # Update systemd service file
    if [[ -f "$TEMP_DIR/caritrans_latest/systemd/cari-api.service" ]]; then
        cp "$TEMP_DIR/caritrans_latest/systemd/cari-api.service" /etc/systemd/system/
        log_info "Updated cari-api.service"
    fi

    # Ensure Python dependencies are installed
    pip3 install --break-system-packages fastapi uvicorn pydantic 2>/dev/null || \
    pip3 install fastapi uvicorn pydantic 2>/dev/null || true

    # Reload systemd and restart API
    systemctl daemon-reload
    systemctl enable cari-api 2>/dev/null || true

    if systemctl is-active --quiet cari-api; then
        systemctl restart cari-api
        log_info "Restarted cari-api service"
    else
        systemctl start cari-api
        log_info "Started cari-api service"
    fi

    # Remove old sudoers file if it exists (API replaces sudo approach)
    if [[ -f /etc/sudoers.d/caritrans ]]; then
        rm -f /etc/sudoers.d/caritrans
        log_info "Removed old sudoers file (API replaces sudo approach)"
    fi

    log_info "API service updated"
}

# Update A/V sync monitor service
update_avsync_service() {
    log_step "Updating A/V Sync Monitor service..."

    # Update systemd service file
    if [[ -f "$TEMP_DIR/caritrans_latest/systemd/cari-avsync.service" ]]; then
        cp "$TEMP_DIR/caritrans_latest/systemd/cari-avsync.service" /etc/systemd/system/
        log_info "Updated cari-avsync.service"
        systemctl daemon-reload
    fi

    # Enable and start/restart service
    systemctl enable cari-avsync 2>/dev/null || true

    if systemctl is-active --quiet cari-avsync; then
        systemctl restart cari-avsync
        log_info "Restarted cari-avsync service"
    else
        systemctl start cari-avsync
        log_info "Started cari-avsync service"
    fi

    log_info "A/V Sync Monitor service updated"
}

# Update nginx config (add missing locations like /preview)
update_nginx_config() {
    log_step "Updating nginx configuration..."

    local NGINX_CONF="/etc/nginx/sites-available/caritrans"

    if [[ ! -f "$NGINX_CONF" ]]; then
        log_warn "Nginx config not found at $NGINX_CONF"
        return
    fi

    # Add /preview location if missing
    if ! grep -q "location /preview/" "$NGINX_CONF"; then
        log_info "Adding /preview location for HLS streams..."

        # Insert the preview location before the WebSocket location
        sed -i '/# WebSocket proxy/i \
    # HLS preview streams - serve directly without PHP auth\
    location /preview/ {\
        alias /var/www/caritrans/public/preview/;\
        add_header Access-Control-Allow-Origin *;\
        add_header Cache-Control "no-cache, no-store, must-revalidate";\
        types {\
            application/vnd.apple.mpegurl m3u8;\
            video/mp2t ts;\
        }\
    }\
' "$NGINX_CONF"

        # Test nginx config
        if nginx -t 2>/dev/null; then
            log_info "Nginx config updated successfully"
        else
            log_warn "Nginx config test failed - reverting"
        fi
    else
        log_info "Nginx config already has /preview location"
    fi
}

# Restart services
restart_services() {
    log_step "Restarting services..."

    # Restart PHP-FPM
    for ver in 8.4 8.3 8.2 8.1 8.0 7.4; do
        if systemctl is-active --quiet "php${ver}-fpm" 2>/dev/null; then
            systemctl restart "php${ver}-fpm"
            log_info "Restarted: php${ver}-fpm"
            break
        fi
    done

    # Reload nginx
    if systemctl is-active --quiet nginx; then
        systemctl reload nginx
        log_info "Reloaded: nginx"
    fi

    log_info "Services restarted"
}

# Fix permissions (ensure web GUI can write to config dirs)
fix_permissions() {
    log_step "Fixing permissions..."

    # Ensure log directory exists for transcoder bitrate monitoring
    LOG_DIR="/var/log/caritrans"
    if [[ ! -d "$LOG_DIR" ]]; then
        log_info "Creating log directory: $LOG_DIR"
        mkdir -p "$LOG_DIR"
    fi
    chown root:root "$LOG_DIR"
    chmod 755 "$LOG_DIR"

    # Parent config dir needs www-data group so web can traverse into subdirs
    if [[ -d "$CONFIG_DIR" ]]; then
        log_info "Fixing: $CONFIG_DIR -> $SERVICE_USER:$WEB_USER (750)"
        chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR"
        chmod 750 "$CONFIG_DIR"
    fi

    # Users config needs www-data read access for PHP authentication
    if [[ -f "$CONFIG_DIR/users.conf" ]]; then
        log_info "Fixing: $CONFIG_DIR/users.conf -> $SERVICE_USER:$WEB_USER (640)"
        chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/users.conf"
        chmod 640 "$CONFIG_DIR/users.conf"
    fi

    # Config subdirectories need www-data write access for web GUI
    for subdir in inputs transcoders muxers outputs; do
        if [[ -d "$CONFIG_DIR/$subdir" ]]; then
            log_info "Fixing: $CONFIG_DIR/$subdir -> $SERVICE_USER:$WEB_USER (775)"
            chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/$subdir"
            chmod 775 "$CONFIG_DIR/$subdir"
            # Also fix existing config files - set ownership AND permissions (664 = group writable)
            find "$CONFIG_DIR/$subdir" -type f \( -name "*.ini" -o -name "*.conf" \) -exec chown "$SERVICE_USER:$WEB_USER" {} \; 2>/dev/null || true
            find "$CONFIG_DIR/$subdir" -type f \( -name "*.ini" -o -name "*.conf" \) -exec chmod 664 {} \; 2>/dev/null || true
        fi
    done

    # Web directory
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    find "$WEB_DIR" -type d -exec chmod 755 {} \;
    find "$WEB_DIR" -type f -exec chmod 644 {} \;

    log_info "Permissions fixed"
}

# Cleanup temp files
cleanup() {
    log_step "Cleaning up..."

    if [[ -n "$TEMP_DIR" && -d "$TEMP_DIR" ]]; then
        rm -rf "$TEMP_DIR"
    fi

    log_info "Cleanup complete"
}

# Print completion
print_completion() {
    local SERVER_IP=$(hostname -I | awk '{print $1}')

    echo ""
    echo -e "${GREEN}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║         CariTranscoder Update Complete!                ║${NC}"
    echo -e "${GREEN}╚════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "Your configuration has been preserved."
    echo ""
    echo -e "${BLUE}Web Interface:${NC}"
    echo -e "  URL: ${GREEN}http://${SERVER_IP}:8080${NC}"
    echo ""
    echo -e "${BLUE}Check service status:${NC}"
    echo "  systemctl status nginx"
    echo "  systemctl status php*-fpm"
    echo ""
    if [[ -n "$BACKUP_DIR" ]]; then
        echo -e "${YELLOW}Backup location: $BACKUP_DIR${NC}"
        echo ""
    fi
}

# Main update flow
main() {
    check_root
    check_installation

    show_plan

    # Prompt for confirmation
    if [[ "$AUTO_CONFIRM" = false ]]; then
        if [[ -t 0 ]]; then
            read -p "Do you want to continue with the update? (y/N): " confirm
            if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
                echo -e "${YELLOW}Update cancelled.${NC}"
                exit 0
            fi
        else
            echo -e "${YELLOW}Non-interactive mode detected. Use -y flag to auto-confirm.${NC}"
            echo -e "Example: curl -sSL \"...update.sh?\$(date +%s)\" | sudo bash -s -- -y"
            exit 0
        fi
    fi

    echo ""
    backup_current
    download_latest
    update_web
    update_php_api
    update_gstreamer_plugins
    rebuild_apps
    build_tools
    rebuild_librist
    update_api
    update_avsync_service
    fix_permissions
    update_nginx_config
    restart_services
    cleanup
    print_completion
}

# Run main function
main "$@"

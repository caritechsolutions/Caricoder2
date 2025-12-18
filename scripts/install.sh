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
SCRIPT_VERSION="1.0.2"
INSTALL_DIR="/opt/caritrans"
CONFIG_DIR="/etc/caritrans"
WEB_DIR="/var/www/caritrans"
LOG_DIR="/var/log/caritrans"
RUN_DIR="/run/caritrans"
DATA_DIR="/var/lib/caritrans"
REPO_URL="https://github.com/caritechsolutions/Caricoder2"
BRANCH="claude/video-transcoder-gstreamer-YnBIH"
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

    # GStreamer
    apt-get install -y \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-tools

    # FFmpeg (for stream analysis and fallback transcoding)
    apt-get install -y ffmpeg

    # SRT support
    apt-get install -y libsrt-dev libsrt1.5-openssl || apt-get install -y libsrt-dev || true

    # SRT tools (srt-live-transmit for stream reception)
    apt-get install -y srt-tools || true

    # Build tools for librist
    apt-get install -y meson ninja-build cmake || true

    # SSL/Crypto
    apt-get install -y libssl-dev

    # PHP for web interface
    apt-get install -y \
        php-cli \
        php-fpm \
        php-json \
        php-mbstring

    # Nginx (optional, for production)
    apt-get install -y nginx || true

    log_info "Dependencies installed successfully"
}

# Install TSDuck from pre-built package
install_tsduck() {
    log_step "Installing TSDuck..."

    if command -v tsp &> /dev/null; then
        log_info "TSDuck already installed: $(tsp --version 2>&1 | head -1)"
        return 0
    fi

    # Detect architecture and OS version
    local ARCH=$(dpkg --print-architecture)
    source /etc/os-release
    log_info "Detected: $ARCH on $PRETTY_NAME ($VERSION_CODENAME)"

    # Determine TSDuck version and package name based on Ubuntu version
    local TSDUCK_VERSION=""
    local UBUNTU_TAG=""

    case "$VERSION_CODENAME" in
        noble|plucky|oracular)
            # Ubuntu 24.04+ - use latest TSDuck
            TSDUCK_VERSION="3.42-4421"
            UBUNTU_TAG="ubuntu24"
            ;;
        jammy)
            # Ubuntu 22.04 - use older version compatible with this release
            TSDUCK_VERSION="3.37-3520"
            UBUNTU_TAG="ubuntu22"
            ;;
        focal)
            # Ubuntu 20.04 - use version that supports focal
            TSDUCK_VERSION="3.26-2349"
            UBUNTU_TAG="ubuntu20"
            ;;
        *)
            # Unknown - try ubuntu24 package
            log_warn "Unknown Ubuntu version, trying ubuntu24 package"
            TSDUCK_VERSION="3.42-4421"
            UBUNTU_TAG="ubuntu24"
            ;;
    esac

    local PACKAGE_NAME="tsduck_${TSDUCK_VERSION}.${UBUNTU_TAG}_${ARCH}.deb"
    log_info "Looking for TSDuck package: $PACKAGE_NAME"

    local DEB_FILE=""
    local FOUND_LOCAL=false

    # STEP 1: Check for local package matching the OS version (preferred method)
    if [[ -d "$INSTALL_DIR/packages" ]]; then
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

# Install librist from source (for ristreceiver)
install_librist() {
    log_step "Installing librist (RIST library)..."

    if command -v ristreceiver &> /dev/null; then
        log_info "librist already installed: $(ristreceiver --help 2>&1 | head -1 || echo 'installed')"
        return 0
    fi

    local TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    log_info "Cloning librist from VideoLAN..."

    # Clone librist repository
    local CLONE_SUCCESS=false
    for attempt in 1 2 3; do
        if git clone --depth 1 https://code.videolan.org/rist/librist.git 2>/dev/null; then
            CLONE_SUCCESS=true
            break
        fi
        log_warn "Clone attempt $attempt failed, retrying..."
        sleep 2
    done

    if [[ "$CLONE_SUCCESS" = false ]]; then
        log_warn "Failed to clone librist repository"
        log_warn "RIST scanning will not be available"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    cd librist

    log_info "Building librist with meson/ninja..."

    # Configure with meson
    if ! meson setup build --buildtype=release -Dbuiltin_cjson=true; then
        log_warn "Meson setup failed"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Build
    if ! ninja -C build; then
        log_warn "Ninja build failed"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Install
    if ! ninja -C build install; then
        log_warn "Ninja install failed"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Update library cache
    ldconfig

    # Cleanup
    cd /
    rm -rf "$TEMP_DIR"

    # Verify installation
    if command -v ristreceiver &> /dev/null; then
        log_info "librist installed successfully"
    else
        # Check if installed to /usr/local/bin
        if [[ -f /usr/local/bin/ristreceiver ]]; then
            log_info "librist installed to /usr/local/bin"
        else
            log_warn "ristreceiver not found in PATH after install"
            log_warn "RIST scanning may not work"
        fi
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
    if command -v wget &> /dev/null; then
        if wget --no-check-certificate -q -O repo.tar.gz "$TARBALL_URL"; then
            DOWNLOAD_OK=true
        fi
    else
        if curl -k -L -f -o repo.tar.gz "$TARBALL_URL" 2>/dev/null; then
            DOWNLOAD_OK=true
        fi
    fi

    if [[ "$DOWNLOAD_OK" = false ]]; then
        log_error "wget/curl download failed"
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

# Install configuration files
install_config() {
    log_step "Installing configuration files..."

    cd "$INSTALL_DIR"

    # Main config
    if [[ ! -f "$CONFIG_DIR/caritrans.conf" ]]; then
        cp config/caritrans.conf "$CONFIG_DIR/"
        # Update paths in config
        sed -i "s|config_dir = .*|config_dir = $CONFIG_DIR|" "$CONFIG_DIR/caritrans.conf"
        sed -i "s|run_dir = .*|run_dir = $RUN_DIR|" "$CONFIG_DIR/caritrans.conf"
        sed -i "s|log_dir = .*|log_dir = $LOG_DIR|" "$CONFIG_DIR/caritrans.conf"
        sed -i "s|data_dir = .*|data_dir = $DATA_DIR|" "$CONFIG_DIR/caritrans.conf"
    else
        log_info "Config exists, preserving: caritrans.conf"
    fi

    # Users config
    if [[ ! -f "$CONFIG_DIR/users.conf" ]]; then
        cp config/users.conf "$CONFIG_DIR/"
        log_info "Created: users.conf"
    else
        log_info "Config exists, preserving: users.conf"
        # Fix broken default password hash from older versions (sha256('admin') -> sha256('caritrans:admin'))
        if grep -q "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918" "$CONFIG_DIR/users.conf" 2>/dev/null; then
            sed -i 's/8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918/ff926ade7adab2dac701d5e883389d449fc40cb79145159ba8905202f2191dbb/' "$CONFIG_DIR/users.conf"
            log_info "Fixed: default admin password hash"
        fi
    fi
    # Always fix users.conf permissions (needs to be readable by www-data for PHP authentication)
    chown "$SERVICE_USER:$WEB_USER" "$CONFIG_DIR/users.conf"
    chmod 640 "$CONFIG_DIR/users.conf"

    # Note: We don't copy example configs - users create their own via the web GUI

    # Set proper ownership on main config file only
    # Note: users.conf ownership is set above with www-data group for PHP access
    chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/caritrans.conf" 2>/dev/null || true

    log_info "Configuration files installed"
}

# Install web interface
install_web() {
    log_step "Installing web interface..."

    cd "$INSTALL_DIR"

    # Copy web files
    cp -r web/* "$WEB_DIR/"

    # Update config path in PHP
    sed -i "s|define('CONFIG_DIR', '.*')|define('CONFIG_DIR', '$CONFIG_DIR')|" "$WEB_DIR/includes/config.php" 2>/dev/null || true

    # Set permissions
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    find "$WEB_DIR" -type d -exec chmod 755 {} \;
    find "$WEB_DIR" -type f -exec chmod 644 {} \;

    log_info "Web interface installed to $WEB_DIR"
}

# Install systemd services
install_services() {
    log_step "Installing systemd services..."

    cd "$INSTALL_DIR"

    # Copy service files
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

# Configure Nginx (optional)
configure_nginx() {
    log_step "Configuring Nginx..."

    if ! command -v nginx &> /dev/null; then
        log_warn "Nginx not installed, skipping configuration"
        return
    fi

    # Detect PHP-FPM socket path
    local PHP_FPM_SOCK=""
    if [[ -S /var/run/php/php8.1-fpm.sock ]]; then
        PHP_FPM_SOCK="/var/run/php/php8.1-fpm.sock"
    elif [[ -S /var/run/php/php8.0-fpm.sock ]]; then
        PHP_FPM_SOCK="/var/run/php/php8.0-fpm.sock"
    elif [[ -S /var/run/php/php7.4-fpm.sock ]]; then
        PHP_FPM_SOCK="/var/run/php/php7.4-fpm.sock"
    else
        # Try to find any php-fpm socket
        PHP_FPM_SOCK=$(find /var/run/php -name "php*-fpm.sock" 2>/dev/null | head -1)
        if [[ -z "$PHP_FPM_SOCK" ]]; then
            PHP_FPM_SOCK="/var/run/php/php-fpm.sock"
        fi
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
    for ver in 8.3 8.2 8.1 8.0 7.4; do
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

    # Start Nginx
    if command -v nginx &> /dev/null; then
        log_info "Starting nginx..."
        systemctl enable nginx 2>/dev/null || true
        systemctl start nginx || systemctl restart nginx
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
    for ver in 8.3 8.2 8.1 8.0 7.4; do
        if systemctl is-active --quiet "php${ver}-fpm" 2>/dev/null; then
            echo -e "  PHP-FPM:     ${GREEN}Running (PHP $ver)${NC}"
            break
        fi
    done
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
    install_tsduck
    install_librist
    create_user
    create_directories
    download_repo
    build_apps
    install_binaries
    install_config
    install_web
    install_services
    configure_nginx
    create_tmpfiles
    start_services
    print_completion
}

# Run main
main "$@"

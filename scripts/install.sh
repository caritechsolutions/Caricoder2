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
INSTALL_DIR="/opt/caritrans"
CONFIG_DIR="/etc/caritrans"
WEB_DIR="/var/www/caritrans"
LOG_DIR="/var/log/caritrans"
RUN_DIR="/run/caritrans"
DATA_DIR="/var/lib/caritrans"
REPO_URL="https://github.com/caritechsolutions/Caricoder2.git"
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

    # Detect architecture and Ubuntu version
    local ARCH=$(dpkg --print-architecture)
    local RELEASE_URL=""

    # Get Ubuntu version codename
    source /etc/os-release
    log_info "Detected: $ARCH on $PRETTY_NAME ($VERSION_CODENAME)"

    # Use specific TSDuck versions known to work with each Ubuntu version
    # Check https://github.com/tsduck/tsduck/releases for available packages
    case "$VERSION_CODENAME" in
        focal)
            # Ubuntu 20.04 - use TSDuck 3.26-2349 (last version with focal packages)
            log_info "Using TSDuck 3.26 for Ubuntu 20.04 (focal)"
            if [[ "$ARCH" == "amd64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.26-2349/tsduck_3.26-2349.ubuntu20_amd64.deb"
            elif [[ "$ARCH" == "arm64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.26-2349/tsduck_3.26-2349.ubuntu20_arm64.deb"
            fi
            ;;
        jammy)
            # Ubuntu 22.04 - use TSDuck 3.32-2983 (last version with jammy packages)
            log_info "Using TSDuck 3.32 for Ubuntu 22.04 (jammy)"
            if [[ "$ARCH" == "amd64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.32-2983/tsduck_3.32-2983.ubuntu22_amd64.deb"
            elif [[ "$ARCH" == "arm64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.32-2983/tsduck_3.32-2983.ubuntu22_arm64.deb"
            fi
            ;;
        noble)
            # Ubuntu 24.04 - use latest TSDuck
            log_info "Using TSDuck 3.42 for Ubuntu 24.04 (noble)"
            if [[ "$ARCH" == "amd64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.42-4421/tsduck_3.42-4421.ubuntu24_amd64.deb"
            elif [[ "$ARCH" == "arm64" ]]; then
                RELEASE_URL="https://github.com/tsduck/tsduck/releases/download/v3.42-4421/tsduck_3.42-4421.ubuntu24_arm64.deb"
            fi
            ;;
        *)
            # Other/unknown - try latest Ubuntu package
            log_info "Unknown Ubuntu version, trying latest TSDuck release"
            local RELEASE_JSON=$(curl -sSL "https://api.github.com/repos/tsduck/tsduck/releases/latest")
            local ALL_DEBS=$(echo "$RELEASE_JSON" | grep -o '"browser_download_url": *"[^"]*\.deb"' | grep -v "\-dev" | cut -d'"' -f4)
            for url in $ALL_DEBS; do
                if echo "$url" | grep -iq "ubuntu" && echo "$url" | grep -iq "$ARCH"; then
                    RELEASE_URL="$url"
                    break
                fi
            done
            ;;
    esac

    if [[ -z "$RELEASE_URL" ]]; then
        log_warn "Could not find compatible TSDuck package for $PRETTY_NAME $ARCH"
        log_warn "TSDuck will need to be installed manually."
        log_warn "Visit: https://github.com/tsduck/tsduck/releases"
        return 1
    fi

    log_info "Downloading TSDuck from: $RELEASE_URL"

    # Download the .deb package
    local DEB_FILE="/tmp/tsduck.deb"
    rm -f "$DEB_FILE"

    # Use wget with explicit redirect following (more reliable for GitHub releases)
    if command -v wget &> /dev/null; then
        wget -q --show-progress -O "$DEB_FILE" "$RELEASE_URL" 2>&1 || true
    else
        curl -L -f -o "$DEB_FILE" "$RELEASE_URL" 2>&1 || true
    fi

    # Verify download
    if [[ ! -f "$DEB_FILE" || ! -s "$DEB_FILE" ]]; then
        log_error "Failed to download TSDuck package"
        return 1
    fi

    # Verify it's actually a .deb file (should start with "!<arch>")
    local FILE_TYPE=$(file "$DEB_FILE" 2>/dev/null || echo "unknown")
    if ! echo "$FILE_TYPE" | grep -qi "debian\|archive"; then
        log_error "Downloaded file is not a valid Debian package"
        log_error "File type: $FILE_TYPE"
        log_error "This may be a GitHub redirect issue. Try manual installation."
        rm -f "$DEB_FILE"
        return 1
    fi

    log_info "Download successful ($(du -h "$DEB_FILE" | cut -f1))"

    # Install dependencies that TSDuck might need
    log_info "Installing TSDuck runtime dependencies..."
    apt-get install -y libcurl4 libpcsclite1 libedit2 || true

    # Install the package
    log_info "Installing TSDuck package..."
    dpkg -i "$DEB_FILE"

    # Fix any missing dependencies
    apt-get install -f -y

    # Cleanup
    rm -f "$DEB_FILE"

    # Verify installation
    if command -v tsp &> /dev/null; then
        log_info "TSDuck installed successfully: $(tsp --version 2>&1 | head -1)"
    else
        log_warn "TSDuck installation may have failed. Check manually."
        log_warn "Visit: https://github.com/tsduck/tsduck/releases"
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
    chown -R "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR"
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$LOG_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$RUN_DIR"
    chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"

    # Secure config directory
    chmod 750 "$CONFIG_DIR"

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

    if command -v wget &> /dev/null; then
        wget -q --show-progress -O repo.tar.gz "$TARBALL_URL" 2>&1
    else
        curl -L -f -o repo.tar.gz "$TARBALL_URL"
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
        chmod 600 "$CONFIG_DIR/users.conf"
    else
        log_info "Config exists, preserving: users.conf"
    fi

    # Example service configs (don't overwrite existing)
    for dir in inputs transcoders muxers outputs; do
        for conf in config/$dir/*.conf; do
            if [[ -f "$conf" ]]; then
                basename=$(basename "$conf")
                if [[ ! -f "$CONFIG_DIR/$dir/$basename" ]]; then
                    cp "$conf" "$CONFIG_DIR/$dir/"
                fi
            fi
        done
    done

    chown -R "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR"

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
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    echo ""

    check_root
    check_os
    install_dependencies
    install_tsduck
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

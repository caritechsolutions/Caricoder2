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
    local UBUNTU_VERSION=""
    local DISTRO_PATTERN=""

    # Get Ubuntu version codename
    if [[ -f /etc/os-release ]]; then
        source /etc/os-release
        case "$VERSION_CODENAME" in
            focal)   UBUNTU_VERSION="20"; DISTRO_PATTERN="ubuntu20\|ubuntu2004\|focal" ;;
            jammy)   UBUNTU_VERSION="22"; DISTRO_PATTERN="ubuntu22\|ubuntu2204\|jammy" ;;
            noble)   UBUNTU_VERSION="24"; DISTRO_PATTERN="ubuntu24\|ubuntu2404\|noble" ;;
            *)       UBUNTU_VERSION="22"; DISTRO_PATTERN="ubuntu" ;;
        esac
    fi

    log_info "Detected: $ARCH on Ubuntu $UBUNTU_VERSION ($VERSION_CODENAME)"

    # Get the latest release .deb URL from GitHub API
    log_info "Fetching latest TSDuck release..."
    local RELEASE_JSON=$(curl -sSL "https://api.github.com/repos/tsduck/tsduck/releases/latest")

    # Find the .deb file matching our Ubuntu version and architecture
    local RELEASE_URL=""

    # Extract all .deb download URLs
    local ALL_DEBS=$(echo "$RELEASE_JSON" | grep -o '"browser_download_url": *"[^"]*\.deb"' | grep -v "\-dev" | cut -d'"' -f4)

    # First try: find exact match for Ubuntu version and architecture
    for url in $ALL_DEBS; do
        if echo "$url" | grep -iqE "$DISTRO_PATTERN" && echo "$url" | grep -iq "$ARCH"; then
            RELEASE_URL="$url"
            break
        fi
    done

    # Second try: find any Ubuntu package for this architecture
    if [[ -z "$RELEASE_URL" ]]; then
        for url in $ALL_DEBS; do
            if echo "$url" | grep -iq "ubuntu" && echo "$url" | grep -iq "$ARCH"; then
                RELEASE_URL="$url"
                break
            fi
        done
    fi

    # Third try: find any package for this architecture (avoid debian13 on older systems)
    if [[ -z "$RELEASE_URL" ]]; then
        for url in $ALL_DEBS; do
            if echo "$url" | grep -iq "$ARCH" && ! echo "$url" | grep -iq "debian13"; then
                RELEASE_URL="$url"
                break
            fi
        done
    fi

    if [[ -z "$RELEASE_URL" ]]; then
        log_warn "Could not find compatible TSDuck package for Ubuntu $UBUNTU_VERSION $ARCH"
        log_warn "TSDuck will need to be installed manually."
        log_warn "Visit: https://github.com/tsduck/tsduck/releases"
        return 1
    fi

    log_info "Downloading TSDuck from: $RELEASE_URL"

    # Download the .deb package
    local DEB_FILE="/tmp/tsduck.deb"
    curl -sSL -o "$DEB_FILE" "$RELEASE_URL"

    if [[ ! -f "$DEB_FILE" || ! -s "$DEB_FILE" ]]; then
        log_error "Failed to download TSDuck package"
        return 1
    fi

    # Install dependencies that TSDuck might need
    log_info "Installing TSDuck runtime dependencies..."
    apt-get install -y libcurl4 libpcsclite1 libedit2 || true
    apt-get install -y libsrt1.4-gnutls || apt-get install -y libsrt1-gnutls || apt-get install -y libsrt-openssl1.4 || true

    # Install the package
    log_info "Installing TSDuck package..."
    dpkg -i "$DEB_FILE" || true

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

# Clone or update repository
clone_repo() {
    log_step "Downloading CariTranscoder..."

    if [[ -d "$INSTALL_DIR/.git" ]]; then
        log_info "Repository exists, updating..."
        cd "$INSTALL_DIR"
        git fetch origin
        git checkout "$BRANCH"
        git pull origin "$BRANCH"
    else
        rm -rf "$INSTALL_DIR"/*
        git clone -b "$BRANCH" "$REPO_URL" "$INSTALL_DIR"
    fi

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

    # Create Nginx config
    cat > /etc/nginx/sites-available/caritrans << 'NGINX'
server {
    listen 8080;
    server_name _;

    root /var/www/caritrans/public;
    index index.php index.html;

    location / {
        try_files $uri $uri/ /index.php?$query_string;
    }

    location ~ \.php$ {
        include snippets/fastcgi-php.conf;
        fastcgi_pass unix:/var/run/php/php-fpm.sock;
        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
        include fastcgi_params;
    }

    location ~ /\.(ht|git) {
        deny all;
    }

    # WebSocket proxy for stats
    location /ws {
        proxy_pass http://127.0.0.1:8081;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
    }
}
NGINX

    # Enable site
    ln -sf /etc/nginx/sites-available/caritrans /etc/nginx/sites-enabled/

    # Test and reload
    nginx -t && systemctl reload nginx

    log_info "Nginx configured on port 8080"
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
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  CariTranscoder Installation Complete  ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Installation Summary:"
    echo "  - Binaries:    /usr/local/bin/cari-*"
    echo "  - Config:      $CONFIG_DIR"
    echo "  - Web:         $WEB_DIR"
    echo "  - Logs:        $LOG_DIR"
    echo "  - Data:        $DATA_DIR"
    echo ""
    echo "Quick Start:"
    echo "  1. Start web interface:"
    echo "     systemctl start nginx php-fpm"
    echo "     # Or use PHP built-in server:"
    echo "     php -S 0.0.0.0:8080 -t $WEB_DIR/public"
    echo ""
    echo "  2. Access web UI:"
    echo "     http://YOUR_SERVER_IP:8080"
    echo "     Default login: admin / admin"
    echo "     (Change password immediately!)"
    echo ""
    echo "  3. Start a service:"
    echo "     systemctl start cari-input@input-001"
    echo ""
    echo "  4. View logs:"
    echo "     journalctl -u cari-input@input-001 -f"
    echo ""
    echo "Update CariTranscoder:"
    echo "  curl -sSL https://raw.githubusercontent.com/caritechsolutions/Caricoder2/main/scripts/update.sh | sudo bash"
    echo ""
    echo -e "${YELLOW}IMPORTANT: Change the default admin password!${NC}"
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
    clone_repo
    build_apps
    install_binaries
    install_config
    install_web
    install_services
    configure_nginx
    create_tmpfiles
    print_completion
}

# Run main
main "$@"

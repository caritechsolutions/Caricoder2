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

# Install TSDuck from source
install_tsduck() {
    log_step "Installing TSDuck..."

    if command -v tsp &> /dev/null; then
        log_info "TSDuck already installed: $(tsp --version 2>&1 | head -1)"
        return 0
    fi

    local TSDUCK_BUILD_DIR="/tmp/tsduck-build"

    # Update CA certificates first
    log_info "Updating CA certificates..."
    apt-get install -y ca-certificates
    update-ca-certificates

    # Install TSDuck build dependencies (minimal, skip docs)
    log_info "Installing TSDuck build dependencies..."
    apt-get install -y \
        g++ \
        cmake \
        dos2unix \
        graphviz \
        libcurl4-openssl-dev \
        libpcsclite-dev \
        dpkg-dev \
        libedit-dev \
        libsrt-openssl-dev || apt-get install -y libsrt-dev || true

    # Clone TSDuck
    rm -rf "$TSDUCK_BUILD_DIR"
    git clone https://github.com/tsduck/tsduck.git "$TSDUCK_BUILD_DIR"
    cd "$TSDUCK_BUILD_DIR"

    # Build TSDuck (without docs to avoid Ruby gem SSL issues)
    log_info "Building TSDuck (this may take a while)..."
    make -j$(nproc) NODOC=1

    # Install TSDuck
    log_info "Installing TSDuck..."
    make install NODOC=1

    # Cleanup
    cd /
    rm -rf "$TSDUCK_BUILD_DIR"

    # Verify installation
    if command -v tsp &> /dev/null; then
        log_info "TSDuck installed successfully: $(tsp --version 2>&1 | head -1)"
    else
        log_warn "TSDuck installation may have failed. Check manually."
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

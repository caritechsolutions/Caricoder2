#!/bin/bash
#
# CariTranscoder Update Script
# Copyright (c) 2024 CariTech Solutions
#
# Usage: curl -sSL https://raw.githubusercontent.com/caritechsolutions/Caricoder2/main/scripts/update.sh | sudo bash
#

set -e

# Colors
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
BRANCH="claude/video-transcoder-gstreamer-YnBIH"

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

# Check root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

# Check installation exists
check_installation() {
    if [[ ! -d "$INSTALL_DIR/.git" ]]; then
        log_error "CariTranscoder not found at $INSTALL_DIR"
        log_error "Please run the install script first."
        exit 1
    fi
}

# Get current version
get_current_version() {
    cd "$INSTALL_DIR"
    git rev-parse --short HEAD 2>/dev/null || echo "unknown"
}

# Stop running services
stop_services() {
    log_step "Stopping running services..."

    local stopped_services=()

    # Find running cari-* services
    for svc in $(systemctl list-units --type=service --state=running --no-legend | grep 'cari-' | awk '{print $1}'); do
        log_info "Stopping $svc..."
        systemctl stop "$svc" || true
        stopped_services+=("$svc")
    done

    # Export for restart later
    export STOPPED_SERVICES="${stopped_services[*]}"
}

# Pull latest changes
pull_updates() {
    log_step "Pulling latest updates..."

    cd "$INSTALL_DIR"

    # Stash any local changes
    git stash 2>/dev/null || true

    # Fetch and pull
    git fetch origin
    git checkout "$BRANCH"
    git pull origin "$BRANCH"

    # Get new version
    NEW_VERSION=$(git rev-parse --short HEAD)
    log_info "Updated to version: $NEW_VERSION"
}

# Rebuild applications
rebuild_apps() {
    log_step "Rebuilding applications..."

    cd "$INSTALL_DIR"

    # Clean and rebuild
    make clean 2>/dev/null || true
    make all

    log_info "Build completed"
}

# Update binaries
update_binaries() {
    log_step "Updating binaries..."

    cd "$INSTALL_DIR"

    for app in cari-input cari-transcoder cari-mux cari-output cari-stats cari-ha; do
        if [[ -f "src/$app/$app" ]]; then
            install -m 755 "src/$app/$app" /usr/local/bin/
            log_info "Updated: $app"
        fi
    done
}

# Update web interface
update_web() {
    log_step "Updating web interface..."

    cd "$INSTALL_DIR"

    # Backup custom files if any
    if [[ -f "$WEB_DIR/includes/config.local.php" ]]; then
        cp "$WEB_DIR/includes/config.local.php" /tmp/config.local.php.bak
    fi

    # Update web files
    rsync -av --exclude='*.local.php' web/ "$WEB_DIR/"

    # Restore custom files
    if [[ -f /tmp/config.local.php.bak ]]; then
        mv /tmp/config.local.php.bak "$WEB_DIR/includes/config.local.php"
    fi

    # Fix permissions
    chown -R "$WEB_USER:$WEB_USER" "$WEB_DIR"
    find "$WEB_DIR" -type d -exec chmod 755 {} \;
    find "$WEB_DIR" -type f -exec chmod 644 {} \;

    log_info "Web interface updated"
}

# Update systemd services
update_services() {
    log_step "Updating systemd services..."

    cd "$INSTALL_DIR"

    # Copy new service files
    cp systemd/*.service /etc/systemd/system/

    # Update paths
    for svc in /etc/systemd/system/cari-*.service; do
        sed -i "s|CONFIG_DIR=/etc/caritrans|CONFIG_DIR=$CONFIG_DIR|g" "$svc"
    done

    # Reload systemd
    systemctl daemon-reload

    log_info "Services updated"
}

# Restart services
restart_services() {
    log_step "Restarting services..."

    if [[ -n "$STOPPED_SERVICES" ]]; then
        for svc in $STOPPED_SERVICES; do
            log_info "Starting $svc..."
            systemctl start "$svc" || log_warn "Failed to start $svc"
        done
    fi

    # Restart nginx if running
    if systemctl is-active --quiet nginx; then
        systemctl reload nginx || true
    fi
}

# Check for config updates
check_config_updates() {
    log_step "Checking configuration changes..."

    cd "$INSTALL_DIR"

    # Compare example configs with installed
    local changes=0
    for conf in config/*.conf; do
        basename=$(basename "$conf")
        if [[ -f "$CONFIG_DIR/$basename" ]]; then
            if ! diff -q "$conf" "$CONFIG_DIR/$basename" > /dev/null 2>&1; then
                changes=$((changes + 1))
            fi
        fi
    done

    if [[ $changes -gt 0 ]]; then
        log_warn "Configuration templates have changed."
        log_warn "Review changes in $INSTALL_DIR/config/ and update your configs if needed."
    fi
}

# Run database migrations if any
run_migrations() {
    log_step "Running migrations..."

    # Placeholder for future database migrations
    # Currently using file-based config, no migrations needed

    log_info "No migrations required"
}

# Print completion
print_completion() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}    CariTranscoder Update Complete      ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Version: $NEW_VERSION"
    echo ""
    echo "Services status:"
    systemctl list-units --type=service --state=running --no-legend | grep 'cari-' || echo "  No services running"
    echo ""
    echo "Check logs:"
    echo "  journalctl -u cari-input@input-001 -f"
    echo ""
}

# Main
main() {
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║     CariTranscoder Update Script       ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    echo ""

    check_root
    check_installation

    OLD_VERSION=$(get_current_version)
    log_info "Current version: $OLD_VERSION"

    stop_services
    pull_updates
    rebuild_apps
    update_binaries
    update_web
    update_services
    check_config_updates
    run_migrations
    restart_services
    print_completion
}

main "$@"

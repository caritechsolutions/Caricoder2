#!/bin/bash
#
# CariTranscoder Uninstall Script
# Copyright (c) 2024 CariTech Solutions
#
# Usage: curl -sSL https://raw.githubusercontent.com/caritechsolutions/Caricoder2/main/scripts/uninstall.sh | sudo bash
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

INSTALL_DIR="/opt/caritrans"
CONFIG_DIR="/etc/caritrans"
WEB_DIR="/var/www/caritrans"
LOG_DIR="/var/log/caritrans"
RUN_DIR="/run/caritrans"
DATA_DIR="/var/lib/caritrans"
SERVICE_USER="caritrans"

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}[ERROR]${NC} This script must be run as root"
        exit 1
    fi
}

confirm_uninstall() {
    echo ""
    echo -e "${YELLOW}WARNING: This will remove CariTranscoder from your system.${NC}"
    echo ""
    read -p "Are you sure you want to continue? [y/N] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Uninstall cancelled."
        exit 0
    fi
}

stop_all_services() {
    log_step "Stopping all services..."

    for svc in $(systemctl list-units --type=service --all --no-legend | grep 'cari-' | awk '{print $1}'); do
        systemctl stop "$svc" 2>/dev/null || true
        systemctl disable "$svc" 2>/dev/null || true
    done
}

remove_services() {
    log_step "Removing systemd services..."

    rm -f /etc/systemd/system/cari-*.service
    systemctl daemon-reload

    log_info "Services removed"
}

remove_binaries() {
    log_step "Removing binaries..."

    rm -f /usr/local/bin/cari-input
    rm -f /usr/local/bin/cari-transcoder
    rm -f /usr/local/bin/cari-mux
    rm -f /usr/local/bin/cari-output
    rm -f /usr/local/bin/cari-stats
    rm -f /usr/local/bin/cari-ha

    log_info "Binaries removed"
}

remove_web() {
    log_step "Removing web interface..."

    rm -rf "$WEB_DIR"
    rm -f /etc/nginx/sites-enabled/caritrans
    rm -f /etc/nginx/sites-available/caritrans

    # Reload nginx if running
    systemctl reload nginx 2>/dev/null || true

    log_info "Web interface removed"
}

remove_directories() {
    log_step "Removing directories..."

    rm -rf "$INSTALL_DIR"
    rm -rf "$RUN_DIR"

    # Ask about config and data
    echo ""
    read -p "Remove configuration files ($CONFIG_DIR)? [y/N] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$CONFIG_DIR"
        log_info "Configuration removed"
    else
        log_info "Configuration preserved at $CONFIG_DIR"
    fi

    read -p "Remove log files ($LOG_DIR)? [y/N] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$LOG_DIR"
        log_info "Logs removed"
    else
        log_info "Logs preserved at $LOG_DIR"
    fi

    read -p "Remove data files ($DATA_DIR)? [y/N] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$DATA_DIR"
        log_info "Data removed"
    else
        log_info "Data preserved at $DATA_DIR"
    fi
}

remove_user() {
    log_step "Removing system user..."

    if id "$SERVICE_USER" &>/dev/null; then
        userdel "$SERVICE_USER" 2>/dev/null || true
        log_info "User $SERVICE_USER removed"
    fi
}

remove_tmpfiles() {
    rm -f /etc/tmpfiles.d/caritrans.conf
}

print_completion() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}   CariTranscoder Uninstall Complete    ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "CariTranscoder has been removed from your system."
    echo ""
}

main() {
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║   CariTranscoder Uninstall Script      ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    echo ""

    check_root
    confirm_uninstall
    stop_all_services
    remove_services
    remove_binaries
    remove_web
    remove_directories
    remove_user
    remove_tmpfiles
    print_completion
}

main "$@"

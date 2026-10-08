#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# install.sh — Install HoustonKVM on CentOS Stream 9 / 10
#
# For production, install the RPM (houstonkvm.spec) instead: it handles
# versions, configuration files and clean upgrades. This script is for quick
# installs without packaging.
#
# Usage:
#   sudo bash scripts/install.sh [--port=<n>]
#
# Options (override the default written to /etc/houstonkvm/houstonkvm.conf):
#   --port=N        HTTP port to listen on  (default: 8080)
#
# Capture device, resolution/fps, and the input backend (HID gadget vs.
# CH9329 serial vs. QEMU QMP) are configured per target after first login
# (Admin -> Targets in the web UI) — not by this script.
#
# What this does:
#   1. Builds the binary (cmake --build build)
#   2. Creates the houstonkvm system user
#   3. Installs binary + HID setup script to /usr/bin/
#   4. Creates the database directory /var/lib/houstonkvm/
#   5. Installs config to /etc/houstonkvm/ (preserves existing)
#   6. Installs udev rule for /dev/hidg* permissions
#   7. Installs and enables two systemd units
#   8. Opens the chosen port, 8443/tcp for HTTPS and the WebRTC UDP range in
#      firewalld (if active)
#   9. Starts the services and prints post-install notes about TLS and SELinux
# -----------------------------------------------------------------------------
set -euo pipefail

# ── Require root ──────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: run as root — sudo bash scripts/install.sh" >&2
    exit 1
fi

# ── Locate project root (directory that contains this script) ─────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# ── Parse optional overrides ──────────────────────────────────────────────────
PORT=8080

for arg in "$@"; do
    case "$arg" in
        --port=*) PORT="${arg#--port=}" ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# ── Colour helpers ────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
step()  { echo -e "\n${GREEN}==> $*${NC}"; }
warn()  { echo -e "${YELLOW}WARN: $*${NC}"; }
fatal() { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }

# ── 1. Build ──────────────────────────────────────────────────────────────────
step "Building HoustonKVM"
if [[ ! -d build ]]; then
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build --parallel "$(nproc)"
[[ -x build/HoustonKVM ]] || fatal "Build failed — build/HoustonKVM not found"

# ── 2. System user ────────────────────────────────────────────────────────────
step "Creating system user 'houstonkvm'"
if ! id -u houstonkvm &>/dev/null; then
    useradd --system \
            --no-create-home \
            --shell /sbin/nologin \
            --comment "HoustonKVM service" \
            --user-group \
            houstonkvm
fi
# Needs 'input' group for /dev/hidg*, 'video' group for /dev/video*,
# 'dialout' group for /dev/ttyUSB* (CH9329 serial HID adapter), 'audio'
# group for /dev/snd/* (the capture dongle's sound).
usermod -aG input houstonkvm
usermod -aG video houstonkvm
usermod -aG dialout houstonkvm
usermod -aG audio houstonkvm

# ── 3. Install binaries ───────────────────────────────────────────────────────
step "Installing binaries to /usr/bin/"
install -m 755 build/HoustonKVM              /usr/bin/HoustonKVM
install -m 755 scripts/setup-hid-gadget.sh /usr/bin/houstonkvm-setup-hid
install -D -m 644 docs/houstonkvm.8 /usr/share/man/man8/houstonkvm.8
install -D -m 644 docs/HoustonKVM.8 /usr/share/man/man8/HoustonKVM.8

# ── 4. Database directory ─────────────────────────────────────────────────────
step "Creating /var/lib/houstonkvm"
install -d -m 750 -o houstonkvm -g houstonkvm /var/lib/houstonkvm

# ── 5. Configuration ──────────────────────────────────────────────────────────
step "Installing configuration to /etc/houstonkvm/"
install -d -m 755 /etc/houstonkvm
# For a certificate the configuration manages (see houstonkvm(8)); left empty.
install -d -m 750 -o root -g houstonkvm /etc/houstonkvm/tls

if [[ -f /etc/houstonkvm/houstonkvm.conf ]]; then
    warn "/etc/houstonkvm/houstonkvm.conf already exists — not overwriting"
    warn "To reset defaults: rm /etc/houstonkvm/houstonkvm.conf && sudo bash scripts/install.sh"
else
    cat > /etc/houstonkvm/houstonkvm.conf <<EOF
# HoustonKVM — runtime configuration
# Edit this file, then run: systemctl restart houstonkvm

HOUSTONKVM_OPTS="--port=${PORT} --db=/var/lib/houstonkvm/houstonkvm.db"
EOF
    chmod 644 /etc/houstonkvm/houstonkvm.conf
fi

# ── 6. udev rule for /dev/hidg* ───────────────────────────────────────────────
step "Installing udev rule for HID gadget devices"
cat > /etc/udev/rules.d/99-houstonkvm-hid.rules <<'EOF'
# Allow the 'input' group to access USB HID gadget endpoints.
KERNEL=="hidg[0-9]*", SUBSYSTEM=="usb_gadget", GROUP="input", MODE="0660"
EOF
udevadm control --reload-rules

# ── 7. systemd units ──────────────────────────────────────────────────────────
step "Installing systemd units"
install -m 644 scripts/houstonkvm-hid.service /etc/systemd/system/
install -m 644 scripts/houstonkvm.service     /etc/systemd/system/

systemctl daemon-reload
systemctl enable houstonkvm-hid.service
systemctl enable houstonkvm.service

# ── 8. Firewall ───────────────────────────────────────────────────────────────
step "Configuring firewall"
# WebRTC (low-latency mode) needs its ICE UDP range reachable too — see
# kWebrtcPortRangeBegin/End in include/webrtc/selective_forwarding_unit.h. Without
# this, the HTTP UI loads fine and MJPEG mode works (it's just TCP on
# $PORT), but WebRTC mode silently fails ICE with no other symptom.
if systemctl is-active --quiet firewalld; then
    if firewall-cmd --query-port="${PORT}/tcp" --permanent &>/dev/null; then
        echo "  firewalld: port ${PORT}/tcp already open"
    else
        firewall-cmd --add-port="${PORT}/tcp" --permanent
        echo "  firewalld: opened port ${PORT}/tcp"
    fi
    # HTTPS, once an Owner turns it on (8443 unless they change it).
    if firewall-cmd --query-port="8443/tcp" --permanent &>/dev/null; then
        echo "  firewalld: port 8443/tcp already open"
    else
        firewall-cmd --add-port="8443/tcp" --permanent
        echo "  firewalld: opened port 8443/tcp (HTTPS)"
    fi
    if firewall-cmd --query-port="50000-50100/udp" --permanent &>/dev/null; then
        echo "  firewalld: port 50000-50100/udp already open"
    else
        firewall-cmd --add-port="50000-50100/udp" --permanent
        echo "  firewalld: opened port 50000-50100/udp (WebRTC ICE)"
    fi
    firewall-cmd --reload
else
    warn "firewalld is not running — open ${PORT}/tcp, 8443/tcp and 50000-50100/udp manually if needed"
fi

# ── 9. Start services ─────────────────────────────────────────────────────────
step "Starting services"
systemctl start houstonkvm-hid.service || warn "HID setup failed (no USB OTG? Video-only mode OK)"
systemctl start houstonkvm.service

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}Installation complete.${NC}"
echo ""
echo "  Status:   systemctl status houstonkvm"
echo "  Logs:     journalctl -u houstonkvm -f"
echo "  Config:   /etc/houstonkvm/houstonkvm.conf  (restart after changes)"
echo "  URL:      http://$(hostname -I | awk '{print $1}'):${PORT}"
echo ""
echo "First visit creates the owner account (via the First Setup form)."
echo "Each machine you want to control is then added as a target under"
echo "Admin → Targets (Owner role only): its capture device, resolution and"
echo "input backend."
echo ""
echo -e "${YELLOW}Security notes:${NC}"
echo "  HTTPS:    The server starts on plain HTTP. Turn on HTTPS under"
echo "            Admin → Network, which can also make the certificate."
echo "            Locked out? Add --tls=off to /etc/houstonkvm/houstonkvm.conf"
echo "            and restart. See: man houstonkvm"
echo "  SELinux:  If the service fails to bind ${PORT}/tcp, run:"
echo "            semanage port -a -t http_port_t -p tcp ${PORT}"
echo "  Rate limiting: /api/login has brute-force lockout per source IP,"
echo "            but consider putting it behind fail2ban too for defense in depth."

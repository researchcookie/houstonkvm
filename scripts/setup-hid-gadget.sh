#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# setup-hid-gadget.sh
# Configure a USB HID gadget (keyboard + mouse) on an ARM board that supports
# USB device/OTG mode (e.g. Raspberry Pi 4 USB-C port).
#
# After running this script the target PC connected via USB cable will see the
# board as a USB keyboard and mouse. HoustonKVM writes to /dev/hidg0
# (keyboard) and /dev/hidg1 (mouse).
#
# Usage:
#   sudo bash scripts/setup-hid-gadget.sh
#
# To run it at every boot, enable houstonkvm-hid.service (installed by the
# RPM and by install.sh), which calls this script before the main service.
# -----------------------------------------------------------------------------
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: run this script as root (sudo)." >&2
    exit 1
fi

GADGET_DIR=/sys/kernel/config/usb_gadget/houstonkvm

# ── Load required kernel modules ─────────────────────────────────────────────
modprobe libcomposite
modprobe dwc2 || true   # may already be built-in on some boards

# ── If already configured, skip recreating ───────────────────────────────────
if [[ -d "$GADGET_DIR" ]]; then
    echo "HID gadget already configured at $GADGET_DIR — skipping setup."
    ls -1 /dev/hidg* 2>/dev/null && echo "Devices: $(ls /dev/hidg*)" || true
    exit 0
fi

# ── Mount configfs if not already mounted ────────────────────────────────────
if ! mountpoint -q /sys/kernel/config; then
    mount -t configfs none /sys/kernel/config
fi

# ── Create gadget ─────────────────────────────────────────────────────────────
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

echo 0x1d6b > idVendor    # Linux Foundation
echo 0x0104 > idProduct   # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "SKVM-001"  > strings/0x409/serialnumber
echo "HoustonKVM"  > strings/0x409/manufacturer
echo "KVM HID"   > strings/0x409/product

# ── HID function: keyboard ────────────────────────────────────────────────────
# Standard boot-compatible keyboard descriptor (8-byte reports).
mkdir -p functions/hid.kbd
echo 1 > functions/hid.kbd/protocol       # Keyboard
echo 1 > functions/hid.kbd/subclass       # Boot Interface Subclass
echo 8 > functions/hid.kbd/report_length
printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' \
    > functions/hid.kbd/report_desc

# ── HID function: mouse ───────────────────────────────────────────────────────
# Absolute (tablet-style) mouse: 3 buttons + absolute X/Y + relative wheel.
# No relative-mode fallback — HoustonKVM only drives this device in absolute
# coordinates (see InputBackend::mouseMoveAbsolute), so there's no 4-byte
# relative report to fall back to. 6-byte report:
#   byte 0    : buttons (bits 0-2) + 5 bits padding
#   bytes 1-2 : X, 16-bit LE, logical range 0..32767
#   bytes 3-4 : Y, 16-bit LE, logical range 0..32767
#   byte 5    : wheel, signed 8-bit, relative
mkdir -p functions/hid.mouse
echo 2 > functions/hid.mouse/protocol     # Mouse
echo 1 > functions/hid.mouse/subclass     # Boot Interface Subclass
echo 6 > functions/hid.mouse/report_length
printf '\x05\x01\x09\x02\xa1\x01\x09\x01\xa1\x00\x05\x09\x19\x01\x29\x03\x15\x00\x25\x01\x95\x03\x75\x01\x81\x02\x95\x01\x75\x05\x81\x03\x05\x01\x09\x30\x09\x31\x16\x00\x00\x26\xff\x7f\x75\x10\x95\x02\x81\x02\x09\x38\x15\x81\x25\x7f\x75\x08\x95\x01\x81\x06\xc0\xc0' \
    > functions/hid.mouse/report_desc

# ── USB configuration ─────────────────────────────────────────────────────────
mkdir -p configs/c.1
echo 250 > configs/c.1/MaxPower
mkdir -p configs/c.1/strings/0x409
echo "HID Keyboard+Mouse" > configs/c.1/strings/0x409/configuration

ln -s functions/hid.kbd   configs/c.1/
ln -s functions/hid.mouse configs/c.1/

# ── Bind to the USB device controller ────────────────────────────────────────
UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
if [[ -z "$UDC" ]]; then
    echo "ERROR: no USB device controller found in /sys/class/udc/." >&2
    echo "       Make sure dwc2 (or your board's OTG driver) is loaded and" >&2
    echo "       the USB-C/OTG port is wired to the target PC." >&2
    exit 1
fi

echo "$UDC" > UDC
echo "HID gadget bound to UDC: $UDC"
echo "Keyboard device : /dev/hidg0"
echo "Mouse device    : /dev/hidg1"

# Allow the 'input' group to write to the HID gadget devices
chown root:input /dev/hidg0 /dev/hidg1
chmod 0660       /dev/hidg0 /dev/hidg1

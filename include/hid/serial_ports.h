#pragma once

#include <string>
#include <vector>

namespace houston_kvm {

struct SerialPort {
    std::string path;   ///< Resolved device node, e.g. "/dev/ttyUSB0" — what
                         ///< gets saved as TargetSettings::serialDevice.
    std::string label;  ///< Human-readable identity when known (the
                         ///< /dev/serial/by-id/ symlink name, which encodes
                         ///< vendor/product/serial — e.g. a CH9329 adapter
                         ///< typically shows up as
                         ///< "usb-QinHeng_Electronics_USB_Single_Serial_...");
                         ///< falls back to `path` when by-id isn't available.
};

/// Lists USB-serial adapters currently attached — used to drive the Admin
/// page's CH9329 serial-device picker so setup doesn't require already
/// knowing which of possibly several /dev/ttyUSB*/ttyACM* nodes is the
/// right one. Prefers /dev/serial/by-id (stable across reboots/replug, and
/// its symlink names carry the vendor string); falls back to a plain /dev
/// scan for ttyUSB*/ttyACM* nodes if that directory doesn't exist or is
/// empty. Never throws — returns an empty list if /dev can't be scanned.
std::vector<SerialPort> enumerateSerialPorts();

} // namespace houston_kvm

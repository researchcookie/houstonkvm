#include "hid/serial_ports.h"

#include <algorithm>
#include <filesystem>

namespace houston_kvm {

namespace fs = std::filesystem;

std::vector<SerialPort> enumerateSerialPorts() {
    std::vector<SerialPort> ports;
    std::error_code ec;

    const fs::path byId = "/dev/serial/by-id";
    if (fs::is_directory(byId, ec)) {
        for (const auto& entry : fs::directory_iterator(byId, ec)) {
            std::error_code rec;
            fs::path resolved = fs::canonical(entry.path(), rec);
            if (rec) continue; // dangling symlink — device unplugged since udev created it
            ports.push_back({resolved.string(), entry.path().filename().string()});
        }
    }
    if (!ports.empty()) {
        std::sort(ports.begin(), ports.end(),
                 [](const auto& a, const auto& b) { return a.label < b.label; });
        return ports;
    }

    // No by-id entries (older kernel, or udev's serial rules not
    // installed) — fall back to a plain scan so a connected adapter still
    // shows up, just without a friendly label.
    for (const auto& entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("ttyUSB") || name.starts_with("ttyACM"))
            ports.push_back({entry.path().string(), name});
    }
    std::sort(ports.begin(), ports.end(),
             [](const auto& a, const auto& b) { return a.path < b.path; });
    return ports;
}

} // namespace houston_kvm

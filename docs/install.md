# Installing HoustonKVM

## What you need

| | |
|---|---|
| **A server** | Linux with a C++20 toolchain to build. Developed and tested on x86-64 CentOS Stream 9. Other distros and ARM boards should work but have not been tested. |
| **Video in** | At least one capture device that Linux exposes through V4L2 and that offers **MJPEG**, which most USB HDMI capture dongles do (`v4l2-ctl --list-formats-ext` will tell you). |
| **Sound (optional)** | Most HDMI capture dongles are also a USB sound card carrying the machine's sound. HoustonKVM finds it on its own, and viewers hear it with low-latency video once they unmute. |
| **Keyboard and mouse out** | At least one **CH9329** serial-to-USB-HID adapter plugged into the target (I bought mine on eBay, but you can solder your own). |

## From the dnf repository (EL9 and EL10, x86_64)

Signed packages for CentOS Stream, RHEL, AlmaLinux and Rocky Linux 9 and 10.
They need EPEL and CRB, and Cisco's OpenH264 repository, which `epel-release`
sets up and enables:

```console
$ sudo dnf install -y epel-release
$ sudo dnf config-manager --set-enabled crb
$ sudo dnf config-manager --add-repo https://researchcookie.github.io/houstonkvm-rpms/houstonkvm.repo
$ sudo dnf install houstonkvm
$ sudo systemctl enable --now houstonkvm
$ sudo firewall-cmd --permanent --add-service=houstonkvm && sudo firewall-cmd --reload
```

dnf asks you to accept the HoustonKVM signing key the first time (fingerprint
`B5C7 3228 C40F 5A14 594F A267 F847 7884 BFE2 8D25`). Then carry on with
[First run](#first-run).


## Building the RPM yourself

The build needs the EPEL and CRB repositories. Low-latency H.264 video also needs
Cisco's OpenH264 build from the `epel-cisco-openh264` repository on the machine that
runs the server. Without it the server still works, with MJPEG video only.

```console
$ sudo dnf install -y epel-release rpm-build
$ sudo dnf config-manager --set-enabled crb epel-cisco-openh264
$ sudo dnf builddep -y houstonkvm.spec
$ bash scripts/fetch-rpm-sources.sh          # the only step that needs the network
$ rpmbuild -ba houstonkvm.spec
$ sudo dnf install ~/rpmbuild/RPMS/*/houstonkvm-[0-9]*.rpm
$ sudo systemctl enable --now houstonkvm
```

The RPM installs a locked-down systemd service (running as its own `houstonkvm`
user), and keeps its database in `/var/lib/houstonkvm/`. If you will use the USB
gadget backend, also `sudo systemctl enable --now houstonkvm-hid`.

## From source

```console
$ scripts/setup-centos9.sh                   # build dependencies into /usr/local
$ cmake -B build -S . && cmake --build build -j"$(nproc)"
$ ./build/HoustonKVM --port=8080             # try it: keeps houstonkvm.db in the current directory
$ sudo bash scripts/install.sh               # or install it as a service
```

`scripts/setup-centos9.sh` is also the readable reference for what the build needs
on another distribution: GCC 11 or newer, CMake, SQLite, OpenSSL, libsodium,
libjpeg-turbo, OpenH264, alsa-lib, Opus, zlib and nlohmann/json from the system, plus uSockets,
uWebSockets and libdatachannel, which the script builds from upstream (the RPM
pins each one to an exact release). If you stage those somewhere other than `/usr/local`, point CMake at them
with `-DHOUSTONKVM_DEPS_PREFIX=<dir>`.

## First run

1. Open `http://<server>:8080/` and create the Owner account. The form asks for a
   one-time **setup code** that the server prints to its log when it starts with no
   accounts (`journalctl -u houstonkvm`, or the terminal it runs in), so someone who
   reaches the port before you can't claim the server.
2. Go to **Admin → Targets → Add target**. Pick the capture device and how keyboard
   and mouse reach the machine, and give it a name, a group and tags. Devices that
   are plugged in are offered in a list, and ones already used by another target are
   marked.
3. Add accounts under **Admin → Users**. **An Owner cannot drive a target**: taking
   control needs an **Operator** account, so that administering the server and
   piloting a machine stay separate.
4. Open the target from the directory, press **Take control**, and click the screen.

A fresh install starts with a placeholder "Default target". Edit or delete it when
you add your own.

The server listens on TCP 8080 by default (`--port`), and on 8443 once HTTPS is
on. Low-latency WebRTC video uses UDP 50000–50100, so open those too if a firewall
is in the way. The RPM ships a firewalld service covering all three, but does not
open anything by itself:
`sudo firewall-cmd --permanent --add-service=houstonkvm && sudo firewall-cmd --reload`.
If SELinux stops the service binding a non-default port, allow it with
`sudo semanage port -a -t http_port_t -p tcp <port>`.

HoustonKVM makes no connections out of your network by itself. Low-latency video
uses no STUN or TURN server unless an Owner adds one under **Admin → Network**,
and on a LAN or VPN it doesn't need one. To fix the list in the server's
configuration instead, use `--ice-server` (see `HoustonKVM --help`).

## Hardware and compatibility

The goal is to work with as many CH9329 adapters and capture dongles as possible.
The CH9329 path is exercised against real hardware. Capture support is the
generic V4L2/MJPEG path, so a UVC device that offers MJPEG should work.

**If you try a device, please tell us how it went** by opening an issue with the
model, its USB ID (`lsusb`), the output of `v4l2-ctl --list-formats-ext` for a
capture device, and what worked or didn't.

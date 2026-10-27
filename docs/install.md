# Installing HoustonKVM

## What you need

| | |
|---|---|
| **A server** | Linux with a C++20 toolchain to build. Developed and tested on x86-64 CentOS Stream 9. Other distros and ARM boards should work but have not been tested. |
| **Video in** | At least one capture device that Linux exposes through V4L2 and that offers **MJPEG**, which most USB HDMI capture dongles do (`v4l2-ctl --list-formats-ext` will tell you). |
| **Sound (optional)** | Most HDMI capture dongles are also a USB sound card carrying the machine's sound. HoustonKVM finds it on its own, and viewers hear it with low-latency video once they unmute. |
| **Keyboard and mouse out** | For each machine, a **CH9329** serial-to-USB-HID adapter plugged into it (I bought mine on eBay, but you can solder your own). Or, if you would rather the part plugged into your machines ran firmware you can read and build yourself, a **Raspberry Pi Pico or Arduino running kvmd's open-source HID firmware**, with a USB-serial adapter: see [Keyboard and mouse with open-source firmware](#keyboard-and-mouse-with-open-source-firmware). |

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

## Keyboard and mouse with open-source firmware

A CH9329's firmware isn't published, so nobody outside its maker can check what
it does, and it is plugged into every machine you drive. The alternative is a
small microcontroller board that you flash yourself with the HID firmware from
the [kvmd project](https://github.com/pikvm/kvmd/tree/master/hid), which is open
source (GPL-3.0-or-later) and which you can read before you build it.
HoustonKVM speaks that firmware's serial protocol, as of kvmd 4.227. The
protocol may change in later kvmd releases, so if a newer build doesn't answer,
build that release's tag instead.

The firmware isn't part of HoustonKVM: HoustonKVM doesn't include or distribute
it, and you build and flash it yourself from its own source. HoustonKVM is an
independent project, not affiliated with or endorsed by the firmware's authors,
so please report problems with this setup to HoustonKVM. This support is new:
it is tested against a simulated board, and reports from real hardware are
welcome.

### What to buy, per machine

| | |
|---|---|
| **A board** | A **Raspberry Pi Pico** (the original RP2040 one, which the firmware is built for), or an ATmega32U4 board running at 5 V and 16 MHz: an **Arduino Micro** or **Leonardo**, or a 5 V **Pro Micro**. |
| **A USB-serial adapter** | Its logic level must match the board: **3.3 V for a Pico**, whose pins don't take 5 V, and 5 V for one of the Arduinos. Adapters with a Silicon Labs CP2102N or an FTDI FT232R chip and a 3.3 V/5 V jumper do either. With several machines, choose one that has a serial number (both of those do): Linux then gives each adapter a fixed name under `/dev/serial/by-id/`. |
| **Wires** | Three female-to-female jumper wires (four for a Pico), and a USB cable from the board to the machine. |

### Build and flash the firmware

Read it first if that is why you are here: it is under `hid/pico` and
`hid/arduino` in the repository. To build a fixed version rather than the
latest, check out one of its release tags.

For a Pico (needs git, CMake, a C++ compiler, and the `arm-none-eabi` GCC
toolchain with newlib):

```console
$ git clone https://github.com/pikvm/kvmd && cd kvmd/hid/pico
$ make                                       # fetches the Pico SDK, then writes hid.uf2
```

Hold the Pico's BOOTSEL button while you plug it into a computer. It shows up as
a drive named `RPI-RP2`. Copy `hid.uf2` onto it, and the Pico restarts running the
firmware.

For an Arduino (needs [PlatformIO](https://platformio.org/), e.g.
`pip install --user platformio`), with the board plugged into this computer:

```console
$ git clone https://github.com/pikvm/kvmd && cd kvmd/hid/arduino
$ make serial                                # commands on the board's serial pins, at 115200
$ make upload                                # flashes the board on /dev/ttyACM0
```

### Wire it up

Connect the adapter to the board's serial pins, crossed over: what one side
transmits, the other receives. Leave the adapter's VCC pin unconnected, because
the board takes its power from the machine it's plugged into. Then plug the board's
USB port into the machine, and the adapter into this server.

| USB-serial adapter | Raspberry Pi Pico | Arduino Micro, Leonardo or Pro Micro |
|---|---|---|
| TX | GP21, pin 27 | RX, pin 0 (`RXI` on a Pro Micro) |
| RX | GP20, pin 26 | TX, pin 1 (`TXO` on a Pro Micro) |
| GND | GND, pin 28 | GND |
| | **GP22, pin 29, wired to GND (pin 33)** | |

The Pico firmware listens on its serial pins only if GP22 is wired to ground.
Without that wire it listens on SPI instead and never answers. Leave GP2 to GP9
unconnected: the firmware reads them at power-up, and grounding them switches the
keyboard or mouse to PS/2, the mouse to relative or Windows 98 mode, or turns USB
off altogether (GP5 selects a bridge mode with no USB keyboard or mouse).
HoustonKVM needs a USB keyboard and an absolute USB mouse.

### Check it, then add the target

`scripts/kvmd-hid-selftest.py` tests a board on its own, without the server. It
checks that the board answers and that the machine has recognised a keyboard and
an absolute mouse, then measures how reliable the link is. Last, it checks that
input really arrives: it presses Caps Lock twice to see the machine switch its
Caps Lock light, and asks you to watch the pointer visit the corners of the
screen. `--skip-target-tests` leaves out that last part, and `--detect-baud`
finds a board built for another speed. Stop the server first if it is already
using the adapter:

```console
$ python3 scripts/kvmd-hid-selftest.py /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A10KZ3X1-if00-port0
```

Then, in **Admin → Targets**, choose **Raspberry Pi Pico or Arduino with kvmd's
open-source HID firmware** and the adapter's serial device, and leave the speed
at 115200. With more than one adapter plugged in, choose **Other…** and enter its
`/dev/serial/by-id/` path, so the target keeps the right one after a reboot.

Some differences from a CH9329:

- The speed is set when the firmware is built (115200 unless you change it), so
  Admin can't change it the way it can for a CH9329. If the target's speed setting
  doesn't match, the input health panel says which speed the board answers at.
- HoustonKVM waits for the board to confirm each key, click and movement, which
  takes a couple of milliseconds, and sends a key or click again if it didn't
  arrive (a lost movement is put right by the next one). FTDI
  adapters normally hold a short reply back for up to 16 ms; HoustonKVM asks
  their driver not to. If the pointer still lags with one, check that
  `/sys/bus/usb-serial/devices/ttyUSB0/latency_timer` (for that adapter) reads 1.
- Input health reports whether the machine has recognised the board, and names a
  mouse set to relative or PS/2 mode. There is no firmware version to show.

## Hardware and compatibility

The goal is to work with as many CH9329 adapters, boards and capture dongles as
possible. The CH9329 path is exercised against real hardware. Boards running
kvmd's HID firmware are new and have been tested against a simulated board so
far. Capture support is the generic V4L2/MJPEG path, so a UVC device that offers
MJPEG should work.

**If you try a device, please tell us how it went** by opening an issue with the
model, its USB ID (`lsusb`), the output of `v4l2-ctl --list-formats-ext` for a
capture device or of `scripts/kvmd-hid-selftest.py` for a board, and what worked
or didn't.

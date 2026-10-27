#include "hid/serial_link.h"

#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

namespace houston_kvm {

namespace {

speed_t baudToSpeed(int baudRate) {
    switch (baudRate) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:
            std::cerr << "serial: unsupported baud rate " << baudRate
                      << ", defaulting to 9600\n";
            return B9600;
    }
}

} // namespace

bool isSerialBaud(int baud) {
    return std::find(kSerialBauds.begin(), kSerialBauds.end(), baud) != kSerialBauds.end();
}

bool setSerialSpeed(int fd, int baud) {
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) return false;
    speed_t speed = baudToSpeed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    return tcsetattr(fd, TCSANOW, &tty) == 0;
}

int openRawSerial(const std::string& device, int baud, std::string& error, bool lowLatency) {
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        error = "cannot open " + device + ": " + strerror(errno);
        return -1;
    }

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        error = std::string("tcgetattr failed: ") + strerror(errno);
        close(fd);
        return -1;
    }

    speed_t speed = baudToSpeed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;   // no parity
    tty.c_cflag &= ~CSTOPB;   // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;       // 8 data bits
    tty.c_cflag &= ~CRTSCTS;  // no hardware flow control
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        error = std::string("tcsetattr failed: ") + strerror(errno);
        close(fd);
        return -1;
    }
    if (lowLatency) {
        // Best effort: a pty, or a driver without the flag, refuses it, and
        // the line works the same, only with the driver's own batching.
        serial_struct ss{};
        if (ioctl(fd, TIOCGSERIAL, &ss) == 0) {
            ss.flags |= ASYNC_LOW_LATENCY;
            ioctl(fd, TIOCSSERIAL, &ss);
        }
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

bool writeAllRetrying(int fd, const uint8_t* data, size_t len) {
    size_t sent     = 0;
    auto   deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            auto remainingMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            pollfd pfd{fd, POLLOUT, 0};
            int ready = remainingMs > 0 ? poll(&pfd, 1, static_cast<int>(remainingMs)) : 0;
            if (ready == 0) errno = ETIMEDOUT;   // so the caller's log names the cause
            if (ready <= 0) return false;
            continue;
        }
        return false; // real error (device gone, etc.) — not retryable
    }
    return true;
}

} // namespace houston_kvm

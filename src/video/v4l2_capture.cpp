#include "video/v4l2_capture.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace houston_kvm {

namespace fs = std::filesystem;


V4l2Capture::V4l2Capture(Config cfg, FrameCallback onFrame)
    : cfg_(std::move(cfg)), frameCb_(std::move(onFrame)) {
    try {
        openDevice();
        initMmap();
        startStreaming();
    } catch (...) {
        // openDevice()/initMmap() may have already opened fd_ and/or mmap'd
        // some buffers before failing; ~V4l2Capture() never runs on a
        // throwing constructor, so clean those up here or they leak.
        releaseResources();
        throw;
    }
    running_ = true;
    captureThread_ = std::thread(&V4l2Capture::captureLoop, this);
}

V4l2Capture::~V4l2Capture() {
    running_ = false;
    if (captureThread_.joinable())
        captureThread_.join();
    releaseResources();
}

void V4l2Capture::releaseResources() noexcept {
    stopStreaming();
    for (auto& buf : bufs_) {
        if (buf.start && buf.start != MAP_FAILED)
            munmap(buf.start, buf.length);
    }
    bufs_.clear();
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
}

std::vector<uint8_t> V4l2Capture::snapshot() const {
    std::lock_guard<std::mutex> lk(snapshotMtx_);
    return latestFrame_;
}

void V4l2Capture::openDevice() {
    fd_ = open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0)
        throw V4l2Exception("V4L2: cannot open " + cfg_.device +
                            ": " + strerror(errno), V4l2Error::CannotOpen);

    v4l2_capability cap{};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
        throw V4l2Exception("V4L2: VIDIOC_QUERYCAP failed: " +
                            std::string(strerror(errno)), V4l2Error::CannotOpen);
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        throw V4l2Exception("V4L2: " + cfg_.device +
                            " is not a capture device", V4l2Error::NotCaptureDevice);
    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        throw V4l2Exception("V4L2: " + cfg_.device +
                            " does not support streaming I/O", V4l2Error::NoStreamingSupport);
    v4l2_format fmt{};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = cfg_.width;
    fmt.fmt.pix.height      = cfg_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        throw V4l2Exception(
            "V4L2: " + cfg_.device +
            " does not support MJPEG; use v4l2-ctl --list-formats-ext to check",
            V4l2Error::MjpegUnsupported);
    }

    if (fmt.fmt.pix.width != cfg_.width || fmt.fmt.pix.height != cfg_.height) {
        std::cerr << "V4L2: requested " << cfg_.width << "x" << cfg_.height
                   << " but driver negotiated " << fmt.fmt.pix.width << "x"
                   << fmt.fmt.pix.height << " instead\n";
    }
    cfg_.width  = fmt.fmt.pix.width;
    cfg_.height = fmt.fmt.pix.height;

    // Best-effort frame rate. Read back whatever the driver actually set
    // (it's free to round or ignore the request, same as width/height
    // above) so fps() reports reality rather than the request.
    v4l2_streamparm parm{};
    parm.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = cfg_.fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator > 0) {
        cfg_.fps = parm.parm.capture.timeperframe.denominator /
                   parm.parm.capture.timeperframe.numerator;
    }
}

std::vector<V4l2Capture::FrameSize> V4l2Capture::enumerateModes(const std::string& device) {
    int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        throw V4l2Exception("V4L2: cannot open " + device +
                            ": " + strerror(errno), V4l2Error::CannotOpen);

    v4l2_capability cap{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        int err = errno;
        close(fd);
        throw V4l2Exception("V4L2: VIDIOC_QUERYCAP failed: " +
                            std::string(strerror(err)), V4l2Error::CannotOpen);
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close(fd);
        throw V4l2Exception("V4L2: " + device +
                            " is not a capture device", V4l2Error::NotCaptureDevice);
    }

    std::vector<FrameSize> modes;
    v4l2_frmsizeenum fse{};
    fse.pixel_format = V4L2_PIX_FMT_MJPEG;
    for (fse.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0; ++fse.index) {
        if (fse.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            continue; // continuous/stepwise range — no fixed list to offer

        FrameSize mode{fse.discrete.width, fse.discrete.height, {}};
        v4l2_frmivalenum fie{};
        fie.pixel_format = V4L2_PIX_FMT_MJPEG;
        fie.width        = fse.discrete.width;
        fie.height       = fse.discrete.height;
        for (fie.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie) == 0; ++fie.index) {
            if (fie.type != V4L2_FRMIVAL_TYPE_DISCRETE)
                continue;
            const auto& iv = fie.discrete;
            if (iv.numerator > 0)
                mode.fps.push_back(iv.denominator / iv.numerator);
        }
        modes.push_back(std::move(mode));
    }

    close(fd);
    return modes;
}

std::vector<V4l2Capture::DeviceInfo> V4l2Capture::enumerateDevices() {
    std::vector<DeviceInfo> devices;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        // "video" followed by one or more digits, e.g. "video0" — skips
        // unrelated /dev entries without assuming a fixed node-number range.
        if (name.size() < 6 || name.compare(0, 5, "video") != 0) continue;
        if (!std::all_of(name.begin() + 5, name.end(),
                         [](unsigned char c) { return std::isdigit(c); }))
            continue;

        const std::string path = entry.path().string();
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue; // e.g. permission denied — not offerable anyway

        v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            close(fd);
            continue;
        }

        v4l2_frmsizeenum fse{};
        fse.pixel_format = V4L2_PIX_FMT_MJPEG;
        bool mjpeg = ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0;
        close(fd);

        // v4l2_capability.card is a fixed-size, NUL-terminated char array
        // per the V4L2 spec, so constructing directly from it is safe.
        devices.push_back({path, reinterpret_cast<const char*>(cap.card), mjpeg});
    }
    std::sort(devices.begin(), devices.end(),
             [](const auto& a, const auto& b) { return a.path < b.path; });
    return devices;
}

void V4l2Capture::initMmap() {
    v4l2_requestbuffers req{};
    req.count  = cfg_.numBufs;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
        throw V4l2Exception("V4L2: VIDIOC_REQBUFS failed: " +
                            std::string(strerror(errno)), V4l2Error::RequestBuffersFailed);
    if (req.count < 2)
        throw V4l2Exception("V4L2: driver allocated fewer than 2 buffers",
                            V4l2Error::InsufficientBuffers);

    bufs_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
            throw V4l2Exception("V4L2: VIDIOC_QUERYBUF failed",
                                V4l2Error::RequestBuffersFailed);

        bufs_[i].length = buf.length;
        bufs_[i].start  = mmap(nullptr, buf.length,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd_, static_cast<off_t>(buf.m.offset));
        if (bufs_[i].start == MAP_FAILED)
            throw V4l2Exception("V4L2: mmap failed: " +
                                std::string(strerror(errno)), V4l2Error::MmapFailed);
    }
}

void V4l2Capture::startStreaming() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(bufs_.size()); ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            throw V4l2Exception("V4L2: VIDIOC_QBUF failed", V4l2Error::StreamingStartFailed);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
        throw V4l2Exception("V4L2: VIDIOC_STREAMON failed: " +
                            std::string(strerror(errno)), V4l2Error::StreamingStartFailed);
}

void V4l2Capture::stopStreaming() {
    if (fd_ < 0) return;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
}

void V4l2Capture::stopWithError(const std::string& reason) {
    std::cerr << "V4L2: " << cfg_.device << ": " << reason << "\n";
    stopReason_ = reason;
    running_.store(false, std::memory_order_release);
}

void V4l2Capture::captureLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{1, 0}; 

        int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            stopWithError(std::string("select: ") + strerror(errno));
            break;
        }
        if (r == 0) continue; // timeout

        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            stopWithError(std::string("VIDIOC_DQBUF: ") + strerror(errno));
            break;
        }

        const auto* data  = static_cast<const uint8_t*>(bufs_[buf.index].start);
        const size_t size = buf.bytesused;
        {
            std::lock_guard<std::mutex> lk(snapshotMtx_);
            latestFrame_.assign(data, data + size);
        }

        if (frameCb_)
            frameCb_(data, size);

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            std::cerr << "V4L2: re-enqueue VIDIOC_QBUF failed\n";
    }
}

}
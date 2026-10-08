#include "video/synthetic_capture.h"

#include "video/v4l2_capture.h"

#include <turbojpeg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace houston_kvm {

namespace {

// Two seconds of frames: long enough that the loop point isn't a keyframe
// storm, short enough to stay a few megabytes per target.
constexpr uint32_t kLoopSeconds = 2;
// Typical of the MJPEG USB capture dongles HoustonKVM is used with.
constexpr int kJpegQuality = 85;

struct Rgb {
    uint8_t r, g, b;
};

class Canvas {
public:
    Canvas(uint32_t w, uint32_t h) : w_(w), h_(h), px_(static_cast<size_t>(w) * h * 3) {}

    void fill(Rgb c) {
        for (size_t i = 0; i < px_.size(); i += 3) {
            px_[i] = c.r;
            px_[i + 1] = c.g;
            px_[i + 2] = c.b;
        }
    }

    void set(int x, int y, Rgb c) {
        if (x < 0 || y < 0 || x >= static_cast<int>(w_) || y >= static_cast<int>(h_)) return;
        auto* p = &px_[(static_cast<size_t>(y) * w_ + x) * 3];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
    }

    void rect(int x, int y, int w, int h, Rgb c) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) set(i, j, c);
    }

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    std::vector<uint8_t>& pixels() { return px_; }
    const std::vector<uint8_t>& pixels() const { return px_; }

private:
    uint32_t             w_, h_;
    std::vector<uint8_t> px_;
};

// Deterministic, so every run and every target shows the same picture.
struct XorShift {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

// Lines of pseudo-text: glyph-sized blobs with a speckled inside, so the
// JPEGs come out about as large as a real screen of text does. Plain
// flat colour would compress to almost nothing and flatter every number.
void drawText(Canvas& c, uint32_t seed, int x0, int y0, int w, int h) {
    constexpr Rgb ink{40, 40, 48};
    XorShift rng{seed | 1};
    for (int y = y0 + 8; y + 12 < y0 + h; y += 18) {
        int x = x0 + 12;
        int lineEnd = x0 + w - 12 - static_cast<int>(rng.next() % (w / 3 + 1));
        while (x + 8 < lineEnd) {
            int word = 2 + static_cast<int>(rng.next() % 9);
            for (int g = 0; g < word && x + 8 < lineEnd; ++g, x += 9)
                for (int gy = 0; gy < 11; ++gy)
                    for (int gx = 0; gx < 7; ++gx)
                        if (rng.next() % 5 < 2) c.set(x + gx, y + gy, ink);
            x += 9;
        }
    }
}

void drawPointer(Canvas& c, int x, int y) {
    for (int j = 0; j < 20; ++j)
        for (int i = 0; i <= j / 2 + 1; ++i)
            c.set(x + i, y + j, (i == 0 || i == j / 2 + 1 || j == 19) ? Rgb{0, 0, 0} : Rgb{255, 255, 255});
}

Canvas desktopBase(uint32_t w, uint32_t h) {
    Canvas c(w, h);
    c.fill({58, 110, 165});                                        // wallpaper
    int bar = 28;
    c.rect(0, static_cast<int>(h) - bar, static_cast<int>(w), bar, {30, 30, 36}); // taskbar
    int wx = static_cast<int>(w) / 10, wy = static_cast<int>(h) / 10;
    int ww = static_cast<int>(w) * 7 / 10, wh = static_cast<int>(h) * 7 / 10;
    c.rect(wx, wy, ww, 24, {200, 200, 210});                       // title bar
    c.rect(wx, wy + 24, ww, wh - 24, {250, 250, 250});             // window
    drawText(c, 0x5eed, wx, wy + 24, ww, wh - 24);
    return c;
}

std::vector<uint8_t> toJpeg(tjhandle tj, const Canvas& c) {
    unsigned char* buf = nullptr;
    unsigned long  size = 0;
    if (tjCompress2(tj, c.pixels().data(), static_cast<int>(c.width()), 0,
                    static_cast<int>(c.height()), TJPF_RGB, &buf, &size,
                    TJSAMP_422, kJpegQuality, TJFLAG_FASTDCT) != 0)
        throw std::runtime_error(std::string("SyntheticCapture: JPEG encode failed: ") +
                                 tjGetErrorStr2(tj));
    std::vector<uint8_t> out(buf, buf + size);
    tjFree(buf);
    return out;
}

bool exists(const std::string& path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

} // namespace

SyntheticCapture::SyntheticCapture(std::string_view device, uint32_t width, uint32_t height,
                                   uint32_t fps, FrameCallback onFrame)
    : width_(std::clamp<uint32_t>(width, 64, 3840)),
      height_(std::clamp<uint32_t>(height, 64, 2160)),
      fps_(std::clamp<uint32_t>(fps, 1, 60)),
      frameCb_(std::move(onFrame)) {
    auto spec = device.substr(kPrefix.size());
    if (auto q = spec.find('?'); q != std::string_view::npos) {
        constexpr std::string_view kUnplugged = "unplugged=", kNoSignal = "nosignal=",
                                   kStall = "stall=";
        auto options = spec.substr(q + 1);
        spec         = spec.substr(0, q);
        while (!options.empty()) {
            auto amp    = options.find('&');
            auto option = options.substr(0, amp);
            options     = amp == std::string_view::npos ? std::string_view{} : options.substr(amp + 1);
            if (option.substr(0, kUnplugged.size()) == kUnplugged)
                unpluggedFile_ = std::string(option.substr(kUnplugged.size()));
            else if (option.substr(0, kNoSignal.size()) == kNoSignal)
                noSignalFile_ = std::string(option.substr(kNoSignal.size()));
            else if (option.substr(0, kStall.size()) == kStall)
                stallFile_ = std::string(option.substr(kStall.size()));
            else
                throw std::invalid_argument("SyntheticCapture: unknown option '" + std::string(option) +
                                            "' (expected unplugged=<file>, nosignal=<file> or "
                                            "stall=<file>)");
        }
    }
    auto pattern = spec.substr(0, spec.find('/'));
    if (pattern != "desktop" && pattern != "motion")
        throw std::invalid_argument("SyntheticCapture: unknown pattern '" + std::string(pattern) +
                                    "' (expected desktop or motion)");
    if (exists(unpluggedFile_))
        throw V4l2Exception("V4L2: cannot open " + std::string(device) + ": No such device",
                            V4l2Error::CannotOpen);

    tjhandle tj = tjInitCompress();
    if (!tj) throw std::runtime_error("SyntheticCapture: tjInitCompress failed");

    const Canvas base = desktopBase(width_, height_);
    const uint32_t count = fps_ * kLoopSeconds;
    frames_.reserve(count);
    try {
        for (uint32_t f = 0; f < count; ++f) {
            Canvas c = base;
            double t = static_cast<double>(f) / count;   // 0..1 around the loop
            if (pattern == "motion") {
                // Scroll the whole screen and sweep a colour wash across it,
                // so every pixel changes every frame.
                auto& px = c.pixels();
                const auto& src = base.pixels();
                size_t rowBytes = static_cast<size_t>(width_) * 3;
                uint32_t shift = static_cast<uint32_t>(t * height_);
                for (uint32_t y = 0; y < height_; ++y) {
                    const uint8_t* s = &src[((y + shift) % height_) * rowBytes];
                    uint8_t* d = &px[y * rowBytes];
                    for (uint32_t x = 0; x < width_; ++x) {
                        double wave = std::sin(6.2832 * (static_cast<double>(x) / width_ + t));
                        auto tint = static_cast<int>(40 * wave);
                        d[x * 3]     = static_cast<uint8_t>(std::clamp(s[x * 3] + tint, 0, 255));
                        d[x * 3 + 1] = s[x * 3 + 1];
                        d[x * 3 + 2] = static_cast<uint8_t>(std::clamp(s[x * 3 + 2] - tint, 0, 255));
                    }
                }
            }
            // A pointer drifting around the screen, and a clock that ticks
            // once a second in the taskbar.
            double a = 6.2832 * t;
            drawPointer(c, static_cast<int>(width_ * (0.5 + 0.35 * std::cos(a))),
                        static_cast<int>(height_ * (0.5 + 0.35 * std::sin(2 * a))));
            drawText(c, 0xc10c + f / fps_, static_cast<int>(width_) - 110,
                     static_cast<int>(height_) - 30, 110, 30);
            frames_.push_back(toJpeg(tj, c));
        }
    } catch (...) {
        tjDestroy(tj);
        throw;
    }
    tjDestroy(tj);

    thread_ = std::thread(&SyntheticCapture::run, this);
}

SyntheticCapture::~SyntheticCapture() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::vector<uint8_t> SyntheticCapture::snapshot() const {
    std::lock_guard<std::mutex> lock(latestMtx_);
    return frames_[latest_];
}

void SyntheticCapture::run() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps_);
    auto next = clock::now();
    for (size_t i = 0; running_; i = (i + 1) % frames_.size()) {
        if (exists(unpluggedFile_)) {
            // What V4l2Capture::captureLoop does on a pulled dongle.
            std::cerr << "V4L2: synthetic: VIDIOC_DQBUF: No such device\n";
            stopReason_ = "VIDIOC_DQBUF: No such device";
            running_.store(false, std::memory_order_release);
            break;
        }
        if (!stalled_ && exists(stallFile_)) stalled_ = true;
        if (stalled_ || exists(noSignalFile_)) {
            next = clock::now() + period;
            std::this_thread::sleep_until(next);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(latestMtx_);
            latest_ = i;
        }
        frameCb_(frames_[i].data(), frames_[i].size());
        // Paced against a fixed schedule rather than "sleep one period", so
        // a slow callback doesn't lower the rate; if it falls a whole frame
        // behind, skip ahead instead of bursting to catch up.
        next += period;
        auto now = clock::now();
        if (next < now - period) next = now;
        std::this_thread::sleep_until(next);
    }
}

} // namespace houston_kvm

// HoustonKVM-loadgen: opens many viewers on one target of a running
// HoustonKVM server and reports what each one actually received. Driven by
// scripts/loadtest.py; built only with -DHOUSTONKVM_LOADTEST=ON.
//
//   HoustonKVM-loadgen --port=8080 --token=<API token> --target=<id>
//                      [--host=127.0.0.1] [--mjpeg=N] [--webrtc=N]
//                      [--warmup=S] [--seconds=S] [--sync]
//                      [--answer-delay-ms=MS] [--legacy-signaling] [--tls]
//
// MJPEG viewers read /api/targets/<id>/stream; WebRTC viewers subscribe the
// way the browser does (subscribe, answer, add the server's candidates from
// the answer's response, receive RTP). --legacy-signaling uses the original
// flow instead, candidates in the offer; --answer-delay-ms holds the answer
// back as a browser's own ICE gathering does. Frames are counted
// only during the --seconds window that follows --warmup, and the result is
// printed as one JSON object on stdout. With --sync it first prints
// {"event":"ready"} after the warmup and waits for a line on stdin before
// measuring, so several instances can measure the same window. --tls talks
// HTTPS (the WebRTC media itself is DTLS either way), without checking the
// certificate: this measures the server, it doesn't trust it.
//
// Written in C++ rather than Python so that, running on the same machine as
// the server, the viewers cost as little CPU as a viewer can.

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int         port = 8080;
    std::string token;
    long        target  = 0;
    int         mjpeg   = 0;
    int         webrtc  = 0;
    double      warmup  = 5;
    double      seconds = 15;
    bool        sync    = false;
    // Wait this long after gathering before posting the answer, as a
    // browser does while its STUN server answers (100 ms to seconds).
    int         answerDelayMs = 0;
    // The original signaling: the server's ICE candidates in its offer,
    // rather than in the answer's response (?candidates=on-answer).
    bool        legacySignaling = false;
    bool        tls = false;
};

SSL_CTX* tlsContext = nullptr;   // set in main with --tls

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int connectTo(const Options& o) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// A connection to the server, over TLS with --tls. Used by one thread;
// another may only shutdown() its fd, to end a blocking recv().
class Conn {
public:
    static std::unique_ptr<Conn> open(const Options& o) {
        int fd = connectTo(o);
        if (fd < 0) return nullptr;
        std::unique_ptr<Conn> c(new Conn(fd));
        if (tlsContext) {
            c->ssl_ = SSL_new(tlsContext);
            if (!c->ssl_ || SSL_set_fd(c->ssl_, fd) != 1 || SSL_connect(c->ssl_) != 1) return nullptr;
        }
        return c;
    }
    ~Conn() {
        if (ssl_) SSL_free(ssl_);
        close(fd_);
    }
    int fd() const { return fd_; }

    bool sendAll(std::string_view data) {
        while (!data.empty()) {
            ssize_t n = ssl_ ? SSL_write(ssl_, data.data(), static_cast<int>(data.size()))
                             : send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            data.remove_prefix(static_cast<size_t>(n));
        }
        return true;
    }
    // > 0 bytes read, <= 0 closed or failed.
    ssize_t recv(char* buf, size_t size) {
        return ssl_ ? SSL_read(ssl_, buf, static_cast<int>(size)) : ::recv(fd_, buf, size, 0);
    }

private:
    explicit Conn(int fd) : fd_(fd) {}
    int  fd_;
    SSL* ssl_ = nullptr;
};

std::string requestHead(const Options& o, std::string_view method, const std::string& path,
                        size_t bodySize) {
    return std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + o.host + ":" +
           std::to_string(o.port) + "\r\nAuthorization: Bearer " + o.token +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(bodySize) +
           "\r\nConnection: close\r\n\r\n";
}

struct HttpResult {
    int         status = 0;
    std::string body;
};

// One request on its own connection. Small bodies only (JSON and SDP).
std::optional<HttpResult> httpRequest(const Options& o, std::string_view method,
                                      const std::string& path, const std::string& body) {
    auto conn = Conn::open(o);
    if (!conn || !conn->sendAll(requestHead(o, method, path, body.size()) + body)) return std::nullopt;
    std::string in;
    char buf[16384];
    size_t headerEnd = std::string::npos;
    size_t length    = std::string::npos;
    while (true) {
        if (headerEnd == std::string::npos) {
            headerEnd = in.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                auto p = in.find("Content-Length:");
                if (p == std::string::npos) p = in.find("content-length:");
                if (p != std::string::npos && p < headerEnd) length = std::stoul(in.substr(p + 15));
            }
        }
        if (headerEnd != std::string::npos && length != std::string::npos &&
            in.size() >= headerEnd + 4 + length)
            break;
        ssize_t n = conn->recv(buf, sizeof(buf));
        if (n <= 0) break;
        in.append(buf, static_cast<size_t>(n));
    }
    if (in.size() < 12 || headerEnd == std::string::npos) return std::nullopt;
    HttpResult r;
    r.status = std::atoi(in.c_str() + 9);
    r.body   = in.substr(headerEnd + 4, length);
    return r;
}

// Counters one viewer updates as frames arrive, and the main thread reads
// at the start and end of the measurement window.
struct Viewer {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<double>   firstFrameMs{-1};
    std::string           error;          // set before `done`, read after
    std::atomic<bool>     done{false};

    uint64_t framesAtStart = 0, bytesAtStart = 0;

    // WebRTC only: H.264 keyframes (IDR) received, and whether the frame
    // being received has one. Frames arrive on one libdatachannel thread.
    std::atomic<uint64_t> keyframes{0};
    std::atomic<double>   firstKeyframeMs{-1};   // a browser shows nothing before it
    uint64_t              keyframesAtStart = 0;
    bool                  frameHasIdr = false;

    // WebRTC only: Opus packets, one per 20 ms of sound.
    std::atomic<uint64_t> audioPackets{0};
    std::atomic<uint64_t> audioBytes{0};
    uint64_t              audioPacketsAtStart = 0, audioBytesAtStart = 0;
};

// Whether an H.264 RTP packet (RFC 6184) carries the start of an IDR slice.
bool startsIdr(const rtc::binary& rtp) {
    auto byte = [&rtp](size_t i) { return std::to_integer<uint8_t>(rtp[i]); };
    size_t at = 12 + 4 * (byte(0) & 0x0F);                     // fixed header + CSRCs
    if (byte(0) & 0x10) {                                       // header extension
        if (rtp.size() < at + 4) return false;
        at += 4 + 4 * ((size_t{byte(at + 2)} << 8) | byte(at + 3));
    }
    if (rtp.size() <= at) return false;
    uint8_t type = byte(at) & 0x1F;
    if (type == 5) return true;                                 // single NAL unit
    if (type == 28)                                             // FU-A: first fragment of an IDR
        return rtp.size() > at + 1 && (byte(at + 1) & 0x80) && (byte(at + 1) & 0x1F) == 5;
    if (type == 24) {                                           // STAP-A: look inside
        for (size_t i = at + 1; i + 2 < rtp.size();) {
            size_t len = (size_t{byte(i)} << 8) | byte(i + 1);
            if ((byte(i + 2) & 0x1F) == 5) return true;
            i += 2 + len;
        }
    }
    return false;
}

// ── MJPEG ────────────────────────────────────────────────────────────────

void runMjpeg(const Options& o, Viewer& v, const std::atomic<bool>& stop, std::atomic<int>& fdOut) {
    auto t0 = Clock::now();
    auto conn = Conn::open(o);
    std::string path = "/api/targets/" + std::to_string(o.target) + "/stream";
    if (!conn || !conn->sendAll(requestHead(o, "GET", path, 0))) {
        v.error = "connect failed";
        v.done  = true;
        return;
    }
    fdOut = conn->fd();

    // The stream is "--frame\r\n...Content-Length: N\r\n\r\n<N bytes>\r\n"
    // repeated. Part headers are collected in `head`; frame bytes are only
    // counted off (`need`), never copied.
    std::string head;
    bool   responseOk = false;
    size_t need = 0;
    auto frameDone = [&v, t0] {
        if (v.firstFrameMs < 0) v.firstFrameMs = msSince(t0);
        ++v.frames;
    };
    // Parses whatever complete headers `head` holds. False on a non-200.
    auto parseHead = [&]() {
        while (need == 0) {
            if (!responseOk) {
                auto e = head.find("\r\n\r\n");
                if (e == std::string::npos) return true;
                if (head.compare(0, 12, "HTTP/1.1 200") != 0) {
                    v.error = "HTTP " + head.substr(9, 3);
                    return false;
                }
                head.erase(0, e + 4);
                responseOk = true;
                continue;
            }
            auto cl = head.find("Content-Length: ");
            auto e  = cl == std::string::npos ? cl : head.find("\r\n\r\n", cl);
            if (e == std::string::npos) return true;
            need = std::stoul(head.substr(cl + 16)) + 2;   // the frame and its CRLF
            head.erase(0, e + 4);
            size_t take = std::min(need, head.size());
            need -= take;
            head.erase(0, take);
            if (need == 0) frameDone();
        }
        return true;
    };

    std::vector<char> chunk(256 * 1024);
    while (!stop) {
        ssize_t n = conn->recv(chunk.data(), chunk.size());
        if (n <= 0) {
            if (!stop) v.error = "stream closed by server";
            break;
        }
        v.bytes += static_cast<uint64_t>(n);
        size_t off = 0;
        if (need > 0) {
            off = std::min(need, static_cast<size_t>(n));
            need -= off;
            if (need == 0) frameDone();
        }
        if (off < static_cast<size_t>(n)) {
            head.append(chunk.data() + off, static_cast<size_t>(n) - off);
            if (!parseHead()) break;
        }
    }
    fdOut = -1;
    conn.reset();
    v.done = true;
}

// ── WebRTC ───────────────────────────────────────────────────────────────

struct WebrtcViewer : Viewer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track>          video, audio;
    std::atomic<bool>                    connected{false};
    double                               subscribeMs = -1;   // HTTP subscribe round trip
};

bool startWebrtc(const Options& o, WebrtcViewer& v) {
    auto t0   = Clock::now();
    auto base = "/api/targets/" + std::to_string(o.target) + "/webrtc/";
    auto sub  = httpRequest(o, "POST", base + (o.legacySignaling ? "subscribe" : "subscribe?candidates=on-answer"), "");
    v.subscribeMs = msSince(t0);
    if (!sub || sub->status != 200) {
        v.error = sub ? "subscribe HTTP " + std::to_string(sub->status) + ": " + sub->body
                      : "subscribe failed";
        return false;
    }
    auto offer = json::parse(sub->body, nullptr, false);
    if (offer.is_discarded()) {
        v.error = "subscribe returned bad JSON";
        return false;
    }

    v.pc = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
    auto* vp = &v;
    v.pc->onStateChange([vp](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Connected) vp->connected = true;
    });
    v.pc->onTrack([vp, t0](std::shared_ptr<rtc::Track> track) {
        bool isVideo = track->description().type() == "video";
        (isVideo ? vp->video : vp->audio) = track;
        // Sends receiver reports, as a browser does, and hands RTP only
        // (no RTCP) to onMessage.
        track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        if (!isVideo) {
            track->onMessage(
                [vp](rtc::binary msg) {
                    if (msg.size() < 12) return;
                    ++vp->audioPackets;
                    vp->audioBytes += msg.size();
                },
                nullptr);
            return;
        }
        track->onMessage(
            [vp, t0](rtc::binary msg) {
                if (msg.size() < 12) return;
                vp->bytes += msg.size();
                if (startsIdr(msg)) vp->frameHasIdr = true;
                // Marker bit: the last packet of a frame.
                if (std::to_integer<uint8_t>(msg[1]) & 0x80) {
                    if (vp->firstFrameMs < 0) vp->firstFrameMs = msSince(t0);
                    ++vp->frames;
                    if (vp->frameHasIdr) {
                        if (vp->firstKeyframeMs < 0) vp->firstKeyframeMs = msSince(t0);
                        ++vp->keyframes;
                    }
                    vp->frameHasIdr = false;
                }
            },
            nullptr);
    });

    // Shared with the callback: after a timeout below, gathering can still
    // complete once this function has returned.
    auto gathered = std::make_shared<std::promise<void>>();
    auto gatheredFuture = gathered->get_future();
    auto once = std::make_shared<std::once_flag>();
    v.pc->onGatheringStateChange([gathered, once](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete)
            std::call_once(*once, [&gathered] { gathered->set_value(); });
    });
    try {
        v.pc->setRemoteDescription(rtc::Description(offer.value("sdp", ""), "offer"));
    } catch (const std::exception& e) {
        v.error = std::string("bad offer: ") + e.what();
        return false;
    }
    if (gatheredFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        v.error = "ICE gathering timed out";
        return false;
    }
    auto local = v.pc->localDescription();
    if (!local) {
        v.error = "no local answer";
        return false;
    }
    if (o.answerDelayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(o.answerDelayMs));
    json answer{{"subscriberId", offer.value("subscriberId", "")}, {"sdp", std::string(*local)}};
    auto ans = httpRequest(o, "POST", base + "answer", answer.dump());
    if (!ans || ans->status != 200) {
        v.error = ans ? "answer HTTP " + std::to_string(ans->status) : "answer failed";
        return false;
    }
    // The server's candidates, now that it has our answer.
    auto reply = json::parse(ans->body, nullptr, false);
    if (!reply.is_discarded() && reply.contains("candidates")) {
        for (const auto& c : reply["candidates"]) {
            try {
                v.pc->addRemoteCandidate(rtc::Candidate(c.value("candidate", ""), c.value("mid", "")));
            } catch (const std::exception& e) {
                v.error = std::string("bad candidate: ") + e.what();
                return false;
            }
        }
    }
    return true;
}

json report(const Viewer& v, double seconds) {
    json j{{"frames", v.frames - v.framesAtStart},
           {"fps", static_cast<double>(v.frames - v.framesAtStart) / seconds},
           {"mbps", static_cast<double>(v.bytes - v.bytesAtStart) * 8 / seconds / 1e6},
           {"first_frame_ms", v.firstFrameMs.load()},
           {"keyframes", v.keyframes - v.keyframesAtStart},
           {"first_keyframe_ms", v.firstKeyframeMs.load()},
           {"audio_packets", v.audioPackets - v.audioPacketsAtStart},
           {"audio_kbps", static_cast<double>(v.audioBytes - v.audioBytesAtStart) * 8 / seconds / 1e3}};
    // A viewer's thread writes `error` only before setting `done`.
    if (v.done && !v.error.empty()) j["error"] = v.error;
    return j;
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        auto val = [&a](std::string_view key) -> std::optional<std::string> {
            if (a.substr(0, key.size()) == key) return std::string(a.substr(key.size()));
            return std::nullopt;
        };
        try {
            if (auto v = val("--host=")) o.host = *v;
            else if (auto v = val("--port=")) o.port = std::stoi(*v);
            else if (auto v = val("--token=")) o.token = *v;
            else if (auto v = val("--target=")) o.target = std::stol(*v);
            else if (auto v = val("--mjpeg=")) o.mjpeg = std::stoi(*v);
            else if (auto v = val("--webrtc=")) o.webrtc = std::stoi(*v);
            else if (auto v = val("--warmup=")) o.warmup = std::stod(*v);
            else if (auto v = val("--seconds=")) o.seconds = std::stod(*v);
            else if (a == "--sync") o.sync = true;
            else if (auto v = val("--answer-delay-ms=")) o.answerDelayMs = std::stoi(*v);
            else if (a == "--legacy-signaling") o.legacySignaling = true;
            else if (a == "--tls") o.tls = true;
            else {
                std::cerr << "Unknown option: " << a << "\n";
                return std::nullopt;
            }
        } catch (const std::exception&) {
            std::cerr << "Bad value in " << a << "\n";
            return std::nullopt;
        }
    }
    if (o.token.empty() || o.target <= 0 || o.seconds <= 0) {
        std::cerr << "Usage: HoustonKVM-loadgen --port=P --token=T --target=ID [--mjpeg=N] "
                     "[--webrtc=N] [--warmup=S] [--seconds=S] [--host=H] [--sync]\n"
                     "                          [--answer-delay-ms=MS] [--legacy-signaling] [--tls]\n";
        return std::nullopt;
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    auto opts = parseArgs(argc, argv);
    if (!opts) return 2;
    const Options& o = *opts;
    // To stderr: stdout carries only the JSON lines loadtest.py reads, and
    // libdatachannel's own logger would write to it.
    rtc::InitLogger(rtc::LogLevel::Error, [](rtc::LogLevel, std::string message) {
        std::cerr << "libdatachannel: " << message << std::endl;
    });

    if (o.tls) {
        std::signal(SIGPIPE, SIG_IGN);   // SSL_write can't pass MSG_NOSIGNAL
        tlsContext = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(tlsContext, SSL_VERIFY_NONE, nullptr);
    }

    std::atomic<bool> stop{false};
    std::vector<std::unique_ptr<Viewer>> mjpeg;
    std::vector<std::unique_ptr<std::atomic<int>>> mjpegFds;
    std::vector<std::thread> threads;
    for (int i = 0; i < o.mjpeg; ++i) {
        mjpeg.push_back(std::make_unique<Viewer>());
        mjpegFds.push_back(std::make_unique<std::atomic<int>>(-1));
        threads.emplace_back(runMjpeg, std::cref(o), std::ref(*mjpeg.back()), std::cref(stop),
                             std::ref(*mjpegFds.back()));
    }

    // Subscribed one after another, the way people arrive; the server does
    // each subscription's ICE gathering while the request waits.
    std::vector<std::unique_ptr<WebrtcViewer>> webrtc;
    auto setupStart = Clock::now();
    for (int i = 0; i < o.webrtc; ++i) {
        webrtc.push_back(std::make_unique<WebrtcViewer>());
        if (!startWebrtc(o, *webrtc.back())) webrtc.back()->done = true;
    }
    double setupMs = msSince(setupStart);

    std::this_thread::sleep_for(std::chrono::duration<double>(o.warmup));
    if (o.sync) {
        std::cout << R"({"event":"ready"})" << std::endl;
        std::string go;
        std::getline(std::cin, go);
    }
    for (auto& v : mjpeg) v->framesAtStart = v->frames, v->bytesAtStart = v->bytes;
    for (auto& v : webrtc)
        v->framesAtStart = v->frames, v->bytesAtStart = v->bytes, v->keyframesAtStart = v->keyframes,
        v->audioPacketsAtStart = v->audioPackets, v->audioBytesAtStart = v->audioBytes;
    std::this_thread::sleep_for(std::chrono::duration<double>(o.seconds));

    json out{{"seconds", o.seconds}, {"webrtc_setup_ms", setupMs},
             {"mjpeg", json::array()}, {"webrtc", json::array()}};
    for (auto& v : mjpeg) out["mjpeg"].push_back(report(*v, o.seconds));
    for (auto& v : webrtc) {
        auto j = report(*v, o.seconds);
        j["connected"]    = v->connected.load();
        j["subscribe_ms"] = v->subscribeMs;
        out["webrtc"].push_back(j);
    }
    std::cout << out.dump() << std::endl;

    stop = true;
    for (auto& fd : mjpegFds)
        if (int f = *fd; f >= 0) shutdown(f, SHUT_RDWR);
    for (auto& t : threads) t.join();
    for (auto& v : webrtc)
        if (v->pc) v->pc->close();
    webrtc.clear();
    rtc::Cleanup().wait_for(std::chrono::seconds(5));
    if (tlsContext) SSL_CTX_free(tlsContext);
    return 0;
}

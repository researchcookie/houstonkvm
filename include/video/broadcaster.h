#pragma once

#include <App.h>

#include <linux/sockios.h>
#include <sys/ioctl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace houston_kvm {

// Fans out MJPEG frames to every connected /api/stream client. onFrame() is
// called from the V4L2 capture thread; it defers the actual write to each
// client onto the uWS event-loop thread, since uWS::HttpResponse must only
// be touched from that thread.
//
// Must be owned by a shared_ptr: each deferred write keeps its Broadcaster
// alive until it has run, so a target being torn down (whose capture thread
// may have queued frames the event loop hasn't reached yet) can drop its own
// reference immediately without those writes touching freed memory.
//
// Two ways of framing the stream. Plain: each part is the boundary, headers
// with a Content-Length, and the JPEG, for any multipart parser. Early
// headers (?headers=early, what the web UI asks for): the next part's
// boundary and headers go out right after each JPEG, so with no
// Content-Length. Chromium shows a part only once the next part's headers
// have arrived, so in an <img> the plain stream shows every frame a frame
// late, and the last one before a still screen (whose frames are
// deduplicated, see StreamManager::handleFrame) a couple of seconds late.
struct Broadcaster : std::enable_shared_from_this<Broadcaster> {
    // One frame, laid out so that each framing is a slice of it:
    //   --frame CRLF Content-Type CRLF Content-Length CRLF CRLF <jpeg>
    //   CRLF --frame CRLF Content-Type CRLF CRLF
    // Plain: from the start to the CRLF after the JPEG. Early headers: from
    // the JPEG to the end.
    struct Frame {
        std::string bytes;
        size_t      jpegAt   = 0;
        size_t      jpegSize = 0;
    };

    uWS::Loop*                   loop = nullptr;
    // The last frame, for a viewer who connects between frames. Event-loop
    // thread only.
    std::shared_ptr<const Frame> latest;

    // Plain HTTP and HTTPS viewers share one list, so each client carries the
    // functions for its own kind of response. An early-headers stream opens
    // with the first part's boundary and headers. A new viewer gets the
    // latest frame at once: a still screen's frames are only resent every
    // couple of seconds.
    template <bool SSL>
    void add(uWS::HttpResponse<SSL>* res, bool earlyHeaders) {
        using Res = uWS::HttpResponse<SSL>;
        clients_.push_back(Client{
            res,
            [](void* r) { return hasUnsentData(static_cast<Res*>(r)); },
            [](void* r, const Frame& frame, bool early) { writeFrame(static_cast<Res*>(r), frame, early); },
            [](void* r) { static_cast<Res*>(r)->close(); },
            earlyHeaders,
        });
        if (earlyHeaders) res->write(kNextPart.substr(2));
        if (latest) writeFrame(res, *latest, earlyHeaders);
    }

    void remove(const void* res) {
        std::erase_if(clients_, [res](const Client& c) { return c.res == res; });
    }

    size_t viewers() const { return clients_.size(); }

    // Force-closes every connected /api/stream client. Used during shutdown:
    // these connections are held open indefinitely, so without this the uWS
    // event loop would never run out of active sockets and run() would never
    // return. Taken out of the list first: closing one runs its onAborted,
    // which removes it.
    void closeAll() {
        auto clients = std::exchange(clients_, {});
        for (const auto& c : clients) c.close(c.res);
    }

    // Laid out once here, on the capture thread, and shared by every viewer.
    void onFrame(const uint8_t* data, size_t size) {
        if (!loop) return;
        auto frame = std::make_shared<const Frame>(layOut(data, size));
        loop->defer([self = shared_from_this(), frame = std::move(frame)]() {
            self->latest = frame;
            for (const auto& c : self->clients_)
                if (!c.hasUnsentData(c.res)) c.writeFrame(c.res, *frame, c.early);
        });
    }

    static Frame layOut(const uint8_t* data, size_t size) {
        std::string headers = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                              std::to_string(size) + "\r\n\r\n";
        Frame f;
        f.bytes.reserve(headers.size() + size + kNextPart.size());
        f.bytes += headers;
        f.bytes.append(reinterpret_cast<const char*>(data), size);
        f.bytes += kNextPart;
        f.jpegAt   = headers.size();
        f.jpegSize = size;
        return f;
    }

    // True while any of the previous frame is still waiting to go out, either
    // in uWS's own backpressure buffer or in the kernel's send queue. A client
    // that can't keep up then skips frames instead of queueing them: a queue
    // only ever grows on a slow link, and every frame in it is added delay.
    // The kernel queue matters as much as uWS's: with send-buffer autotuning
    // it can hold several megabytes, i.e. seconds of video, before uWS ever
    // sees backpressure.
    template <bool SSL>
    static bool hasUnsentData(uWS::HttpResponse<SSL>* res) {
        if ((res->*Access<SSL>::buffered)() > 0) return true;
        // The fd, asked for as a plain socket even under TLS: a TLS socket
        // starts with its plain one, and asked for as TLS it gives its SSL*.
        auto* socket = reinterpret_cast<us_socket_t*>(res);
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(us_socket_get_native_handle(0, socket)));
        int unsent = 0;
        return ioctl(fd, SIOCOUTQNSD, &unsent) == 0 && unsent > 0;
    }

    // Corked, the chunk header uWS puts in front of each write leaves with
    // the frame, in two sends (the first flagged MSG_MORE); uncorked, as it
    // is outside a request handler, every write is four sends of its own
    // (CRLF, chunk size, CRLF, data), each mostly a tiny TCP packet.
    template <bool SSL>
    static void writeFrame(uWS::HttpResponse<SSL>* res, const Frame& frame, bool early) {
        std::string_view bytes(frame.bytes);
        auto part = early ? bytes.substr(frame.jpegAt)
                          : bytes.substr(0, frame.jpegAt + frame.jpegSize + 2);
        res->cork([res, part] { res->write(part); });
    }

private:
    // What ends a frame: the CRLF before the next boundary, then (for
    // early headers) that boundary and the next part's headers.
    static constexpr std::string_view kNextPart = "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n";

    struct Client {
        void* res;
        bool (*hasUnsentData)(void*);
        void (*writeFrame)(void*, const Frame&, bool early);
        void (*close)(void*);
        bool early;   // ?headers=early
    };
    std::vector<Client> clients_;

    // getBufferedAmount is protected in uWS; naming it through a derived
    // class yields an ordinary pointer-to-member callable on any response.
    template <bool SSL>
    struct Access : uWS::HttpResponse<SSL> {
        static constexpr auto buffered = &Access::getBufferedAmount;
    };
};

} // namespace houston_kvm

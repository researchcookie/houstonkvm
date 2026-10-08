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
struct Broadcaster : std::enable_shared_from_this<Broadcaster> {
    uWS::Loop*              loop    = nullptr;
    std::vector<uint8_t>    latest;

    // Plain HTTP and HTTPS viewers share one list, so each client carries the
    // functions for its own kind of response.
    template <bool SSL>
    void add(uWS::HttpResponse<SSL>* res) {
        using Res = uWS::HttpResponse<SSL>;
        clients_.push_back(Client{
            res,
            [](void* r) { return hasUnsentData(static_cast<Res*>(r)); },
            [](void* r, const std::vector<uint8_t>& frame) { writeFrame(static_cast<Res*>(r), frame); },
            [](void* r) { static_cast<Res*>(r)->close(); },
        });
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

    void onFrame(const uint8_t* data, size_t size) {
        if (!loop) return;
        auto frame = std::make_shared<std::vector<uint8_t>>(data, data + size);
        loop->defer([self = shared_from_this(), frame]() {
            self->latest = *frame;
            for (const auto& c : self->clients_)
                if (!c.hasUnsentData(c.res)) c.writeFrame(c.res, *frame);
        });
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

    template <bool SSL>
    static void writeFrame(uWS::HttpResponse<SSL>* res,
                           const std::vector<uint8_t>& frame) {
        std::string hdr =
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
            std::to_string(frame.size()) + "\r\n\r\n";
        res->write(std::string_view(hdr));
        res->write(std::string_view(
            reinterpret_cast<const char*>(frame.data()), frame.size()));
        res->write(std::string_view("\r\n"));
    }

private:
    struct Client {
        void* res;
        bool (*hasUnsentData)(void*);
        void (*writeFrame)(void*, const std::vector<uint8_t>&);
        void (*close)(void*);
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

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace houston_kvm {

// A STUN or TURN server for low-latency video. Used on both ends: the
// server's connections to viewers, and each viewer's browser (sent with
// every WebRTC subscription), so the two always agree.
//
// None by default. On a LAN or VPN the server's own addresses are all a
// browser needs, and a server that reaches out to a public STUN server by
// itself is the first thing a network review flags. An Owner adds one in
// Admin -> Network, or the server's configuration pins the list with
// --ice-server.
struct IceServerConfig {
    std::string url;          // stun:HOST[:PORT], turn:HOST[:PORT][?transport=udp|tcp], turns:...
    std::string username;     // TURN only
    std::string credential;   // TURN only

    bool operator==(const IceServerConfig&) const = default;
};

inline constexpr size_t kMaxIceServers = 8;

// "" when the server is usable, else what's wrong with it, worded for the
// person who entered it.
std::string validateIceServer(const IceServerConfig& server);

// One --ice-server value: stun:HOST[:PORT] or turn[s]:USER:PASS@HOST[:PORT].
// The credentials come out of the URL, since browsers only accept them
// separately. nullopt with `error` set when it doesn't parse.
std::optional<IceServerConfig> parseIceServerArg(std::string_view arg, std::string& error);

// Stored form and API form: [{"url", "username", "credential"}].
nlohmann::json iceServersToJson(const std::vector<IceServerConfig>& servers);
std::optional<std::vector<IceServerConfig>> iceServersFromJson(const nlohmann::json& j,
                                                               std::string& error);

// What a browser's RTCPeerConnection takes: [{"urls", "username", "credential"}].
nlohmann::json iceServersForBrowser(const std::vector<IceServerConfig>& servers);

} // namespace houston_kvm

#include "webrtc/ice_servers.h"

#include <cctype>
#include <charconv>

namespace houston_kvm {

namespace {

constexpr size_t kMaxFieldLength = 255;

bool printable(std::string_view s) {
    for (unsigned char c : s)
        if (c < 0x21 || c == 0x7f) return false;   // no spaces or control characters
    return true;
}

// HOST[:PORT], HOST a name, an IPv4 address or a [bracketed] IPv6 address.
bool validHostPort(std::string_view hp) {
    std::string_view port;
    bool hasPort = false;
    if (hp.starts_with('[')) {
        auto close = hp.find(']');
        if (close == std::string_view::npos || close == 1) return false;
        for (char c : hp.substr(1, close - 1))
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return false;
        auto rest = hp.substr(close + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':')) return false;
            port = rest.substr(1);
            hasPort = true;
        }
    } else {
        auto colon = hp.find(':');
        auto host = hp.substr(0, colon);
        if (host.empty()) return false;
        for (char c : host)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
        if (colon != std::string_view::npos) {
            port = hp.substr(colon + 1);
            hasPort = true;
        }
    }
    if (!hasPort) return true;
    int value = 0;
    auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
    return !port.empty() && ec == std::errc{} && ptr == port.data() + port.size() &&
           value >= 1 && value <= 65535;
}

} // namespace

std::string validateIceServer(const IceServerConfig& s) {
    if (s.url.size() > kMaxFieldLength || s.username.size() > kMaxFieldLength ||
        s.credential.size() > kMaxFieldLength)
        return "Too long (at most 255 characters each)";
    if (!printable(s.url) || !printable(s.username) || !printable(s.credential))
        return "No spaces or control characters, please";

    std::string_view url = s.url;
    bool turn = false;
    if (url.starts_with("stun:")) url.remove_prefix(5);
    else if (url.starts_with("turn:")) { url.remove_prefix(5); turn = true; }
    else if (url.starts_with("turns:")) { url.remove_prefix(6); turn = true; }
    else return "Must start with stun:, turn: or turns:";

    if (auto q = url.find('?'); q != std::string_view::npos) {
        auto query = url.substr(q + 1);
        if (!turn || (query != "transport=udp" && query != "transport=tcp"))
            return "Only a TURN server takes ?transport=udp or ?transport=tcp";
        url = url.substr(0, q);
    }
    if (url.find('@') != std::string_view::npos)
        return "Give the username and password in their own fields, not in the address";
    if (!validHostPort(url)) return "Expected HOST or HOST:PORT after the scheme";

    if (turn && (s.username.empty() || s.credential.empty()))
        return "A TURN server needs a username and password";
    if (!turn && (!s.username.empty() || !s.credential.empty()))
        return "A STUN server takes no username or password";
    return "";
}

std::optional<IceServerConfig> parseIceServerArg(std::string_view arg, std::string& error) {
    IceServerConfig s;
    auto scheme = arg.find(':');
    auto at = arg.rfind('@');
    if (scheme != std::string_view::npos && at != std::string_view::npos && at > scheme) {
        // turn:USER:PASS@HOST — the password may itself contain ':' or '@'.
        auto userinfo = arg.substr(scheme + 1, at - scheme - 1);
        auto colon = userinfo.find(':');
        if (colon == std::string_view::npos) {
            error = "expected turn:USER:PASS@HOST[:PORT]";
            return std::nullopt;
        }
        s.url = std::string(arg.substr(0, scheme + 1)) + std::string(arg.substr(at + 1));
        s.username = std::string(userinfo.substr(0, colon));
        s.credential = std::string(userinfo.substr(colon + 1));
    } else {
        s.url = std::string(arg);
    }
    error = validateIceServer(s);
    if (!error.empty()) return std::nullopt;
    return s;
}

nlohmann::json iceServersToJson(const std::vector<IceServerConfig>& servers) {
    auto out = nlohmann::json::array();
    for (const auto& s : servers)
        out.push_back({{"url", s.url}, {"username", s.username}, {"credential", s.credential}});
    return out;
}

std::optional<std::vector<IceServerConfig>> iceServersFromJson(const nlohmann::json& j,
                                                               std::string& error) {
    if (!j.is_array()) { error = "Expected a list of servers"; return std::nullopt; }
    if (j.size() > kMaxIceServers) {
        error = "At most " + std::to_string(kMaxIceServers) + " servers";
        return std::nullopt;
    }
    std::vector<IceServerConfig> out;
    for (const auto& item : j) {
        if (!item.is_object()) { error = "Each server must be an object"; return std::nullopt; }
        IceServerConfig s;
        for (auto [key, field] : {std::pair{"url", &s.url}, std::pair{"username", &s.username},
                                  std::pair{"credential", &s.credential}}) {
            auto it = item.find(key);
            if (it == item.end()) continue;
            if (!it->is_string()) { error = std::string(key) + " must be text"; return std::nullopt; }
            *field = it->get<std::string>();
        }
        if (auto e = validateIceServer(s); !e.empty()) {
            error = (s.url.empty() ? std::string("A server") : s.url) + ": " + e;
            return std::nullopt;
        }
        for (const auto& seen : out)
            if (seen.url == s.url) { error = s.url + " is listed twice"; return std::nullopt; }
        out.push_back(std::move(s));
    }
    return out;
}

nlohmann::json iceServersForBrowser(const std::vector<IceServerConfig>& servers) {
    auto out = nlohmann::json::array();
    for (const auto& s : servers) {
        nlohmann::json entry = {{"urls", s.url}};
        if (!s.username.empty()) {
            entry["username"] = s.username;
            entry["credential"] = s.credential;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace houston_kvm

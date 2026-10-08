#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/events.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace houston_kvm {

using json = nlohmann::json;

inline std::string getCookie(std::string_view header, std::string_view name) {
    std::string search = std::string(name) + "=";
    size_t pos = 0;
    while ((pos = header.find(search, pos)) != std::string_view::npos) {
        bool atBoundary = pos == 0 ||
            (pos >= 2 && header[pos - 1] == ' ' && header[pos - 2] == ';');
        if (atBoundary) {
            auto start = pos + search.size();
            auto end   = header.find(';', start);
            return std::string(
                header.substr(start, end == std::string_view::npos
                                     ? std::string_view::npos : end - start));
        }
        pos += search.size();
    }
    return {};
}

// Whether an If-None-Match header names this ETag. The header is "*" or a
// comma-separated list of tags, compared weakly (RFC 9110 §13.1.2), so a W/
// prefix added by a proxy still matches.
inline bool etagListMatches(std::string_view header, std::string_view etag) {
    while (!header.empty()) {
        const size_t comma = header.find(',');
        std::string_view tag = header.substr(0, comma);
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
        if (tag.starts_with("W/")) tag.remove_prefix(2);
        if (tag == "*" || tag == etag) return true;
        if (comma == std::string_view::npos) break;
        header.remove_prefix(comma + 1);
    }
    return false;
}

template <typename T>
inline std::optional<T> jsonValueOr(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) return std::nullopt;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) return std::nullopt;
    } else if constexpr (std::is_integral_v<T>) {
        if (!it->is_number_integer()) return std::nullopt;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!it->is_number()) return std::nullopt;
    }
    return it->template get<T>();
}

// Accumulates a request body across onData chunks (rejecting anything over
// maxBytes with 413) and invokes onComplete once the last chunk has
// arrived. Callers must do any req->getHeader()/getParameter()/
// res->getRemoteAddressAsText() reads *before* calling this — those become
// invalid once the handler returns, before onData's async callback fires.
template <bool SSL>
inline void readBody(uWS::HttpResponse<SSL>* res, size_t maxBytes,
                      std::function<void(std::string&)> onComplete) {
    auto body = std::make_shared<std::string>();
    res->onData([res, body, maxBytes, onComplete = std::move(onComplete)]
                (std::string_view chunk, bool last) mutable {
        if (body->size() + chunk.size() > maxBytes) {
            res->writeStatus("413 Payload Too Large")->end();
            return;
        }
        body->append(chunk);
        if (!last) return;
        onComplete(*body);
    });
    res->onAborted([](){});
}

template <bool SSL>
inline std::optional<json> parseJsonOr400(uWS::HttpResponse<SSL>* res, const std::string& body) {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        res->writeStatus("400 Bad Request")->end("Invalid JSON");
        return std::nullopt;
    }
    return j;
}

inline std::optional<int64_t> parseId(std::string_view param) {
    int64_t value;
    auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), value);
    if (ec != std::errc{} || ptr != param.data() + param.size()) return std::nullopt;
    return value;
}

// The session cookie's name. Over HTTPS it is __Host-session, which
// browsers only accept when set Secure by this exact host over HTTPS: a
// cookie that crossed the network in the clear on the plain-HTTP side is
// never honoured there, so switching to HTTPS signs everyone in once more.
template <bool SSL>
inline constexpr const char* kSessionCookie = SSL ? "__Host-session" : "session";

// `secure` (--secure-cookies, for TLS ended by a proxy in front) only
// matters on plain HTTP: over HTTPS the cookie is always Secure.
template <bool SSL>
inline std::string sessionCookieHeader(const std::string& token, int maxAgeSeconds, bool secure) {
    std::string h = std::string(kSessionCookie<SSL>) + "=" + token +
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(maxAgeSeconds);
    if (SSL || secure) h += "; Secure";
    return h;
}

// Browsers attach the session cookie to a request whatever page started it.
// SameSite=Strict keeps other *sites* out, but another port or subdomain of
// the same site (another homelab app on this box, say) still gets the
// cookie sent — so cookie-authenticated WebSocket upgrades and state-changing
// requests must also come from our own origin. Browsers always send Origin
// on those, and a page's script can't forge it; clients that send no Origin
// aren't browsers, so there's no ambient cookie to abuse, and they pass.
// Over HTTPS (`https`) the page must be an https:// one too.
inline bool isSameOrigin(const RequestPolicy& policy, uWS::HttpRequest* req, bool https = false) {
    auto origin = req->getHeader("origin");
    if (origin.empty()) return true;
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    auto o = lower(origin);
    for (const auto& allowed : policy.allowedOrigins)
        if (o == lower(allowed)) return true;
    auto sep = o.find("://");
    if (sep == std::string::npos) return false; // includes "null" (sandboxed frames, file://)
    if (https && !o.starts_with("https://")) return false;
    auto host = lower(req->getHeader("host"));
    return !host.empty() && o.compare(sep + 3, std::string::npos, host) == 0;
}

template <bool SSL>
inline bool sameOriginOr403(const RequestPolicy& policy,
                            uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    if (isSameOrigin(policy, req, SSL)) return true;
    res->writeStatus("403 Forbidden")
       ->end("Cross-origin request refused (behind a proxy that rewrites Host? see --allowed-origin)");
    return false;
}

// The address a request came from: the peer, unless the peer is a trusted
// proxy, in which case the nearest X-Forwarded-For hop that isn't one.
// Hops further left were written by whoever sent the request and can't be
// believed. Must be read before readBody() (see there).
template <bool SSL>
inline std::string clientAddress(const RequestPolicy& policy,
                                 uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto normalize = [](std::string_view a) {
        // An IPv4 client on a dual-stack socket shows up as ::ffff:a.b.c.d.
        if (a.starts_with("::ffff:") && a.find('.') != std::string_view::npos) a.remove_prefix(7);
        return std::string(a);
    };
    auto trusted = [&policy](const std::string& a) {
        for (const auto& p : policy.trustedProxies)
            if (a == p) return true;
        return false;
    };
    auto addr = normalize(res->getRemoteAddressAsText());
    if (!trusted(addr)) return addr;
    std::string_view xff = req->getHeader("x-forwarded-for");
    while (!xff.empty()) {
        auto comma = xff.rfind(',');
        auto hop = comma == std::string_view::npos ? xff : xff.substr(comma + 1);
        xff = comma == std::string_view::npos ? std::string_view{} : xff.substr(0, comma);
        while (!hop.empty() && hop.front() == ' ') hop.remove_prefix(1);
        while (!hop.empty() && hop.back() == ' ') hop.remove_suffix(1);
        if (hop.empty()) continue;
        addr = normalize(hop);
        if (!trusted(addr)) return addr;
    }
    return addr;
}

// The Host header without its port, lowercased: a name, an IPv4 address or a
// [bracketed] IPv6 address. "" when it's missing or anything else, so what
// comes back is safe to put in a URL.
inline std::string requestHost(uWS::HttpRequest* req) {
    std::string_view h = req->getHeader("host");
    std::string_view host, port;
    if (h.starts_with('[')) {
        auto close = h.find(']');
        if (close == std::string_view::npos || close == 1) return {};
        for (char c : h.substr(1, close - 1))
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return {};
        host = h.substr(0, close + 1);
        port = h.substr(close + 1);
    } else {
        auto colon = h.find(':');
        host = h.substr(0, colon);
        port = colon == std::string_view::npos ? std::string_view{} : h.substr(colon);
        if (host.empty()) return {};
        for (char c : host)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return {};
    }
    if (!port.empty()) {
        if (port.size() < 2 || port[0] != ':') return {};
        for (char c : port.substr(1))
            if (!std::isdigit(static_cast<unsigned char>(c))) return {};
    }
    std::string out(host);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool isValidUsername(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            return false;
    return true;
}

struct AuthResult {
    int64_t userId;
    std::string token;
    Role role;
    std::string username;
    bool viaToken = false;   // an API token, not a browser session
    TokenScope scope = TokenScope::Full;
};

// The audit log's view of an authenticated caller. `ip` is clientAddress(),
// read before any readBody().
inline Actor actorOf(const AuthResult& a, std::string ip) {
    return Actor{a.userId, a.username, std::move(ip), a.viaToken};
}

// Pulls the bearer/session token out of a request with no DB lookup. Used by
// routes that can compare it against already-validated in-memory state (e.g.
// TargetRuntime::driverToken) instead of paying a DB round trip on every call
// — see requireExactRole()'s caller in routes_input.cpp for the fast path
// this enables on the high-frequency /api/input route.
template <bool SSL>
inline std::string extractToken(uWS::HttpRequest* req) {
    static constexpr std::string_view kBearerPrefix = "Bearer ";
    auto authHeader = req->getHeader("authorization");
    if (authHeader.substr(0, kBearerPrefix.size()) == kBearerPrefix)
        return std::string(authHeader.substr(kBearerPrefix.size()));
    return getCookie(req->getHeader("cookie"), kSessionCookie<SSL>);
}

// Requests the Origin check applies to when they carry the session cookie:
// anything that can change state, and WebSocket upgrades (GETs that open a
// channel which can).
inline bool needsOriginCheck(uWS::HttpRequest* req) {
    auto m = req->getCaseSensitiveMethod();
    return !(m == "GET" || m == "HEAD") || !req->getHeader("upgrade").empty();
}

// Every route but GET /metrics: a metrics-scoped token is refused here, so
// no other route has to know that scope exists. A read-scoped token arrives
// with its role already lowered to Viewer, which the role checks below
// enforce.
template <bool SSL>
inline std::optional<AuthResult> authenticate(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req,
        bool metricsRoute = false) {
    static constexpr std::string_view kBearerPrefix = "Bearer ";
    auto authHeader = req->getHeader("authorization");
    if (authHeader.substr(0, kBearerPrefix.size()) == kBearerPrefix) {
        std::string token(authHeader.substr(kBearerPrefix.size()));
        DbOutcome outcome;
        auto u = auth.verifyApiToken(db, token, outcome);
        if (!outcome.ok) {
            std::cerr << "Auth DB error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
            res->writeStatus("500 Internal Server Error")->end("Server error");
            return std::nullopt;
        }
        if (!u) {
            res->writeStatus("401 Unauthorized")->end("Invalid or expired API token");
            return std::nullopt;
        }
        if (u->scope == TokenScope::Metrics && !metricsRoute) {
            res->writeStatus("403 Forbidden")->end("This API token can only read /metrics");
            return std::nullopt;
        }
        return AuthResult{u->userId, std::move(token), u->role, u->username, true, u->scope};
    }

    auto token = getCookie(req->getHeader("cookie"), kSessionCookie<SSL>);
    if (token.empty()) {
        res->writeStatus("401 Unauthorized")->end("Not authenticated");
        return std::nullopt;
    }
    if (needsOriginCheck(req) && !sameOriginOr403(auth.policy, res, req)) return std::nullopt;
    DbOutcome outcome;
    auto u = auth.verifySession(db, token, outcome);
    if (!outcome.ok) {
        std::cerr << "Auth DB error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return std::nullopt;
    }
    if (!u) {
        res->writeStatus("401 Unauthorized")->end("Session expired");
        return std::nullopt;
    }
    return AuthResult{u->userId, std::move(token), u->role, u->username};
}

template <bool SSL>
inline std::optional<int64_t> requireAuth(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto r = authenticate(auth, db, res, req);
    if (!r) return std::nullopt;
    return r->userId;
}

template <bool SSL>
inline std::optional<AuthResult> browserAuthenticate(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto token = getCookie(req->getHeader("cookie"), kSessionCookie<SSL>);
    if (token.empty()) {
        res->writeStatus("401 Unauthorized")->end("Not authenticated");
        return std::nullopt;
    }
    if (needsOriginCheck(req) && !sameOriginOr403(auth.policy, res, req)) return std::nullopt;
    DbOutcome outcome;
    auto u = auth.verifySession(db, token, outcome);
    if (!outcome.ok) {
        std::cerr << "Auth DB error [sqlite:" << outcome.sqliteCode << "]: " << outcome.message << "\n";
        res->writeStatus("500 Internal Server Error")->end("Server error");
        return std::nullopt;
    }
    if (!u) {
        res->writeStatus("401 Unauthorized")->end("Session expired");
        return std::nullopt;
    }
    return AuthResult{u->userId, std::move(token), u->role, u->username};
}

template <bool SSL>
inline std::optional<int64_t> requireBrowserAuth(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
    auto r = browserAuthenticate(auth, db, res, req);
    if (!r) return std::nullopt;
    return r->userId;
}

template <bool SSL>
inline std::optional<AuthResult> requireRole(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req,
        Role minRole) {
    auto r = authenticate(auth, db, res, req);
    if (!r) return std::nullopt;
    if (r->role < minRole) {
        res->writeStatus("403 Forbidden")->end("Insufficient permissions");
        return std::nullopt;
    }
    return r;
}

template <bool SSL>
inline std::optional<AuthResult> requireBrowserRole(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req,
        Role minRole) {
    auto r = browserAuthenticate(auth, db, res, req);
    if (!r) return std::nullopt;
    if (r->role < minRole) {
        res->writeStatus("403 Forbidden")->end("Insufficient permissions");
        return std::nullopt;
    }
    return r;
}

// Exact-role gate, not "at least" — used for control/input, where piloting
// the target is deliberately a separate capability from administering the
// server. requireRole()'s ordinal check would let Owner through here too
// (Owner > Operator), which is exactly what this avoids: an Owner who
// wants to drive the target needs a dedicated Operator account. Owner-only
// management routes are unaffected (requireBrowserRole(..., Role::Owner) is
// already an exact match in practice, since nothing outranks Owner).
template <bool SSL>
inline std::optional<AuthResult> requireExactRole(
        Auth& auth, Database& db,
        uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req,
        Role role) {
    auto r = authenticate(auth, db, res, req);
    if (!r) return std::nullopt;
    if (r->role != role) {
        res->writeStatus("403 Forbidden")->end("Insufficient permissions");
        return std::nullopt;
    }
    return r;
}

} // namespace houston_kvm

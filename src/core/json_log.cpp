#include "core/json_log.h"

#include <nlohmann/json.hpp>

#include <dlfcn.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <string_view>

namespace houston_kvm {

namespace {

std::mutex& writeMutex() {
    static std::mutex m;
    return m;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

// "InputQueue: ring buffer full" -> "InputQueue". One word of letters,
// digits, '_' or '-', followed by ": ".
std::string_view componentOf(std::string_view line) {
    auto colon = line.find(": ");
    if (colon == std::string_view::npos || colon == 0 || colon > 32) return {};
    for (char c : line.substr(0, colon))
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return {};
    return line.substr(0, colon);
}

// Collects what a thread writes until a newline, then writes that line to
// the real stream as JSON.
class JsonLineBuf : public std::streambuf {
public:
    JsonLineBuf(std::streambuf* sink, const char* level, int slot)
        : sink_(sink), level_(level), slot_(slot) {}

protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        char ch = traits_type::to_char_type(c);
        xsputn(&ch, 1);
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::string& pending = pendingFor(slot_);
        for (std::streamsize i = 0; i < n; ++i) {
            if (s[i] == '\n') {
                emit(pending);
                pending.clear();
            } else {
                pending += s[i];
            }
        }
        return n;
    }

    // A flush mid-line waits for the rest of the line.
    int sync() override { return 0; }

private:
    static std::string& pendingFor(int slot) {
        thread_local std::string pending[2];
        return pending[slot];
    }

    void emit(std::string_view line) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        // Leading blank lines in messages ("\nHoustonKVM received...").
        if (line.empty()) return;
        nlohmann::json j{{"ts", timestamp()}, {"level", level_}};
        auto component = componentOf(line);
        if (!component.empty()) j["component"] = component;
        j["msg"] = line;
        std::string out = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        std::lock_guard<std::mutex> lock(writeMutex());
        sink_->sputn(out.data(), static_cast<std::streamsize>(out.size()));
        sink_->pubsync();
    }

    std::streambuf* sink_;
    const char*     level_;
    int             slot_;
};

// GLib writes its warnings straight to stderr, bypassing std::cerr. EL9
// builds use EPEL's libdatachannel, which brings in libnice and with it
// GLib, and some pairings of the two warn as each WebRTC connection is set
// up. GLib's own writer is replaced with one that sends its messages
// through std::cerr, so they come out as JSON like everything else. GLib
// is looked up rather than linked: builds without it (EL10 bundles
// libdatachannel without libnice) skip this.
//
// The declarations below match GLib's stable C ABI (gmessages.h).
struct GLogFieldAbi {
    const char*    key;
    const void*    value;
    std::ptrdiff_t length;   // -1: value is a NUL-terminated string
};
using GLogWriterAbi = int (*)(int level, const GLogFieldAbi* fields, std::size_t n, void* data);
using SetWriterAbi  = void (*)(GLogWriterAbi, void* data, void (*destroy)(void*));
constexpr int kGLogLevelInfo  = 1 << 6;
constexpr int kGLogLevelDebug = 1 << 7;
constexpr int kGLogWriterHandled = 1;

std::string_view glibField(const GLogFieldAbi& f) {
    if (!f.value) return {};
    auto s = static_cast<const char*>(f.value);
    return f.length < 0 ? std::string_view(s) : std::string_view(s, static_cast<std::size_t>(f.length));
}

int glibToCerr(int level, const GLogFieldAbi* fields, std::size_t n, void*) {
    // As GLib's default writer: info and debug only with G_MESSAGES_DEBUG.
    if ((level & (kGLogLevelInfo | kGLogLevelDebug)) && !std::getenv("G_MESSAGES_DEBUG"))
        return kGLogWriterHandled;
    std::string_view domain, message;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::strcmp(fields[i].key, "GLIB_DOMAIN") == 0) domain = glibField(fields[i]);
        else if (std::strcmp(fields[i].key, "MESSAGE") == 0) message = glibField(fields[i]);
    }
    std::string line(domain.empty() ? "GLib" : domain);
    line += ": ";
    for (char c : message) line += c == '\n' ? ' ' : c;
    line += '\n';
    std::cerr << line;
    return kGLogWriterHandled;
}

void routeGlibLogs() {
    if (auto set = reinterpret_cast<SetWriterAbi>(dlsym(RTLD_DEFAULT, "g_log_set_writer_func")))
        set(glibToCerr, nullptr, nullptr);
}

} // namespace

void enableJsonLogs() {
    routeGlibLogs();
    // Never destroyed: other threads may still log during static destruction.
    static auto* out = new JsonLineBuf(std::cout.rdbuf(), "info", 0);
    static auto* err = new JsonLineBuf(std::cerr.rdbuf(), "warning", 1);
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
}

} // namespace houston_kvm

#include "hid/text_typing.h"

#include <cstdint>
#include <cstdio>
#include <utility>

namespace houston_kvm {

std::optional<TypedKey> usKeyFor(char c) {
    // Letters and digits by arithmetic; KeyA..KeyZ and Digit0..Digit9 are
    // static strings so TypedKey can hold a plain pointer.
    static const char* const kLetters[26] = {
        "KeyA","KeyB","KeyC","KeyD","KeyE","KeyF","KeyG","KeyH","KeyI","KeyJ","KeyK","KeyL","KeyM",
        "KeyN","KeyO","KeyP","KeyQ","KeyR","KeyS","KeyT","KeyU","KeyV","KeyW","KeyX","KeyY","KeyZ",
    };
    static const char* const kDigits[10] = {
        "Digit0","Digit1","Digit2","Digit3","Digit4","Digit5","Digit6","Digit7","Digit8","Digit9",
    };
    if (c >= 'a' && c <= 'z') return TypedKey{kLetters[c - 'a'], false};
    if (c >= 'A' && c <= 'Z') return TypedKey{kLetters[c - 'A'], true};
    if (c >= '0' && c <= '9') return TypedKey{kDigits[c - '0'], false};

    switch (c) {
        case '\n': return TypedKey{"Enter", false};
        case '\t': return TypedKey{"Tab", false};
        case ' ':  return TypedKey{"Space", false};
        case '-':  return TypedKey{"Minus", false};
        case '_':  return TypedKey{"Minus", true};
        case '=':  return TypedKey{"Equal", false};
        case '+':  return TypedKey{"Equal", true};
        case '[':  return TypedKey{"BracketLeft", false};
        case '{':  return TypedKey{"BracketLeft", true};
        case ']':  return TypedKey{"BracketRight", false};
        case '}':  return TypedKey{"BracketRight", true};
        case '\\': return TypedKey{"Backslash", false};
        case '|':  return TypedKey{"Backslash", true};
        case ';':  return TypedKey{"Semicolon", false};
        case ':':  return TypedKey{"Semicolon", true};
        case '\'': return TypedKey{"Quote", false};
        case '"':  return TypedKey{"Quote", true};
        case '`':  return TypedKey{"Backquote", false};
        case '~':  return TypedKey{"Backquote", true};
        case ',':  return TypedKey{"Comma", false};
        case '<':  return TypedKey{"Comma", true};
        case '.':  return TypedKey{"Period", false};
        case '>':  return TypedKey{"Period", true};
        case '/':  return TypedKey{"Slash", false};
        case '?':  return TypedKey{"Slash", true};
        case '!':  return TypedKey{"Digit1", true};
        case '@':  return TypedKey{"Digit2", true};
        case '#':  return TypedKey{"Digit3", true};
        case '$':  return TypedKey{"Digit4", true};
        case '%':  return TypedKey{"Digit5", true};
        case '^':  return TypedKey{"Digit6", true};
        case '&':  return TypedKey{"Digit7", true};
        case '*':  return TypedKey{"Digit8", true};
        case '(':  return TypedKey{"Digit9", true};
        case ')':  return TypedKey{"Digit0", true};
        default:   return std::nullopt;
    }
}

namespace {

// The code point starting at s[i], and how many bytes it takes. The JSON
// parser has already rejected invalid UTF-8, so this only has to be right
// for valid input; it's used just to name a character in an error.
std::pair<uint32_t, size_t> decodeUtf8(std::string_view s, size_t i) {
    auto b = static_cast<unsigned char>(s[i]);
    size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if (i + len > s.size()) return {b, 1};
    uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return {cp, len};
}

} // namespace

PreparedText prepareTextForTyping(std::string_view utf8) {
    PreparedText out;
    out.text.reserve(utf8.size());
    size_t charIndex = 0; // 1-based position in the error, counted in characters
    for (size_t i = 0; i < utf8.size();) {
        ++charIndex;
        char c = utf8[i];
        if (c == '\r') {
            out.text += '\n';
            i += (i + 1 < utf8.size() && utf8[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (usKeyFor(c)) {
            out.text += c;
            ++i;
            continue;
        }
        auto [cp, len] = decodeUtf8(utf8, i);
        char name[16];
        std::snprintf(name, sizeof(name), "U+%04X", cp);
        std::string shown = (cp >= 0x20 && cp != 0x7F) ? "'" + std::string(utf8.substr(i, len)) + "' " : "";
        out.text.clear();
        out.error = "Character " + std::to_string(charIndex) + " (" + shown + name +
                    ") can't be typed: only US-keyboard characters (plain ASCII, "
                    "newlines and tabs) can be.";
        return out;
    }
    return out;
}

} // namespace houston_kvm

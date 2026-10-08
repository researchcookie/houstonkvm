#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace houston_kvm {

// The key that types one character on a target with a US keyboard layout:
// a browser KeyboardEvent.code, the same vocabulary every InputBackend's
// keyEvent() takes, and whether Shift has to be held for it.
struct TypedKey {
    const char* code;
    bool        shift;
};

// Only printable ASCII, newline and tab have a key; everything else is
// nullopt. The target's layout is assumed to be US: on any other layout the
// same keys produce different characters, which is also true of what a
// person types through the KVM, so there's no worse surprise here than there.
std::optional<TypedKey> usKeyFor(char c);

// Pasted text made ready for InputQueue::pushText(): line endings normalised
// (CRLF and a lone CR become "\n", so a Windows clipboard doesn't press
// Enter twice per line), and every character checked for a key. `error` is
// set instead when something can't be typed, naming the first such
// character and where it is, so nothing is typed at all rather than a
// silently mangled half of it.
struct PreparedText {
    std::string text;
    std::string error;
};
PreparedText prepareTextForTyping(std::string_view utf8);

} // namespace houston_kvm

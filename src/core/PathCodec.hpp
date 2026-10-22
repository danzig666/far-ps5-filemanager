#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace ps5 {

// The payload treats file names as raw bytes and moves them through JSON and
// URLs with its own scheme (see docs/DESIGN.md, "Path encoding"):
//
//  - Server to client: every byte >= 0x80 is written as \u00XX, so after
//    JSON decoding a name is a "token string" of code points where each code
//    point 0x80..0xFF stands for one raw byte.
//  - Client to server: fs_path_value() decodes the parameter as UTF-8 and, if
//    every code point is <= U+00FF and at least one is >= U+0080, collapses
//    each code point back into one byte.
//
// So the only safe way to send an arbitrary byte path is the token form.
namespace path_codec {

// Token string (UTF-8 text as produced by our JSON parser) -> raw bytes.
std::string TokenToRaw(std::string_view token);

// Raw bytes -> token string (UTF-8 text) to put in a request parameter.
std::string RawToToken(std::string_view raw);

// Decodes non-UTF-8 byte names for display. Set by the platform layer
// (Windows uses MultiByteToWideChar with the configured code page). The
// default maps each byte to U+00XX.
using FallbackDecoder = std::function<std::wstring(std::string_view raw)>;
void SetFallbackDecoder(FallbackDecoder decoder);

// Raw bytes -> display name. Valid UTF-8 decodes normally, anything else goes
// through the fallback decoder. *lossy is set when the fallback was used.
std::wstring RawToDisplay(std::string_view raw, bool* lossy = nullptr);

// Display name typed by the user -> raw bytes (UTF-8).
std::string DisplayToRaw(std::wstring_view display);

// Joins a raw directory path and a raw leaf with exactly one '/'.
std::string Join(std::string_view dir, std::string_view leaf);

// Parent of a raw path ("/a/b" -> "/a", "/a" -> "/", "/" -> "/").
std::string Parent(std::string_view path);

// Last component of a raw path.
std::string Leaf(std::string_view path);

}  // namespace path_codec
}  // namespace ps5

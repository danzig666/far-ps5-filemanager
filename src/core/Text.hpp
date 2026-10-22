#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps5 {

void AppendUtf8(std::string& out, char32_t cp);

// Strict UTF-8 decoding (rejects overlongs, surrogates, > U+10FFFF).
bool DecodeUtf8(std::string_view text, std::u32string& out);
bool IsValidUtf8(std::string_view text);

// Invalid sequences become U+FFFD. Works with 16-bit (Windows) and 32-bit
// wchar_t.
std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

// Percent-encodes everything except RFC 3986 unreserved characters. Space
// becomes %20 and '+' becomes %2B, which matters because the payload decodes
// '+' as a space.
std::string PercentEncode(std::string_view text);

// application/x-www-form-urlencoded body or query string (no leading '?').
std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields);

std::string ToLowerAscii(std::string_view text);

// "1.5 GB" style sizes and "1:02:03" style durations for progress displays.
std::string FormatSize(uint64_t bytes);
std::string FormatDuration(uint64_t seconds);

// Unix permission bits as "rwxr-xr-x", and as the payload's "0755" form.
std::string FormatMode(uint32_t mode);
std::string FormatOctalMode(uint32_t mode);
bool ParseOctalMode(std::string_view text, uint32_t& mode);

}  // namespace ps5

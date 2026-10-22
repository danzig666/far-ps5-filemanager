#include "Text.hpp"

#include <cstdio>

namespace ps5 {

void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    AppendUtf8(out, 0xFFFD);
  }
}

namespace {

// Decodes one sequence at text[pos]. Returns false on an invalid sequence and
// leaves pos unchanged.
bool DecodeOne(std::string_view text, size_t& pos, char32_t& cp) {
  unsigned char c = static_cast<unsigned char>(text[pos]);
  size_t n;
  char32_t value;
  if (c < 0x80) {
    cp = c;
    ++pos;
    return true;
  } else if ((c & 0xE0) == 0xC0) {
    n = 2;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
    value = c & 0x07;
  } else {
    return false;
  }
  if (pos + n > text.size()) return false;
  for (size_t i = 1; i < n; ++i) {
    unsigned char t = static_cast<unsigned char>(text[pos + i]);
    if ((t & 0xC0) != 0x80) return false;
    value = (value << 6) | (t & 0x3F);
  }
  if ((n == 2 && value < 0x80) || (n == 3 && value < 0x800) ||
      (n == 4 && (value < 0x10000 || value > 0x10FFFF)) ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return false;
  }
  cp = value;
  pos += n;
  return true;
}

void AppendWide(std::wstring& out, char32_t cp) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
      return;
    }
  }
  out.push_back(static_cast<wchar_t>(cp));
}

}  // namespace

bool DecodeUtf8(std::string_view text, std::u32string& out) {
  out.clear();
  size_t pos = 0;
  while (pos < text.size()) {
    char32_t cp;
    if (!DecodeOne(text, pos, cp)) return false;
    out.push_back(cp);
  }
  return true;
}

bool IsValidUtf8(std::string_view text) {
  size_t pos = 0;
  while (pos < text.size()) {
    char32_t cp;
    if (!DecodeOne(text, pos, cp)) return false;
  }
  return true;
}

std::wstring Utf8ToWide(std::string_view text) {
  std::wstring out;
  out.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    char32_t cp;
    if (!DecodeOne(text, pos, cp)) {
      cp = 0xFFFD;
      ++pos;
    }
    AppendWide(out, cp);
  }
  return out;
}

std::string WideToUtf8(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    char32_t cp = static_cast<char32_t>(text[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
        char32_t low = static_cast<char32_t>(text[i + 1]);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          ++i;
        }
      }
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    AppendUtf8(out, cp);
  }
  return out;
}

std::string PercentEncode(std::string_view text) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size() * 3);
  for (char ch : text) {
    unsigned char c = static_cast<unsigned char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& field : fields) {
    if (!out.empty()) out.push_back('&');
    out += PercentEncode(field.first);
    out.push_back('=');
    out += PercentEncode(field.second);
  }
  return out;
}

std::string ToLowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string FormatSize(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  if (bytes < 1024) return std::to_string(bytes) + " B";
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024 && unit < 5) {
    value /= 1024;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), value < 10 ? "%.2f %s" : value < 100 ? "%.1f %s" : "%.0f %s",
                value, kUnits[unit]);
  return buf;
}

std::string FormatDuration(uint64_t seconds) {
  char buf[32];
  uint64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
  if (h) {
    std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
  } else {
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu", static_cast<unsigned long long>(m),
                  static_cast<unsigned long long>(s));
  }
  return buf;
}

std::string FormatMode(uint32_t mode) {
  static const char kFlags[] = "rwxrwxrwx";
  std::string out(9, '-');
  for (int i = 0; i < 9; ++i) {
    if (mode & (1u << (8 - i))) out[i] = kFlags[i];
  }
  return out;
}

std::string FormatOctalMode(uint32_t mode) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "0%03o", mode & 0777);
  return buf;
}

bool ParseOctalMode(std::string_view text, uint32_t& mode) {
  if (text.size() == 4 && text[0] == '0') text.remove_prefix(1);
  if (text.size() != 3) return false;
  uint32_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '7') return false;
    value = (value << 3) | static_cast<uint32_t>(c - '0');
  }
  mode = value;
  return true;
}

}  // namespace ps5

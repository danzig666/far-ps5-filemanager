#include "PathCodec.hpp"

#include "Text.hpp"

namespace ps5 {
namespace path_codec {

namespace {
FallbackDecoder& Decoder() {
  static FallbackDecoder decoder;
  return decoder;
}
}  // namespace

std::string TokenToRaw(std::string_view token) {
  std::u32string cps;
  if (!DecodeUtf8(token, cps)) return std::string(token);
  std::string raw;
  raw.reserve(cps.size());
  for (char32_t cp : cps) {
    if (cp <= 0xFF) {
      raw.push_back(static_cast<char>(cp));
    } else {
      // The payload never sends these, but keep them as UTF-8 if it does.
      AppendUtf8(raw, cp);
    }
  }
  return raw;
}

std::string RawToToken(std::string_view raw) {
  std::string token;
  token.reserve(raw.size() * 2);
  for (char ch : raw) {
    AppendUtf8(token, static_cast<unsigned char>(ch));
  }
  return token;
}

void SetFallbackDecoder(FallbackDecoder decoder) {
  Decoder() = std::move(decoder);
}

std::wstring RawToDisplay(std::string_view raw, bool* lossy) {
  if (IsValidUtf8(raw)) {
    if (lossy) *lossy = false;
    return Utf8ToWide(raw);
  }
  if (lossy) *lossy = true;
  if (Decoder()) {
    std::wstring decoded = Decoder()(raw);
    if (!decoded.empty() || raw.empty()) return decoded;
  }
  std::wstring out;
  out.reserve(raw.size());
  for (char ch : raw) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
  return out;
}

std::string DisplayToRaw(std::wstring_view display) {
  return WideToUtf8(display);
}

std::string Join(std::string_view dir, std::string_view leaf) {
  std::string out(dir);
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  while (!leaf.empty() && leaf.front() == '/') leaf.remove_prefix(1);
  if (out.empty() || out.back() != '/') out.push_back('/');
  out += leaf;
  return out;
}

std::string Parent(std::string_view path) {
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return "/";
  if (slash == 0) return "/";
  // Keep "smb://server" style roots intact.
  if (slash >= 1 && p[slash - 1] == '/') return p.substr(0, slash + 1);
  return p.substr(0, slash);
}

std::string Leaf(std::string_view path) {
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return p;
  return p.substr(slash + 1);
}

}  // namespace path_codec
}  // namespace ps5

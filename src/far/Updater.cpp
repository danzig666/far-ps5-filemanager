#include "Updater.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "FarApi.hpp"
#include "Guids.hpp"
#include "UpdaterConfig.hpp"

namespace updater {

namespace {

namespace core = updcfg::core;

std::wstring W(const std::string& s) {
  return core::Utf8ToWide(s);
}

std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

class Internet {
public:
  explicit Internet(HINTERNET h = nullptr) : h_(h) {}
  ~Internet() {
    if (h_) WinHttpCloseHandle(h_);
  }
  Internet(const Internet&) = delete;
  Internet& operator=(const Internet&) = delete;
  HINTERNET get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

private:
  HINTERNET h_;
};

// HTTPS GET. Follows redirects (no credentials are ever sent, so that is
// safe). Returns the HTTP status, or 0 with `error` set on network failure.
int HttpsGet(const std::wstring& url, const wchar_t* accept, std::string& body, std::wstring& error) {
  body.clear();
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 4096;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    error = L"bad URL";
    return 0;
  }
  std::wstring agent = L"far-plugin-updater/" + W(updcfg::kVersion);
  HINTERNET raw = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
  if (!raw) {
    // Before Windows 8.1: the WinHTTP proxy configuration.
    raw = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  Internet session(raw);
  if (!session) {
    error = L"WinHTTP is not available";
    return 0;
  }
  Internet connect(WinHttpConnect(session.get(), host, parts.nPort, 0));
  if (!connect) {
    error = L"cannot connect to " + std::wstring(host);
    return 0;
  }
  std::wstring target = std::wstring(path) + extra;
  Internet request(WinHttpOpenRequest(connect.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
  if (!request) {
    error = L"cannot open the request";
    return 0;
  }
  WinHttpSetTimeouts(request.get(), 10000, 10000, 30000, 30000);
  std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
  if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    DWORD code = GetLastError();
    error = code == ERROR_WINHTTP_NAME_NOT_RESOLVED ? L"no internet connection (cannot resolve the host)"
            : code == ERROR_WINHTTP_TIMEOUT          ? L"the connection timed out"
                                                     : L"network error " + std::to_wstring(code);
    return 0;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  std::vector<char> buffer(64 * 1024);
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
      error = L"the download was interrupted";
      return 0;
    }
    if (!read) break;
    body.append(buffer.data(), read);
    if (body.size() > 64u * 1024 * 1024) {
      error = L"the download is unexpectedly large";
      return 0;
    }
  }
  return static_cast<int>(status);
}

std::vector<int> ParseVersion(std::string s) {
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
  std::vector<int> parts;
  int value = 0;
  bool any = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      any = true;
    } else if (c == '.') {
      parts.push_back(value);
      value = 0;
      any = false;
    } else {
      break;  // "-beta" and the like: ignore the suffix
    }
  }
  if (any || !parts.empty()) parts.push_back(value);
  while (parts.size() < 3) parts.push_back(0);
  return parts;
}

bool IsNewer(const std::string& candidate, const std::string& current) {
  return ParseVersion(candidate) > ParseVersion(current);
}

std::wstring OwnDllPath() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&CheckForUpdates), &module);
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (len < path.size()) {
      path.resize(len);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring DirOf(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring LeafOf(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool CanWriteTo(const std::wstring& dir) {
  std::wstring probe = dir + L"\\.update-write-test-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

std::string Sha256Hex(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  unsigned char digest[32] = {};
  NTSTATUS st = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                           static_cast<ULONG>(data.size()), digest, sizeof(digest));
  BCryptCloseAlgorithmProvider(alg, 0);
  if (st != 0) return {};
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  for (unsigned char b : digest) {
    hex.push_back(kHex[b >> 4]);
    hex.push_back(kHex[b & 15]);
  }
  return hex;
}

// "C:\x\y" -> "\Device\HarddiskVolume3\x\y", the form GetMappedFileName uses.
std::wstring ToDevicePath(const std::wstring& path) {
  if (path.size() >= 2 && path[1] == L':') {
    wchar_t drive[3] = {path[0], L':', 0};
    wchar_t device[MAX_PATH] = {};
    if (QueryDosDeviceW(drive, device, MAX_PATH)) return std::wstring(device) + path.substr(2);
  }
  if (path.rfind(L"\\\\", 0) == 0) return L"\\Device\\Mup" + path.substr(1);
  return path;
}

// Paths (device form, lower case) of every image mapped in every running
// far.exe, including this one. `complete` is false if any far.exe could not
// be inspected - then nobody can say a file is unused.
std::vector<std::wstring> ImagesLoadedByFar(bool& complete) {
  complete = true;
  std::vector<std::wstring> images;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    complete = false;
    return images;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
    if (Lower(entry.szExeFile) != L"far.exe") continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
    if (!process) {
      complete = false;  // e.g. a Far running as administrator
      continue;
    }
    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    for (;;) {
      if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed,
                                LIST_MODULES_ALL)) {
        complete = false;
        needed = 0;
        break;
      }
      if (needed <= modules.size() * sizeof(HMODULE)) break;
      modules.resize(needed / sizeof(HMODULE) + 16);
    }
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
      wchar_t mapped[MAX_PATH * 2] = {};
      // The name of the file behind the mapping as it is *now*, so a DLL
      // that was renamed to *.old after loading is reported as *.old.
      if (GetMappedFileNameW(process, modules[i], mapped, MAX_PATH * 2)) images.push_back(Lower(mapped));
    }
    CloseHandle(process);
  }
  CloseHandle(snapshot);
  return images;
}

void Shell(const std::wstring& url) {
  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

void CleanupOldFiles() {
  std::wstring dll = OwnDllPath();
  std::wstring dir = DirOf(dll);
  if (dir.empty()) return;
  std::vector<std::wstring> candidates;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dll + L".*.old").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) candidates.push_back(dir + L"\\" + fd.cFileName);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  if (candidates.empty()) return;

  bool complete = false;
  std::vector<std::wstring> loaded = ImagesLoadedByFar(complete);
  if (!complete) return;  // some Far could not be checked: keep everything
  for (const auto& path : candidates) {
    std::wstring device = Lower(ToDevicePath(path));
    if (std::find(loaded.begin(), loaded.end(), device) != loaded.end()) continue;  // still in use
    // Windows also refuses to delete a DLL any process has loaded, so a
    // failure here just means "try again next time".
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());
  }
}

void CheckForUpdates() {
  const std::wstring title = L"Check for updates";
  const std::wstring current = W(updcfg::kVersion);
  std::string body;
  std::wstring error;
  std::wstring api = L"https://api.github.com/repos/" + W(updcfg::kOwner) + L"/" + W(updcfg::kRepo) + L"/releases/latest";

  HANDLE screen = PsInfo.SaveScreen(0, 0, -1, -1);
  fui::Message(FMSG_NONE, {title, L"Asking GitHub for the latest version..."}, {}, MessageGuid);
  int status = HttpsGet(api, L"application/vnd.github+json", body, error);
  PsInfo.RestoreScreen(screen);
  if (status == 0) {
    fui::ShowError(title, L"Could not reach GitHub:\n" + error);
    return;
  }
  if (status == 404) {
    fui::ShowInfo(title, L"No releases were found.");
    return;
  }
  if (status != 200) {
    fui::ShowError(title, L"GitHub answered with HTTP " + std::to_wstring(status) +
                              (status == 403 ? L".\nThe hourly limit for anonymous requests may be used up." : L"."));
    return;
  }
  core::JsonValue release;
  try {
    release = core::ParseJson(body);
  } catch (const std::exception&) {
    fui::ShowError(title, L"GitHub sent an unexpected answer.");
    return;
  }
  std::string tag = release["tag_name"].asString();
  std::wstring page = W(release["html_url"].asString());
  if (!IsNewer(tag, updcfg::kVersion)) {
    fui::ShowInfo(title, std::wstring(updcfg::kProduct) + L" is up to date.\nYou have version " + current +
                             L"; the latest release is " + W(tag) + L".");
    return;
  }

  std::vector<std::wstring> lines{title, std::wstring(updcfg::kProduct) + L" " + W(tag) + L" is available.",
                                  L"You have version " + current + L".", L"\x1"};
  {
    std::wstring notes = W(release["body"].asString());
    size_t start = 0, shown = 0;
    while (start < notes.size() && shown < 10) {
      size_t end = notes.find(L'\n', start);
      std::wstring line = notes.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
      while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
      if (!line.empty()) {
        if (line.size() > 72) line = line.substr(0, 69) + L"...";
        lines.push_back(line);
        ++shown;
      }
      if (end == std::wstring::npos) break;
      start = end + 1;
    }
  }
  intptr_t choice = fui::Message(FMSG_LEFTALIGN, lines, {L"&Update now", L"Open release &page", L"&Cancel"}, MessageGuid);
  if (choice == 1) Shell(page);
  if (choice != 0) return;

  std::wstring dll = OwnDllPath();
  std::wstring dir = DirOf(dll);
  if (!CanWriteTo(dir)) {
    if (fui::Message(FMSG_WARNING | FMSG_LEFTALIGN,
                     {title, L"The plugin is in a folder you cannot write to:", dir, L"\x1",
                      L"Far also loads plugins from your profile, which needs no admin rights:",
                      L"  %FARPROFILE%\\Plugins  (usually %APPDATA%\\Far Manager\\Profile\\Plugins)",
                      L"Move the plugin's folder there (and remove it from here), then update again.",
                      L"Or update by hand from the release page."},
                     {L"Open release &page", L"&Close"}, MessageGuid) == 0) {
      Shell(page);
    }
    return;
  }

  std::string dllName = updcfg::kDllName;
  std::string dllUrl, shaUrl;
  uint64_t dllSize = 0;
  for (const auto& asset : release["assets"].items()) {
    std::string name = asset["name"].asString();
    if (name == dllName) {
      dllUrl = asset["browser_download_url"].asString();
      dllSize = asset["size"].asUInt();
    } else if (name == dllName + ".sha256") {
      shaUrl = asset["browser_download_url"].asString();
    }
  }
  if (dllUrl.empty() || shaUrl.empty()) {
    if (fui::Message(FMSG_WARNING, {title, L"This release has no files for automatic updates.", L"Please update by hand."},
                     {L"Open release &page", L"&Close"}, MessageGuid) == 0) {
      Shell(page);
    }
    return;
  }

  std::string dllBytes, shaText;
  screen = PsInfo.SaveScreen(0, 0, -1, -1);
  fui::Message(FMSG_NONE, {title, L"Downloading " + W(tag) + L"..."}, {}, MessageGuid);
  int s1 = HttpsGet(W(dllUrl), L"application/octet-stream", dllBytes, error);
  int s2 = s1 == 200 ? HttpsGet(W(shaUrl), L"application/octet-stream", shaText, error) : s1;
  PsInfo.RestoreScreen(screen);
  if (s1 != 200 || s2 != 200) {
    fui::ShowError(title, L"The download failed: " + (error.empty() ? L"HTTP " + std::to_wstring(s1 != 200 ? s1 : s2) : error));
    return;
  }
  std::string expected = shaText.substr(0, std::min<size_t>(64, shaText.size()));
  for (auto& c : expected) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if ((dllSize && dllBytes.size() != dllSize) || dllBytes.size() < 4096 || dllBytes.compare(0, 2, "MZ") != 0 ||
      expected.size() != 64 || Sha256Hex(dllBytes) != expected) {
    fui::ShowError(title, L"The downloaded file failed its integrity check.\nNothing was changed.");
    return;
  }

  // Write next to the old DLL, then swap by renaming (allowed while loaded).
  std::wstring fresh = dll + L".new";
  HANDLE file = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  bool ok = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, dllBytes.data(), static_cast<DWORD>(dllBytes.size()), &written, nullptr) &&
            written == dllBytes.size() && FlushFileBuffers(file);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!ok) {
    DeleteFileW(fresh.c_str());
    fui::ShowError(title, L"Could not write " + fresh + L".\nNothing was changed.");
    return;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t stamp[32];
  swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  std::wstring old = dll + L"." + current + L"-" + stamp + L".old";
  if (!MoveFileExW(dll.c_str(), old.c_str(), 0)) {
    DeleteFileW(fresh.c_str());
    fui::ShowError(title, L"Could not rename the current plugin file.\nNothing was changed.");
    return;
  }
  if (!MoveFileExW(fresh.c_str(), dll.c_str(), 0)) {
    MoveFileExW(old.c_str(), dll.c_str(), 0);  // put the old one back
    DeleteFileW(fresh.c_str());
    fui::ShowError(title, L"Could not put the new plugin file in place.\nThe old version was restored.");
    return;
  }
  CleanupOldFiles();  // removes earlier leftovers nobody uses; this run's .old is still loaded
  fui::Message(FMSG_LEFTALIGN,
               {title, std::wstring(updcfg::kProduct) + L" was updated to " + W(tag) + L".", L"\x1",
                L"Restart Far to start using the new version.",
                L"Far windows that are open now keep the old version until they restart.",
                L"The old file (" + LeafOf(old) + L")",
                L"is deleted automatically once no running Far uses it."},
               {L"OK"}, MessageGuid);
}

}  // namespace updater

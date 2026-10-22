#include "Panel.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <cwctype>

#include "../core/PathCodec.hpp"
#include "../core/Text.hpp"
#include "FarApi.hpp"
#include "Guids.hpp"
#include "Progress.hpp"
#include "WinHttpTransport.hpp"

namespace codec = ps5::path_codec;
using fui::W;

namespace {

std::set<Panel*>& AllPanels() {
  static std::set<Panel*> panels;
  return panels;
}

std::shared_ptr<ps5::HttpTransport> Transport() {
  static std::shared_ptr<ps5::HttpTransport> transport = std::make_shared<ps5::WinHttpTransport>();
  return transport;
}

std::shared_ptr<Session> GetSession(const HostConfig& host) {
  static std::map<std::wstring, std::weak_ptr<Session>> sessions;
  std::wstring key = host.Endpoint();
  if (auto existing = sessions[key].lock()) {
    existing->host.name = host.name;
    existing->host.codePage = host.codePage;
    return existing;
  }
  auto session = std::make_shared<Session>();
  session->host = host;
  session->client = std::make_unique<ps5::Ps5Client>(Transport(), fui::U(host.address), host.port);
  sessions[key] = session;
  return session;
}

bool IsAbsoluteRaw(const std::string& raw) {
  return (!raw.empty() && raw[0] == '/') || raw.rfind("smb://", 0) == 0;
}

std::wstring Lower(std::wstring text) {
  for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
  return text;
}

std::wstring Extension(const std::wstring& name) {
  size_t dot = name.rfind(L'.');
  return dot == std::wstring::npos ? L"" : Lower(name.substr(dot));
}

std::wstring Trim(const std::wstring& text) {
  size_t a = text.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return L"";
  size_t b = text.find_last_not_of(L" \t");
  return text.substr(a, b - a + 1);
}

std::wstring Slashes(std::wstring text) {
  std::replace(text.begin(), text.end(), L'\\', L'/');
  return text;
}

std::wstring Leaf(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsAbsoluteLocal(const std::wstring& path) {
  return (path.size() > 2 && path[1] == L':') || path.rfind(L"\\\\", 0) == 0;
}

std::wstring JoinLocal(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

std::wstring LastErrorText() {
  DWORD code = GetLastError();
  wchar_t* buffer = nullptr;
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring text = len && buffer ? std::wstring(buffer, len) : L"error " + std::to_wstring(code);
  if (buffer) LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
    text.pop_back();
  return text;
}

[[noreturn]] void ThrowLastError(const std::wstring& what) {
  throw std::runtime_error(fui::U(what + L"\n" + LastErrorText()));
}

class File {
public:
  explicit File(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
  ~File() { Close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  HANDLE get() const { return h_; }
  bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
  void Close() {
    if (valid()) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

private:
  HANDLE h_;
};

bool CreateDirs(const std::wstring& path) {
  std::wstring p = path;
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  if (p.empty()) return true;
  DWORD attrs = GetFileAttributesW(fui::LongPath(p).c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 2) {
    if (!CreateDirs(p.substr(0, slash))) return false;
  }
  return CreateDirectoryW(fui::LongPath(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring FullLocalPath(const std::wstring& path) {
  DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!size) return path;
  std::wstring out(size, L'\0');
  DWORD len = GetFullPathNameW(path.c_str(), size, out.data(), nullptr);
  out.resize(len);
  return out;
}

void SleepMs(int ms) {
  Sleep(static_cast<DWORD>(ms));
}

const wchar_t* kTitle = L"PS5";

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime and lookup

Panel::Panel() {
  AllPanels().insert(this);
}

Panel::~Panel() {
  AllPanels().erase(this);
}

Panel* Panel::FromHandle(HANDLE handle) {
  auto* panel = static_cast<Panel*>(handle);
  return AllPanels().count(panel) ? panel : nullptr;
}

namespace {
Panel* PanelAt(HANDLE which) {
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(which, FCTL_GETPANELINFO, 0, &pi)) return nullptr;
  if (!(pi.Flags & PFLAGS_PLUGIN) || pi.OwnerGuid != PluginGuid) return nullptr;
  return Panel::FromHandle(pi.PluginHandle);
}
}  // namespace

Panel* Panel::Active() {
  return PanelAt(PANEL_ACTIVE);
}

Panel* Panel::Passive() {
  return PanelAt(PANEL_PASSIVE);
}

ps5::Ps5Client& Panel::Client() {
  return *session_->client;
}

void Panel::ApplyCodePage() {
  unsigned cp = session_ ? session_->host.codePage : 936;
  codec::SetFallbackDecoder([cp](std::string_view raw) -> std::wstring {
    if (raw.empty()) return {};
    int len = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), out.data(), len);
    return out;
  });
}

// ---------------------------------------------------------------------------
// Error handling and tasks

bool Panel::Run(const std::wstring& title, bool silent, const std::function<void()>& fn) {
  for (int attempt = 0;; ++attempt) {
    try {
      fn();
      return true;
    } catch (const ps5::CancelledError&) {
    } catch (const ps5::ApiError& e) {
      if (!silent) {
        if (e.code() == "active_task") {
          // Show the other task live; when it ends, try again.
          if (WatchBusy(title) && attempt < 3) continue;
        } else {
          fui::ShowError(title, W(ps5::DescribeApiError(e)));
        }
      }
    } catch (const ps5::NetError& e) {
      if (!silent) {
        std::wstring where = session_ ? session_->host.Endpoint() : L"the console";
        fui::ShowError(title, L"Cannot reach " + where + L":\n" + W(e.what()) +
                                  L"\nIs the payload running on the console?");
      }
    } catch (const std::exception& e) {
      if (!silent) fui::ShowError(title, W(e.what()));
    }
    return false;
  }
}

bool Panel::WatchBusy(const std::wstring& title, bool explicitRequest) {
  ps5::TaskInfo task;
  bool found = false;
  try {
    found = ps5::FindActiveTask(Client(), task);
  } catch (const std::exception&) {
  }
  if (!found) return true;  // it ended in the meantime
  // Don't pop the view up again on every panel refresh after the user closed it.
  if (!explicitRequest && inFindData_ && session_->dismissedTask == task.id) return false;
  try {
    WatchTask(task.id, title, &task);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

ps5::TaskResult Panel::WatchTask(uint64_t id, const std::wstring& title, const ps5::TaskInfo* initial) {
  Progress progress(title);
  std::string op = initial ? initial->op : std::string();
  auto setHint = [&] {
    progress.SetHint(op == "pkg_install" ? L"Esc: close this window (the installation keeps running)"
                                         : L"Esc: close this window (the task keeps running)    F8: stop the task");
  };
  setHint();
  if (initial) {
    progress.ServerTask(*initial);
  } else {
    progress.Status(L"Waiting for the console...");
  }
  bool stopSent = false;
  return ps5::WaitForTask(
      Client(), id,
      [&](const ps5::TaskInfo* t) {
        if (t) {
          if (t->op != op) {
            op = t->op;
            setHint();
          }
          progress.ServerTask(*t);
        }
        int key = fui::PollKey();
        if (key == VK_ESCAPE) {
          session_->dismissedTask = id;
          throw ps5::CancelledError();  // close the view; the task keeps running
        }
        if (key != VK_F8 || stopSent) return true;
        if (op == "pkg_install") {
          fui::ShowInfo(title, L"A package installation cannot be stopped.");
          progress.Draw(true);
          return true;
        }
        // "Keep running" is the first, default button: Enter or Esc keeps the task.
        intptr_t choice = fui::Message(FMSG_WARNING,
                                       {L"Stop the task", L"Stop this task on the console?",
                                        L"Work already done (copied or extracted files) is not undone."},
                                       {L"&Keep running", L"&Stop the task"}, MessageGuid);
        progress.Draw(true);
        if (choice != 1) return true;
        stopSent = true;
        try {
          Client().Cancel(id);
        } catch (const ps5::ApiError&) {
        }
        if (op == "upload") {
          // An upload only ends once its uploader calls finish.
          try {
            Client().UploadFinish(id);
          } catch (const ps5::ApiError&) {
          }
        }
        return true;
      },
      SleepMs, 500);
}

bool Panel::ReportTask(const ps5::TaskResult& result, const std::wstring& title) {
  switch (result.outcome) {
    case ps5::TaskOutcome::Done:
    case ps5::TaskOutcome::Vanished:
      return true;
    case ps5::TaskOutcome::Failed:
      fui::ShowError(title, W(ps5::DescribeTaskError(result.last)));
      return false;
    case ps5::TaskOutcome::Canceled:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Opening

bool Panel::OpenHosts() {
  level_ = Level::Hosts;
  hosts_ = config::LoadHosts();
  if (hosts_.empty()) AddHost();
  UpdateStrings();
  return true;
}

bool Panel::OpenTarget(const HostConfig& host, const std::string& rawPath) {
  session_ = GetSession(host);
  ApplyCodePage();
  bool ok = Run(L"Connect to " + host.name, false, [&] { LoadVolumes(); });
  if (!ok) return false;
  level_ = Level::Volumes;
  if (!rawPath.empty()) {
    Run(kTitle, false, [&] {
      fromRoot_ = VolumeFor(rawPath) == nullptr;
      EnterPath(rawPath, false);
    });
  }
  UpdateStrings();
  return true;
}

Panel* Panel::OpenFromText(const std::wstring& input, bool shortcut) {
  std::wstring text = Trim(input);
  std::string rawPath;
  std::wstring hostPart = text;
  if (shortcut) {
    // "address:port|<token path>"
    size_t bar = text.find(L'|');
    if (bar != std::wstring::npos) {
      hostPart = text.substr(0, bar);
      rawPath = codec::TokenToRaw(fui::U(text.substr(bar + 1)));
    }
  } else {
    if (hostPart.rfind(L"//", 0) == 0) hostPart = hostPart.substr(2);
    size_t slash = hostPart.find(L'/');
    if (slash != std::wstring::npos) {
      rawPath = codec::DisplayToRaw(hostPart.substr(slash));
      hostPart = hostPart.substr(0, slash);
    }
  }

  auto panel = std::make_unique<Panel>();
  if (hostPart.empty()) {
    panel->OpenHosts();
    return panel.release();
  }

  HostConfig host;
  bool found = false;
  for (const auto& h : config::LoadHosts()) {
    if (Lower(h.name) == Lower(hostPart) || Lower(h.Endpoint()) == Lower(hostPart) ||
        (Lower(h.address) == Lower(hostPart) && h.port == 8888)) {
      host = h;
      found = true;
      break;
    }
  }
  if (!found) {
    size_t colon = hostPart.rfind(L':');
    host.address = hostPart;
    if (colon != std::wstring::npos) {
      unsigned long port = wcstoul(hostPart.c_str() + colon + 1, nullptr, 10);
      if (port > 0 && port <= 65535) {
        host.address = hostPart.substr(0, colon);
        host.port = static_cast<uint16_t>(port);
      }
    }
    host.name = host.Endpoint();
  }
  if (!panel->OpenTarget(host, rawPath)) return nullptr;
  return panel.release();
}

// ---------------------------------------------------------------------------
// Panel info

void Panel::UpdateStrings() {
  keyLabels_.clear();
  auto label = [&](WORD vk, DWORD state, const wchar_t* text) {
    keyLabels_.push_back(KeyBarLabel{FarKey{vk, state}, text, text});
  };
  switch (level_) {
    case Level::Hosts:
      curDirW_.clear();
      titleW_ = L" PS5: consoles ";
      shortcutW_.clear();
      label(VK_F3, 0, L"");
      label(VK_F4, 0, L"Edit");
      label(VK_F5, 0, L"");
      label(VK_F6, 0, L"");
      label(VK_F7, 0, L"New");
      label(VK_F8, 0, L"Delete");
      label(VK_F4, SHIFT_PRESSED, L"New");
      label(VK_F6, SHIFT_PRESSED, L"Edit");
      break;
    case Level::Volumes:
      curDirW_ = session_->host.name;
      titleW_ = L" PS5:" + session_->host.name + L" ";
      shortcutW_ = session_->host.Endpoint() + L"|";
      label(VK_F3, 0, L"");
      label(VK_F4, 0, L"");
      label(VK_F5, 0, L"");
      label(VK_F6, 0, L"");
      label(VK_F7, 0, L"");
      label(VK_F8, 0, L"");
      label(VK_F1, SHIFT_PRESSED, L"Action");
      break;
    case Level::Dir:
      curDirW_ = codec::RawToDisplay(cur_);
      titleW_ = L" PS5:" + session_->host.name + L":" + curDirW_ + L" ";
      shortcutW_ = session_->host.Endpoint() + L"|" + W(codec::RawToToken(cur_));
      label(VK_F1, SHIFT_PRESSED, L"Action");
      break;
  }
  keyBar_.CountLabels = keyLabels_.size();
  keyBar_.Labels = keyLabels_.data();
}

void Panel::GetOpenPanelInfo(OpenPanelInfo* info) {
  UpdateStrings();
  info->StructSize = sizeof(*info);
  info->Flags = OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE | OPIF_SHORTCUT;
  info->HostFile = nullptr;
  info->CurDir = curDirW_.c_str();
  info->Format = L"ps5";
  info->PanelTitle = titleW_.c_str();
  info->ShortcutData = shortcutW_.empty() ? nullptr : shortcutW_.c_str();
  info->KeyBar = &keyBar_;
  if (level_ == Level::Dir) {
    if (const Volume* v = VolumeFor(cur_)) {
      info->Flags |= OPIF_USEFREESIZE;
      info->FreeSize = v->info.free;
    }
  }
}

// ---------------------------------------------------------------------------
// Navigation

void Panel::LoadVolumes() {
  std::vector<ps5::SpaceInfo> spaces = Client().Space();
  volumes_.clear();
  std::set<std::wstring> used;
  bool hasRoot = false;
  auto add = [&](std::wstring name, const ps5::SpaceInfo& info) {
    name = fui::SafeFileName(name);
    std::wstring unique = name;
    for (int i = 2; used.count(Lower(unique)); ++i) unique = name + L"-" + std::to_wstring(i);
    used.insert(Lower(unique));
    volumes_.push_back({unique, info});
  };
  for (const auto& s : spaces) {
    std::wstring name;
    if (s.path == "/") {
      name = L"root";
      hasRoot = true;
    } else if (s.path == "/data") {
      name = L"internal";
    } else if (s.path == "/mnt/ext1") {
      name = L"m2";
    } else if (s.path == "/mnt/ext0") {
      name = L"extended";
    } else if (s.isSmb()) {
      std::wstring rest = codec::RawToDisplay(s.path.substr(6));
      std::replace(rest.begin(), rest.end(), L'/', L'-');
      name = L"smb-" + rest;
    } else {
      name = codec::RawToDisplay(codec::Leaf(s.path));
    }
    add(name, s);
  }
  if (!hasRoot) {
    ps5::SpaceInfo root;
    root.labelKey = "storageRoot";
    root.path = "/";
    add(L"root", root);
  }
  volumesFresh_ = true;
}

const Panel::Volume* Panel::VolumeFor(const std::string& rawPath) const {
  const Volume* best = nullptr;
  for (const auto& v : volumes_) {
    const std::string& p = v.info.path;
    if (p == "/") continue;
    bool match = rawPath == p || (rawPath.size() > p.size() && rawPath.compare(0, p.size(), p) == 0 &&
                                  (rawPath[p.size()] == '/' || p.back() == '/'));
    if (match && (!best || p.size() > best->info.path.size())) best = &v;
  }
  return best;
}

void Panel::UnlockSmb(const std::string& rawPath, bool silent) {
  if (rawPath.rfind("smb://", 0) != 0) return;
  const Volume* volume = VolumeFor(rawPath);
  if (!volume || session_->smbUnlocked.count(volume->info.path)) return;
  const std::string& root = volume->info.path;
  std::string user = ps5::ToLowerAscii(volume->info.user);
  bool guest = user.empty() || user == "guest" || user == "anonymous";
  auto ask = [&]() -> std::string {
    if (silent) throw ps5::CancelledError();
    std::wstring password;
    if (!fui::Input(L"SMB password", L"Password for " + codec::RawToDisplay(root) + L":", password,
                    nullptr, FIB_PASSWORD | FIB_ENABLEEMPTY)) {
      throw ps5::CancelledError();
    }
    return fui::U(password);
  };
  std::string password = guest ? std::string() : ask();
  try {
    Client().SmbConnect(root, password);
  } catch (const ps5::ApiError& e) {
    if (!guest || e.code() != "smb_password_required") throw;
    Client().SmbConnect(root, ask());
  }
  session_->smbUnlocked.insert(root);
}

void Panel::EnterPath(const std::string& rawPath, bool silent) {
  UnlockSmb(rawPath, silent);
  ps5::Listing listing = Client().List(rawPath);
  listing_ = std::move(listing);
  cur_ = listing_.path.empty() ? rawPath : listing_.path;
  BuildNames();
  listingFresh_ = true;
  level_ = Level::Dir;
}

void Panel::GoUp(bool silent) {
  const Volume* v = VolumeFor(cur_);
  bool atVolumeRoot = fromRoot_ ? cur_ == "/" : (cur_ == "/" || !v || v->info.path == cur_);
  if (atVolumeRoot) {
    level_ = Level::Volumes;
    if (!volumesFresh_) LoadVolumes();
    return;
  }
  std::string parent = codec::Parent(cur_);
  if (parent.rfind("smb://", 0) == 0 && v && parent.size() < v->info.path.size()) {
    level_ = Level::Volumes;
    return;
  }
  EnterPath(parent, silent);
}

void Panel::BuildNames() {
  names_.clear();
  lossy_.clear();
  byName_.clear();
  for (size_t i = 0; i < listing_.entries.size(); ++i) {
    bool lossy = false;
    std::wstring name = codec::RawToDisplay(listing_.entries[i].name, &lossy);
    if (name.empty() || name == L"." || name == L"..") name = L"_";
    std::wstring unique = name;
    for (int k = 2; byName_.count(unique); ++k) unique = name + L" ~" + std::to_wstring(k);
    byName_[unique] = i;
    names_.push_back(unique);
    lossy_.push_back(lossy);
  }
}

std::string Panel::ResolveInput(const std::wstring& text) const {
  std::wstring path = Slashes(Trim(text));
  std::string raw = codec::DisplayToRaw(path);
  if (IsAbsoluteRaw(raw)) return raw;
  return codec::Join(cur_, raw);
}

intptr_t Panel::SetDirectory(const SetDirectoryInfo* info) {
  std::wstring dir = info->Dir ? info->Dir : L"";
  bool silent = (info->OpMode & (OPM_FIND | OPM_SILENT)) != 0;
  ApplyCodePage();
  bool ok = false;
  switch (level_) {
    case Level::Hosts: {
      if (dir == L"..") return FALSE;
      hosts_ = config::LoadHosts();
      for (const auto& h : hosts_) {
        if (h.name == dir) return OpenTarget(h, {}) ? TRUE : FALSE;
      }
      return FALSE;
    }
    case Level::Volumes: {
      if (dir == L"..") {
        level_ = Level::Hosts;
        hosts_ = config::LoadHosts();
        UpdateStrings();
        return TRUE;
      }
      if (dir == L"\\" || dir == L"/") return TRUE;
      for (const auto& v : volumes_) {
        if (v.name == dir) {
          ok = Run(kTitle, silent, [&] {
            fromRoot_ = v.info.path == "/";
            EnterPath(v.info.path, silent);
          });
          UpdateStrings();
          return ok ? TRUE : FALSE;
        }
      }
      std::string raw = codec::DisplayToRaw(Slashes(dir));
      if (!IsAbsoluteRaw(raw)) return FALSE;
      ok = Run(kTitle, silent, [&] {
        fromRoot_ = VolumeFor(raw) == nullptr;
        EnterPath(raw, silent);
      });
      UpdateStrings();
      return ok ? TRUE : FALSE;
    }
    case Level::Dir: {
      if (dir == L"..") {
        ok = Run(kTitle, silent, [&] { GoUp(silent); });
      } else if (dir == L"\\" || dir == L"/") {
        level_ = Level::Volumes;
        ok = Run(kTitle, silent, [&] { LoadVolumes(); });
        ok = true;
      } else if (const ps5::ListEntry* e = EntryByName(dir)) {
        std::string path = e->path;
        ok = Run(kTitle, silent, [&] { EnterPath(path, silent); });
      } else {
        std::string raw = ResolveInput(dir);
        ok = Run(kTitle, silent, [&] {
          if (IsAbsoluteRaw(codec::DisplayToRaw(Slashes(dir)))) fromRoot_ = VolumeFor(raw) == nullptr;
          EnterPath(raw, silent);
        });
      }
      UpdateStrings();
      return ok ? TRUE : FALSE;
    }
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// Find data

intptr_t Panel::GetFindData(GetFindDataInfo* info) {
  bool silent = (info->OpMode & (OPM_FIND | OPM_SILENT)) != 0;
  ApplyCodePage();
  auto data = std::make_unique<FindData>();
  auto addString = [&](const std::wstring& s) -> const wchar_t* {
    data->strings.push_back(s);
    return data->strings.back().c_str();
  };

  switch (level_) {
    case Level::Hosts: {
      hosts_ = config::LoadHosts();
      data->items.resize(hosts_.size());
      for (size_t i = 0; i < hosts_.size(); ++i) {
        PluginPanelItem& it = data->items[i];
        it = {};
        it.FileName = addString(hosts_[i].name);
        it.Description = addString(hosts_[i].Endpoint());
        it.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      }
      break;
    }
    case Level::Volumes: {
      if (!volumesFresh_) {
        if (!Run(kTitle, silent, [&] { LoadVolumes(); }) && volumes_.empty()) return silent ? FALSE : TRUE;
      }
      volumesFresh_ = false;
      data->items.resize(volumes_.size());
      for (size_t i = 0; i < volumes_.size(); ++i) {
        const auto& v = volumes_[i];
        PluginPanelItem& it = data->items[i];
        it = {};
        it.FileName = addString(v.name);
        std::wstring kind;
        const std::string& key = v.info.labelKey;
        if (key == "storageInternal") kind = L"Internal storage";
        else if (key == "storageUsb") kind = L"USB storage";
        else if (key == "storageM2") kind = L"M.2 storage";
        else if (key == "storageExtended") kind = L"Extended storage";
        else if (key == "storageRoot") kind = L"Whole file system";
        else if (v.info.isSmb()) kind = L"SMB " + codec::RawToDisplay(v.info.path);
        else kind = codec::RawToDisplay(v.info.path);
        if (v.info.total) kind += L", " + fui::FormatSize(v.info.free) + L" free of " + fui::FormatSize(v.info.total);
        if (v.info.custom && !v.info.available) kind += L" (offline)";
        it.Description = addString(kind);
        it.FileSize = v.info.total;
        it.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      }
      break;
    }
    case Level::Dir: {
      if (!listingFresh_) {
        inFindData_ = true;
        bool ok = Run(kTitle, silent, [&] {
          ps5::Listing listing = Client().List(cur_);
          listing_ = std::move(listing);
          BuildNames();
        });
        inFindData_ = false;
        if (!ok && (info->OpMode & OPM_FIND)) return FALSE;
      }
      listingFresh_ = false;
      data->items.resize(listing_.entries.size());
      for (size_t i = 0; i < listing_.entries.size(); ++i) {
        const ps5::ListEntry& e = listing_.entries[i];
        PluginPanelItem& it = data->items[i];
        it = {};
        it.FileName = addString(names_[i]);
        it.FileSize = e.isDir() ? 0 : e.size;
        FILETIME ft = fui::UnixToFileTime(e.mtime);
        it.LastWriteTime = ft;
        it.ChangeTime = ft;
        it.CreationTime = ft;
        it.LastAccessTime = ft;
        uintptr_t attrs = 0;
        if (e.isDir()) attrs |= FILE_ATTRIBUTE_DIRECTORY;
        if (e.type == 'l') attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
        if (!e.name.empty() && e.name[0] == '.') attrs |= FILE_ATTRIBUTE_HIDDEN;
        if (!(e.mode & 0222)) attrs |= FILE_ATTRIBUTE_READONLY;
        if (!e.isDir() && e.type != '-' && e.type != 'l') attrs |= FILE_ATTRIBUTE_SYSTEM;
        it.FileAttributes = attrs;
        std::wstring descr = W(ps5::FormatMode(e.mode));
        if (lossy_[i]) descr += L" (name not UTF-8)";
        it.Description = addString(descr);
      }
      break;
    }
  }
  info->PanelItem = data->items.empty() ? nullptr : data->items.data();
  info->ItemsNumber = data->items.size();
  if (info->PanelItem) findData_[info->PanelItem] = std::move(data);
  return TRUE;
}

void Panel::FreeFindData(const FreeFindDataInfo* info) {
  findData_.erase(info->PanelItem);
}

// ---------------------------------------------------------------------------
// Items

const ps5::ListEntry* Panel::EntryByName(const std::wstring& name) const {
  auto it = byName_.find(name);
  if (it == byName_.end() || it->second >= listing_.entries.size()) return nullptr;
  return &listing_.entries[it->second];
}

ps5::ListEntry Panel::EntryFromItem(const PluginPanelItem& item) const {
  std::wstring name = item.FileName ? item.FileName : L"";
  if (const ps5::ListEntry* e = EntryByName(name)) return *e;
  ps5::ListEntry e;
  e.name = codec::DisplayToRaw(name);
  e.path = codec::Join(cur_, e.name);
  e.type = (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : '-';
  e.size = item.FileSize;
  return e;
}

std::vector<ps5::ListEntry> Panel::EntriesFromItems(const PluginPanelItem* items, size_t count) const {
  std::vector<ps5::ListEntry> out;
  for (size_t i = 0; i < count; ++i) {
    if (!items[i].FileName || !wcscmp(items[i].FileName, L"..")) continue;
    out.push_back(EntryFromItem(items[i]));
  }
  return out;
}

std::vector<std::wstring> Panel::PanelNames(HANDLE panel, bool currentOnly) const {
  std::vector<std::wstring> names;
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(panel, FCTL_GETPANELINFO, 0, &pi)) return names;
  auto fetch = [&](FILE_CONTROL_COMMANDS cmd, intptr_t index) {
    intptr_t size = PsInfo.PanelControl(panel, cmd, index, nullptr);
    if (size <= 0) return;
    std::vector<std::max_align_t> buffer((static_cast<size_t>(size) + sizeof(std::max_align_t) - 1) /
                                         sizeof(std::max_align_t));
    FarGetPluginPanelItem gpi{sizeof(gpi), static_cast<size_t>(size),
                              reinterpret_cast<PluginPanelItem*>(buffer.data())};
    if (!PsInfo.PanelControl(panel, cmd, index, &gpi)) return;
    if (gpi.Item->FileName && wcscmp(gpi.Item->FileName, L"..")) names.emplace_back(gpi.Item->FileName);
  };
  if (currentOnly) {
    fetch(FCTL_GETCURRENTPANELITEM, 0);
  } else {
    for (size_t i = 0; i < pi.SelectedItemsNumber; ++i) fetch(FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(i));
  }
  return names;
}

std::vector<ps5::ListEntry> Panel::SelectedEntries(bool currentOnly) const {
  std::vector<ps5::ListEntry> out;
  for (const auto& name : PanelNames(PANEL_ACTIVE, currentOnly)) {
    if (const ps5::ListEntry* e = EntryByName(name)) out.push_back(*e);
  }
  return out;
}

void Panel::Refresh(bool keepSelection) {
  listingFresh_ = false;
  PsInfo.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, keepSelection ? 1 : 0, nullptr);
  PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
}

void Panel::GoToName(const std::wstring& name) {
  PanelInfo pi{sizeof(pi)};
  if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi)) return;
  for (size_t i = 0; i < pi.ItemsNumber; ++i) {
    intptr_t size = PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, static_cast<intptr_t>(i), nullptr);
    if (size <= 0) continue;
    std::vector<std::max_align_t> buffer((static_cast<size_t>(size) + sizeof(std::max_align_t) - 1) /
                                         sizeof(std::max_align_t));
    FarGetPluginPanelItem gpi{sizeof(gpi), static_cast<size_t>(size),
                              reinterpret_cast<PluginPanelItem*>(buffer.data())};
    if (!PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, static_cast<intptr_t>(i), &gpi)) continue;
    if (gpi.Item->FileName && name == gpi.Item->FileName) {
      PanelRedrawInfo ri{sizeof(ri), i, pi.TopPanelItem};
      PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, &ri);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Hosts level

void Panel::AddHost() {
  HostConfig host;
  if (!config::EditHost(host, true)) return;
  hosts_ = config::LoadHosts();
  std::wstring name = host.name;
  for (int i = 2;; ++i) {
    bool clash = std::any_of(hosts_.begin(), hosts_.end(), [&](const HostConfig& h) { return h.name == host.name; });
    if (!clash) break;
    host.name = name + L" (" + std::to_wstring(i) + L")";
  }
  hosts_.push_back(host);
  config::SaveHosts(hosts_);
  newNameW_ = host.name;
}

void Panel::EditCurrentHost() {
  auto names = PanelNames(PANEL_ACTIVE, true);
  if (names.empty()) return;
  hosts_ = config::LoadHosts();
  for (auto& h : hosts_) {
    if (h.name != names[0]) continue;
    HostConfig edited = h;
    if (!config::EditHost(edited, false)) return;
    h = edited;
    config::SaveHosts(hosts_);
    PsInfo.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, 1, nullptr);
    PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
    GoToName(edited.name);
    return;
  }
}

// ---------------------------------------------------------------------------
// Delete / mkdir

intptr_t Panel::DeleteFiles(const DeleteFilesInfo* info) {
  bool silent = (info->OpMode & OPM_SILENT) != 0;
  ApplyCodePage();
  if (level_ == Level::Hosts) {
    std::set<std::wstring> names;
    for (size_t i = 0; i < info->ItemsNumber; ++i) {
      if (info->PanelItem[i].FileName && wcscmp(info->PanelItem[i].FileName, L".."))
        names.insert(info->PanelItem[i].FileName);
    }
    if (names.empty()) return FALSE;
    std::wstring what = names.size() == 1 ? L"\"" + *names.begin() + L"\"" : std::to_wstring(names.size()) + L" consoles";
    if (!silent && !fui::Confirm(L"Delete", L"Remove " + what + L" from the list?", L"Delete", true)) return FALSE;
    hosts_ = config::LoadHosts();
    hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(), [&](const HostConfig& h) { return names.count(h.name) > 0; }),
                 hosts_.end());
    config::SaveHosts(hosts_);
    return TRUE;
  }
  if (level_ == Level::Volumes) {
    if (!silent) fui::ShowError(L"Delete", L"Volumes cannot be deleted.");
    return FALSE;
  }
  std::vector<ps5::ListEntry> entries = EntriesFromItems(info->PanelItem, info->ItemsNumber);
  if (entries.empty()) return FALSE;
  if (!silent) {
    std::wstring what;
    if (entries.size() == 1) {
      what = (entries[0].isDir() ? L"the folder\n" : L"the file\n") + codec::RawToDisplay(entries[0].name);
    } else {
      what = std::to_wstring(entries.size()) + L" items";
    }
    if (fui::Message(FMSG_WARNING, {L"Delete", L"Do you wish to delete " + what + L"?"},
                     {L"&Delete", L"Cancel"}, MessageGuid) != 0) {
      return FALSE;
    }
  }
  bool ok = false;
  Run(L"Delete", silent, [&] {
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    uint64_t id = Client().Delete(paths);
    ok = ReportTask(WaitTask(id, L"Delete"), L"Delete");
  });
  listingFresh_ = false;
  return ok ? TRUE : FALSE;
}

intptr_t Panel::MakeDirectory(MakeDirectoryInfo* info) {
  bool silent = (info->OpMode & OPM_SILENT) != 0;
  ApplyCodePage();
  if (level_ == Level::Hosts) {
    newNameW_.clear();
    AddHost();
    if (newNameW_.empty()) return -1;
    info->Name = newNameW_.c_str();
    return 1;
  }
  if (level_ == Level::Volumes) {
    if (!silent) fui::ShowError(L"Make folder", L"Open a volume first.");
    return -1;
  }
  std::wstring name = info->Name ? info->Name : L"";
  if (!silent && !fui::Input(L"Make folder", L"Create the folder:", name, L"NewFolder")) return -1;
  name = Trim(Slashes(name));
  std::vector<std::wstring> parts;
  size_t start = 0;
  while (start <= name.size()) {
    size_t slash = name.find(L'/', start);
    std::wstring part = Trim(name.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start));
    if (!part.empty()) parts.push_back(part);
    if (slash == std::wstring::npos) break;
    start = slash + 1;
  }
  if (parts.empty()) return -1;
  bool ok = Run(L"Make folder", silent, [&] {
    std::string parent = cur_;
    for (size_t i = 0; i < parts.size(); ++i) {
      std::string raw = codec::DisplayToRaw(parts[i]);
      try {
        Client().Mkdir(parent, raw);
      } catch (const ps5::ApiError& e) {
        if (i + 1 == parts.size() || e.code() == "active_task") throw;
      }
      parent = codec::Join(parent, raw);
    }
  });
  listingFresh_ = false;
  if (!ok) return -1;
  newNameW_ = parts[0];
  info->Name = newNameW_.c_str();
  return 1;
}

// ---------------------------------------------------------------------------
// Download (GetFiles)

intptr_t Panel::GetFiles(GetFilesInfo* info) {
  ApplyCodePage();
  OPERATION_MODES mode = info->OpMode;
  bool quiet = (mode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;
  if (level_ != Level::Dir) {
    if (!quiet) fui::ShowError(kTitle, level_ == Level::Hosts ? L"Select a console first." : L"Open a volume first.");
    return 0;
  }
  std::vector<ps5::ListEntry> roots = EntriesFromItems(info->PanelItem, info->ItemsNumber);
  if (roots.empty()) return 0;

  std::wstring dest = info->DestPath ? info->DestPath : L"";
  if (!quiet) {
    std::wstring what = roots.size() == 1 ? L"\"" + codec::RawToDisplay(roots[0].name) + L"\""
                                          : std::to_wstring(roots.size()) + L" items";
    std::wstring prompt = (info->Move ? L"Move " : L"Copy ") + what + L" to:";
    if (!dest.empty() && dest.back() != L'\\') dest += L"\\";
    if (!fui::Input(info->Move ? L"Move from PS5" : L"Copy from PS5", prompt, dest, L"Copy",
                    FIB_EDITPATH)) {
      return -1;
    }
    dest = Trim(dest);
    if (dest.empty()) return -1;
  }
  destPathW_ = dest;
  info->DestPath = destPathW_.c_str();

  if (mode & (OPM_EDIT)) {
    for (const auto& r : roots) {
      editSnapshots_[codec::RawToDisplay(r.name)] = {r.path, r.size, r.mtime};
    }
  }
  return Download(roots, FullLocalPath(dest), info->Move != FALSE, mode) ? 1 : 0;
}

bool Panel::Download(const std::vector<ps5::ListEntry>& roots, const std::wstring& destInput, bool move,
                     OPERATION_MODES mode) {
  bool quiet = (mode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;
  bool showProgress = (mode & OPM_FIND) == 0;
  std::wstring title = move ? L"Move from PS5" : L"Copy from PS5";

  // "Copy as": one item to a path that is not an existing folder.
  std::wstring destDir = destInput;
  std::wstring renameTo;
  DWORD destAttrs = GetFileAttributesW(fui::LongPath(destInput).c_str());
  bool destIsDir = destAttrs != INVALID_FILE_ATTRIBUTES && (destAttrs & FILE_ATTRIBUTE_DIRECTORY);
  bool trailingSlash = !destInput.empty() && (destInput.back() == L'\\' || destInput.back() == L'/');
  if (roots.size() == 1 && !destIsDir && !trailingSlash) {
    size_t slash = destInput.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      destDir = destInput.substr(0, slash + 1);
      renameTo = destInput.substr(slash + 1);
    }
  }

  bool success = false;
  Run(title, (mode & OPM_FIND) != 0, [&] {
    if (!CreateDirs(destDir)) ThrowLastError(L"Cannot create the folder " + destDir);

    Progress progress(title, showProgress);
    progress.Status(L"Scanning...");
    std::vector<ps5::RemoteNode> nodes = ps5::WalkRemote(Client(), roots, [&](const std::string& dir) {
      progress.Status(L"Scanning " + codec::RawToDisplay(dir));
      return !progress.CheckCancel();
    });

    uint64_t totalBytes = 0, totalFiles = 0;
    for (const auto& n : nodes) {
      if (!n.entry.isDir()) {
        totalBytes += n.entry.size;
        ++totalFiles;
      }
    }
    progress.SetTotals(totalBytes, totalFiles);

    bool overwriteAll = quiet, skipAll = false;
    std::set<std::string> failedRoots;  // top-level names with skipped files

    for (const auto& node : nodes) {
      // Local path: each raw segment decoded for display and made Windows-safe.
      std::wstring local = destDir;
      size_t start = 0;
      bool first = true;
      std::string topName;
      while (start <= node.rel.size()) {
        size_t slash = node.rel.find('/', start);
        std::string seg = node.rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (first) topName = seg;
        std::wstring name = first && !renameTo.empty() ? renameTo : fui::SafeFileName(codec::RawToDisplay(seg));
        local = JoinLocal(local, name);
        first = false;
        if (slash == std::string::npos) break;
        start = slash + 1;
      }
      std::wstring remoteDisplay = codec::RawToDisplay(node.entry.path);

      if (node.entry.isDir()) {
        if (!CreateDirs(local)) ThrowLastError(L"Cannot create the folder " + local);
        continue;
      }

      for (;;) {
        try {
          WIN32_FILE_ATTRIBUTE_DATA fad{};
          bool exists = GetFileAttributesExW(fui::LongPath(local).c_str(), GetFileExInfoStandard, &fad) != FALSE;
          bool resume = false;
          if (exists) {
            if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
              throw std::runtime_error(fui::U(L"A folder with this name already exists:\n" + local));
            uint64_t localSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (skipAll) {
              failedRoots.insert(topName);
              progress.EndFile();
              break;
            }
            if (!overwriteAll) {
              bool canResume = localSize < node.entry.size;
              FILETIME lt;
              SYSTEMTIME st;
              wchar_t localTime[64] = L"";
              if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &lt) && FileTimeToSystemTime(&lt, &st))
                swprintf(localTime, 64, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
              std::vector<std::wstring> buttons{L"&Overwrite", L"&Skip"};
              if (canResume) buttons.push_back(L"&Resume");
              buttons.insert(buttons.end(), {L"Overwrite &all", L"S&kip all", L"&Cancel"});
              intptr_t r = fui::Message(
                  FMSG_WARNING,
                  {L"File exists", L"The file already exists:", local, L"\x1",
                   L"New:      " + fui::FormatSize(node.entry.size) + L"  " + fui::FormatTime(node.entry.mtime),
                   L"Existing: " + fui::FormatSize(localSize) + L"  " + localTime},
                  buttons, MessageGuid);
              progress.Draw(true);
              std::wstring choice = r >= 0 ? buttons[static_cast<size_t>(r)] : L"&Cancel";
              if (choice == L"&Cancel") throw ps5::CancelledError();
              if (choice == L"&Skip" || choice == L"S&kip all") {
                if (choice == L"S&kip all") skipAll = true;
                failedRoots.insert(topName);
                progress.EndFile();
                break;
              }
              if (choice == L"Overwrite &all") overwriteAll = true;
              if (choice == L"&Resume") resume = true;
            }
            if (!resume) SetFileAttributesW(fui::LongPath(local).c_str(), FILE_ATTRIBUTE_NORMAL);
          }

          std::wstring target = fui::LongPath(local);
          std::wstring temp = resume ? target : fui::LongPath(local + L".ps5part");
          File file(CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                resume ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
          if (!file.valid()) ThrowLastError(L"Cannot create " + local);
          uint64_t offset = 0;
          if (resume) {
            LARGE_INTEGER size{};
            GetFileSizeEx(file.get(), &size);
            offset = static_cast<uint64_t>(size.QuadPart);
            LARGE_INTEGER zero{};
            SetFilePointerEx(file.get(), zero, nullptr, FILE_END);
          }
          progress.BeginFile(remoteDisplay, local, node.entry.size, offset);
          try {
            for (int attempt = 0;; ++attempt) {
              try {
                Client().ReadFile(node.entry.path, offset, [&](const char* data, size_t size) {
                  DWORD written = 0;
                  if (!WriteFile(file.get(), data, static_cast<DWORD>(size), &written, nullptr) || written != size)
                    ThrowLastError(L"Cannot write " + local);
                  offset += size;
                  progress.AddBytes(size);
                  return !progress.CheckCancel();
                });
                break;
              } catch (const ps5::NetError&) {
                if (attempt >= 2) throw;
                progress.Status(L"Connection lost, retrying...");
                SleepMs(1000);
                progress.BeginFile(remoteDisplay, local, node.entry.size, 0);
              }
            }
            FILETIME ft = fui::UnixToFileTime(node.entry.mtime);
            SetFileTime(file.get(), nullptr, nullptr, &ft);
            file.Close();
            if (temp != target && !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
              ThrowLastError(L"Cannot replace " + local);
          } catch (...) {
            file.Close();
            if (temp != target) DeleteFileW(temp.c_str());
            throw;
          }
          progress.EndFile();
          break;
        } catch (const ps5::CancelledError&) {
          throw;
        } catch (const std::exception& e) {
          if (quiet) throw;
          std::wstring text = W(e.what());
          if (auto api = dynamic_cast<const ps5::ApiError*>(&e)) text = W(ps5::DescribeApiError(*api));
          intptr_t r = fui::Message(FMSG_WARNING, {title, L"Cannot copy", remoteDisplay, text},
                                    {L"&Retry", L"&Skip", L"&Cancel"}, MessageGuid);
          progress.Draw(true);
          if (r == 0) continue;
          if (r == 1) {
            failedRoots.insert(topName);
            progress.EndFile();
            break;
          }
          throw ps5::CancelledError();
        }
      }
    }

    if (move) {
      std::vector<std::string> paths;
      for (const auto& r : roots) {
        if (!failedRoots.count(r.name)) paths.push_back(r.path);
      }
      if (!paths.empty()) {
        uint64_t id = Client().Delete(paths);
        progress.Status(L"Deleting the originals...");
        ReportTask(WaitTask(id, title), title);
      }
      listingFresh_ = false;
    }
    success = failedRoots.empty();
  });
  return success;
}

// ---------------------------------------------------------------------------
// Upload (PutFiles)

intptr_t Panel::PutFiles(const PutFilesInfo* info) {
  ApplyCodePage();
  bool silent = (info->OpMode & OPM_SILENT) != 0;
  if (level_ != Level::Dir) {
    if (!silent) fui::ShowError(kTitle, level_ == Level::Hosts ? L"Select a console first." : L"Open a volume first.");
    return 0;
  }
  bool ok = Upload(info);
  listingFresh_ = false;
  return ok ? 1 : 0;
}

bool Panel::Upload(const PutFilesInfo* info) {
  bool silent = (info->OpMode & OPM_SILENT) != 0;
  bool edit = (info->OpMode & OPM_EDIT) != 0;
  std::wstring title = info->Move ? L"Move to PS5" : L"Copy to PS5";
  std::wstring srcPath = info->SrcPath ? info->SrcPath : L"";

  struct LocalNode {
    std::wstring path;
    std::string rel;  // UTF-8, '/' separated
    uint64_t size = 0;
    bool dir = false;
    size_t top = 0;
  };
  struct Top {
    std::wstring path;
    std::wstring name;
    bool dir = false;
    bool keep = true;
  };
  std::vector<Top> tops;
  for (size_t i = 0; i < info->ItemsNumber; ++i) {
    const PluginPanelItem& item = info->PanelItem[i];
    if (!item.FileName || !wcscmp(item.FileName, L"..")) continue;
    std::wstring name = item.FileName;
    std::wstring path = IsAbsoluteLocal(name) ? name : JoinLocal(srcPath, name);
    tops.push_back({path, Leaf(name), (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, true});
  }
  if (tops.empty()) return false;

  bool success = false;
  Run(title, false, [&] {
    Progress progress(title);
    progress.Status(L"Scanning...");

    // Conflicts against a fresh listing of the target folder.
    ps5::Listing remote = Client().List(cur_);
    std::map<std::string, const ps5::ListEntry*> remoteByName;
    for (const auto& e : remote.entries) remoteByName[e.name] = &e;

    bool overwrite = false;
    bool overwriteAll = silent || edit, skipAll = false;
    for (auto& t : tops) {
      std::string rawName = codec::DisplayToRaw(t.name);
      auto it = remoteByName.find(rawName);
      if (it == remoteByName.end()) continue;
      const ps5::ListEntry& existing = *it->second;
      if (existing.isDir() != t.dir) {
        if (!t.dir || !existing.isDir()) {
          fui::ShowError(title, L"\"" + t.name + L"\" exists on the console as a " +
                                    (existing.isDir() ? L"folder" : L"file") + L" and will be skipped.");
          t.keep = false;
          continue;
        }
      }
      if (edit) {
        auto snap = editSnapshots_.find(t.name);
        if (snap != editSnapshots_.end() &&
            (snap->second.size != existing.size || snap->second.mtime != existing.mtime)) {
          if (fui::Message(FMSG_WARNING,
                           {title, L"The file changed on the console since it was opened:", t.name,
                            L"Overwrite it with your version?"},
                           {L"&Overwrite", L"&Cancel"}, MessageGuid) != 0) {
            throw ps5::CancelledError();
          }
        }
        overwrite = true;
        continue;
      }
      if (overwriteAll) {
        overwrite = true;
        continue;
      }
      if (skipAll) {
        t.keep = false;
        continue;
      }
      std::wstring kind = existing.isDir() ? L"The folder already exists on the console:" : L"The file already exists on the console:";
      std::vector<std::wstring> lines{L"Already exists", kind, t.name};
      if (!existing.isDir()) {
        lines.push_back(L"\x1");
        lines.push_back(L"Existing: " + fui::FormatSize(existing.size) + L"  " + fui::FormatTime(existing.mtime));
      }
      intptr_t r = fui::Message(FMSG_WARNING, lines,
                                {L"&Overwrite", L"&Skip", L"Overwrite &all", L"S&kip all", L"&Cancel"}, MessageGuid);
      progress.Draw(true);
      switch (r) {
        case 0: overwrite = true; break;
        case 1: t.keep = false; break;
        case 2: overwrite = overwriteAll = true; break;
        case 3: t.keep = false; skipAll = true; break;
        default: throw ps5::CancelledError();
      }
    }

    // Expand folders.
    std::vector<LocalNode> nodes;
    std::function<void(const std::wstring&, const std::string&, size_t)> walk =
        [&](const std::wstring& dir, const std::string& rel, size_t top) {
          progress.Status(L"Scanning " + dir);
          progress.ThrowIfCancelled();
          WIN32_FIND_DATAW fd;
          HANDLE find = FindFirstFileExW(fui::LongPath(JoinLocal(dir, L"*")).c_str(), FindExInfoBasic, &fd,
                                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
          if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
            ThrowLastError(L"Cannot read the folder " + dir);
          }
          std::vector<std::pair<std::wstring, std::string>> subdirs;
          do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = JoinLocal(dir, name);
            std::string childRel = rel + "/" + fui::U(name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
              if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // avoid link loops
              nodes.push_back({path, childRel, 0, true, top});
              subdirs.emplace_back(path, childRel);
            } else {
              uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
              nodes.push_back({path, childRel, size, false, top});
            }
          } while (FindNextFileW(find, &fd));
          FindClose(find);
          for (const auto& sub : subdirs) walk(sub.first, sub.second, top);
        };
    for (size_t i = 0; i < tops.size(); ++i) {
      const Top& t = tops[i];
      if (!t.keep) continue;
      std::string rel = fui::U(t.name);
      if (t.dir) {
        nodes.push_back({t.path, rel, 0, true, i});
        walk(t.path, rel, i);
      } else {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(fui::LongPath(t.path).c_str(), GetFileExInfoStandard, &fad))
          ThrowLastError(L"Cannot read " + t.path);
        uint64_t size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        nodes.push_back({t.path, rel, size, false, i});
      }
    }
    if (nodes.empty()) {
      success = true;
      return;
    }

    // Folders first (mkdir is refused while the upload task is active).
    for (const auto& n : nodes) {
      if (!n.dir) continue;
      size_t slash = n.rel.rfind('/');
      std::string parent = slash == std::string::npos ? cur_ : codec::Join(cur_, n.rel.substr(0, slash));
      std::string leaf = slash == std::string::npos ? n.rel : n.rel.substr(slash + 1);
      try {
        Client().Mkdir(parent, leaf);
      } catch (const ps5::ApiError& e) {
        if (e.code() == "active_task") throw;  // anything else: most likely it exists
      }
    }

    std::vector<const LocalNode*> files;
    std::vector<ps5::UploadItem> items;
    uint64_t totalBytes = 0;
    for (const auto& n : nodes) {
      if (n.dir) continue;
      files.push_back(&n);
      items.push_back({n.rel, n.size});
      totalBytes += n.size;
    }
    if (!files.empty()) {
      progress.SetTotals(totalBytes, files.size());
      uint64_t id = Client().UploadPrepare(cur_, items, overwrite);
      try {
        for (size_t i = 0; i < files.size(); ++i) {
          const LocalNode& n = *files[i];
          File file(CreateFileW(fui::LongPath(n.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
          if (!file.valid()) ThrowLastError(L"Cannot open " + n.path);
          progress.BeginFile(n.path, codec::RawToDisplay(codec::Join(cur_, n.rel)), n.size);
          Client().UploadFile(id, cur_, items[i], overwrite, [&](char* buffer, size_t capacity) -> size_t {
            DWORD read = 0;
            if (!ReadFile(file.get(), buffer, static_cast<DWORD>(capacity), &read, nullptr))
              ThrowLastError(L"Cannot read " + n.path);
            progress.AddBytes(read);
            progress.ThrowIfCancelled();
            return read;
          });
          progress.EndFile();
        }
        Client().UploadFinish(id);
      } catch (...) {
        try {
          Client().Cancel(id);
        } catch (const std::exception&) {
        }
        try {
          Client().UploadFinish(id);
        } catch (const std::exception&) {
        }
        throw;
      }
      ps5::TaskResult result = ps5::WaitForTask(
          Client(), id, [&](const ps5::TaskInfo* t) {
            if (t) progress.Status(L"Finishing upload...");
            return true;
          },
          SleepMs, 200);
      if (!ReportTask(result, title)) return;
    }

    if (info->Move) {
      for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->dir) {
          RemoveDirectoryW(fui::LongPath(it->path).c_str());
        } else {
          SetFileAttributesW(fui::LongPath(it->path).c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(fui::LongPath(it->path).c_str());
        }
      }
    }
    success = std::all_of(tops.begin(), tops.end(), [](const Top& t) { return t.keep; });
  });
  return success;
}

// ---------------------------------------------------------------------------
// Keys

intptr_t Panel::ProcessInput(const ProcessPanelInputInfo* info) {
  const INPUT_RECORD& rec = info->Rec;
  if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return FALSE;
  WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
  DWORD state = rec.Event.KeyEvent.dwControlKeyState;
  bool shift = (state & SHIFT_PRESSED) != 0;
  bool ctrl = (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  bool alt = (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  bool plain = !shift && !ctrl && !alt;
  bool shiftOnly = shift && !ctrl && !alt;
  ApplyCodePage();

  switch (level_) {
    case Level::Hosts:
      if ((plain && vk == VK_F4) || (shiftOnly && vk == VK_F6)) {
        EditCurrentHost();
        return TRUE;
      }
      if (shiftOnly && vk == VK_F4) {
        newNameW_.clear();
        AddHost();
        PsInfo.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, 1, nullptr);
        PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
        if (!newNameW_.empty()) GoToName(newNameW_);
        return TRUE;
      }
      if (plain && (vk == VK_F3 || vk == VK_F5 || vk == VK_F6)) return TRUE;
      return FALSE;

    case Level::Volumes:
      if (shiftOnly && vk == VK_F1) {
        ActionsMenu();
        return TRUE;
      }
      if (plain && (vk == VK_F3 || vk == VK_F4 || vk == VK_F5 || vk == VK_F6)) return TRUE;
      if (shiftOnly && (vk == VK_F4 || vk == VK_F5 || vk == VK_F6)) return TRUE;
      return FALSE;

    case Level::Dir:
      if (shiftOnly && vk == VK_F1) {
        ActionsMenu();
        return TRUE;
      }
      if (shiftOnly && vk == VK_F6) {
        RenameCurrent();
        return TRUE;
      }
      if (plain && (vk == VK_F5 || vk == VK_F6)) return RemoteCopyMove(vk == VK_F6) ? TRUE : FALSE;
      if (plain && vk == VK_RETURN && config::Opt().enterOpensPkgInfo) {
        if (PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETCMDLINE, 0, nullptr) > 1) return FALSE;
        auto current = SelectedEntries(true);
        if (current.size() == 1 && !current[0].isDir() &&
            Extension(codec::RawToDisplay(current[0].name)) == L".pkg") {
          PkgInfo(current[0]);
          return TRUE;
        }
      }
      return FALSE;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// On-console copy / move and rename

bool Panel::RemoteCopyMove(bool move) {
  Panel* other = Passive();
  if (!other || other == this || other->level_ != Level::Dir || other->session_ != session_) return false;
  std::vector<ps5::ListEntry> entries = SelectedEntries(false);
  if (entries.empty()) return true;

  std::wstring title = move ? L"Move on the console" : L"Copy on the console";
  std::string dst = other->cur_;
  std::wstring dstDisplay = codec::RawToDisplay(dst);
  if (config::Opt().confirmRemoteCopy) {
    std::wstring what = entries.size() == 1 ? L"\"" + codec::RawToDisplay(entries[0].name) + L"\""
                                            : std::to_wstring(entries.size()) + L" items";
    std::wstring input = dstDisplay;
    if (!fui::Input(title, (move ? L"Move " : L"Copy ") + what + L" to:", input, L"PS5FM.CopyTarget")) return true;
    input = Trim(input);
    if (input.empty()) return true;
    if (input != dstDisplay) dst = ResolveInput(input);
  }

  Run(title, false, [&] {
    bool overwrite = false;
    bool dstIsDir = false;
    ps5::Listing target;
    try {
      target = Client().List(dst);
      dstIsDir = true;
    } catch (const ps5::ApiError& e) {
      if (e.code() == "active_task") throw;
    }
    if (dstIsDir) {
      std::set<std::string> existing;
      for (const auto& e : target.entries) existing.insert(e.name);
      std::vector<std::wstring> conflicts;
      for (const auto& e : entries) {
        if (existing.count(e.name)) conflicts.push_back(codec::RawToDisplay(e.name));
      }
      if (!conflicts.empty()) {
        std::vector<std::wstring> lines{title, L"These items already exist in the destination:"};
        for (size_t i = 0; i < conflicts.size() && i < 5; ++i) lines.push_back(conflicts[i]);
        if (conflicts.size() > 5) lines.push_back(L"...and " + std::to_wstring(conflicts.size() - 5) + L" more");
        intptr_t r = fui::Message(FMSG_WARNING, lines, {L"&Overwrite", L"&Skip them", L"&Cancel"}, MessageGuid);
        if (r == 0) {
          overwrite = true;
        } else if (r == 1) {
          entries.erase(std::remove_if(entries.begin(), entries.end(),
                                       [&](const ps5::ListEntry& e) { return existing.count(e.name) > 0; }),
                        entries.end());
        } else {
          throw ps5::CancelledError();
        }
      }
    }
    if (entries.empty()) return;
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    uint64_t id = move ? Client().Move(paths, dst, overwrite) : Client().Copy(paths, dst, overwrite);
    ReportTask(WaitTask(id, title), title);
  });

  listingFresh_ = false;
  other->listingFresh_ = false;
  PsInfo.PanelControl(PANEL_ACTIVE, FCTL_UPDATEPANEL, 0, nullptr);
  PsInfo.PanelControl(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, nullptr);
  PsInfo.PanelControl(PANEL_PASSIVE, FCTL_UPDATEPANEL, 1, nullptr);
  PsInfo.PanelControl(PANEL_PASSIVE, FCTL_REDRAWPANEL, 0, nullptr);
  return true;
}

void Panel::RenameCurrent() {
  auto current = SelectedEntries(true);
  if (current.empty()) return;
  const ps5::ListEntry entry = current[0];
  std::wstring oldName = codec::RawToDisplay(entry.name);
  std::wstring input = oldName;
  if (!fui::Input(L"Rename", L"Rename or move \"" + oldName + L"\" to:", input, L"PS5FM.Rename")) return;
  input = Trim(input);
  if (input.empty() || input == oldName) return;
  bool isMove = input.find_first_of(L"/\\") != std::wstring::npos;
  std::wstring newName = isMove ? L"" : input;
  bool ok = Run(L"Rename", false, [&] {
    if (isMove) {
      uint64_t id = Client().Move({entry.path}, ResolveInput(input), false);
      ReportTask(WaitTask(id, L"Move"), L"Move");
    } else {
      Client().Rename(entry.path, codec::DisplayToRaw(input));
    }
  });
  Refresh(true);
  if (ok && !newName.empty()) GoToName(newName);
}

// ---------------------------------------------------------------------------
// Actions menu

void Panel::ActionsMenu() {
  ApplyCodePage();
  if (level_ == Level::Hosts || !session_) {
    fui::ShowInfo(kTitle, L"Open a console first.");
    return;
  }
  std::vector<ps5::ListEntry> selected = level_ == Level::Dir ? SelectedEntries(false) : std::vector<ps5::ListEntry>{};
  std::vector<ps5::ListEntry> pkgs, archives;
  for (const auto& e : selected) {
    if (e.isDir()) continue;
    std::wstring ext = Extension(codec::RawToDisplay(e.name));
    if (ext == L".pkg") pkgs.push_back(e);
    if (ext == L".zip" || ext == L".rar" || ext == L".7z" || ext == L".tar" || ext == L".gz" || ext == L".tgz" ||
        ext == L".xz" || ext == L".bz2" || ext == L".zst")
      archives.push_back(e);
  }
  bool haveFile = selected.size() == 1 && !selected[0].isDir();

  enum Action { InstallPkg, PkgInfoItem, LaunchElfItem, Extract, Chmod, Space, Task, Browser, Stop };
  std::vector<std::wstring> items;
  std::vector<int> actions;
  auto add = [&](const std::wstring& text, int action) {
    items.push_back(text);
    actions.push_back(action);
  };
  if (level_ == Level::Dir) {
    add(L"&Install PKG" + (pkgs.size() > 1 ? L" (" + std::to_wstring(pkgs.size()) + L" files)" : std::wstring()), InstallPkg);
    add(L"PKG in&formation", PkgInfoItem);
    add(L"&Launch ELF payload", LaunchElfItem);
    add(L"E&xtract archive...", Extract);
    add(L"Change &permissions...", Chmod);
    add(L"-", -1);
  }
  add(L"Free &space", Space);
  add(L"Running &task...", Task);
  add(L"Open &web UI in browser", Browser);
  add(L"Stop the pa&yload", Stop);

  intptr_t index = fui::Menu(L"PS5 actions", items, session_->host.Endpoint());
  if (index < 0 || static_cast<size_t>(index) >= actions.size()) return;
  switch (actions[static_cast<size_t>(index)]) {
    case InstallPkg:
      if (pkgs.empty()) fui::ShowError(L"Install PKG", L"Select one or more .pkg files.");
      else InstallPkgs(pkgs);
      break;
    case PkgInfoItem:
      if (pkgs.empty()) fui::ShowError(L"PKG information", L"Put the cursor on a .pkg file.");
      else PkgInfo(pkgs[0]);
      break;
    case LaunchElfItem:
      if (!haveFile || Extension(codec::RawToDisplay(selected[0].name)) != L".elf")
        fui::ShowError(L"Launch ELF", L"Put the cursor on an .elf file.");
      else LaunchElf(selected[0]);
      break;
    case Extract:
      if (archives.empty() && selected.empty()) fui::ShowError(L"Extract", L"Select one or more archives.");
      else ExtractDialog(archives.empty() ? selected : archives);
      break;
    case Chmod:
      if (selected.empty()) fui::ShowError(L"Permissions", L"Select files or folders.");
      else ChmodDialog(selected);
      break;
    case Space: ShowSpace(); break;
    case Task: ShowRunningTask(); break;
    case Browser: {
      std::wstring url = L"http://" + session_->host.Endpoint() + L"/";
      ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    }
    case Stop: StopPayload(); return;
    default: break;
  }
}

void Panel::PkgInfo(const ps5::ListEntry& entry) {
  ps5::JsonValue json;
  if (!Run(L"PKG information", false, [&] { json = Client().PkgInfo(entry.path); })) return;
  std::vector<std::wstring> lines{L"PKG information", codec::RawToDisplay(entry.name), L"\x1"};
  auto text = [](const ps5::JsonValue& v) { return codec::RawToDisplay(codec::TokenToRaw(v.toDisplay())); };
  std::wstring titleName, titleId, version;
  for (const auto& field : json["fields"].items()) {
    std::string name = field["name"].asString();
    if (name == "TITLE" && titleName.empty()) titleName = text(field["value"]);
    if (name == "TITLE_ID") titleId = text(field["value"]);
    if (name == "APP_VER" || name == "VERSION" || name == "contentVersion") version = text(field["value"]);
  }
  if (!titleName.empty()) lines.push_back(L"Title:      " + titleName);
  if (!titleId.empty()) lines.push_back(L"Title ID:   " + titleId);
  lines.push_back(L"Content ID: " + text(json["content_id"]));
  lines.push_back(L"Platform:   " + text(json["platform"]));
  if (!version.empty()) lines.push_back(L"Version:    " + version);
  lines.push_back(L"Size:       " + fui::FormatSize(json["size"].asUInt()));
  size_t extra = 0;
  for (const auto& field : json["fields"].items()) {
    std::string name = field["name"].asString();
    if (name == "TITLE" || name == "TITLE_ID" || name == "APP_VER" || name == "VERSION" || name == "contentVersion")
      continue;
    if (++extra > 8) break;
    std::wstring value = text(field["value"]);
    if (value.size() > 50) value = value.substr(0, 47) + L"...";
    lines.push_back(W(name) + L": " + value);
  }
  if (fui::Message(FMSG_LEFTALIGN, lines, {L"&Install", L"Close"}, MessageGuid) == 0) InstallPkgs({entry});
}

void Panel::InstallPkgs(const std::vector<ps5::ListEntry>& entries) {
  std::wstring what = entries.size() == 1 ? codec::RawToDisplay(entries[0].name)
                                          : std::to_wstring(entries.size()) + L" packages";
  if (!fui::Confirm(L"Install PKG", L"Install " + what + L" on the console?", L"&Install")) return;
  Run(L"Install PKG", false, [&] {
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    std::vector<uint64_t> ids = Client().InstallPkg(paths);
    try {
      for (uint64_t id : ids) ReportTask(WaitTask(id, L"Installing package"), L"Install PKG");
    } catch (const ps5::CancelledError&) {
      fui::ShowInfo(L"Install PKG", L"The installation continues on the console.");
      return;
    }
    fui::ShowInfo(L"Install PKG", L"Done. Check the console's notifications for the result.");
  });
}

void Panel::LaunchElf(const ps5::ListEntry& entry) {
  if (!fui::Confirm(L"Launch ELF", L"Send " + codec::RawToDisplay(entry.name) + L" to the ELF loader?", L"&Launch"))
    return;
  if (Run(L"Launch ELF", false, [&] { Client().LaunchElf(entry.path); }))
    fui::ShowInfo(L"Launch ELF", L"The payload was sent to the ELF loader.");
}

void Panel::ExtractDialog(const std::vector<ps5::ListEntry>& entries) {
  std::wstring dest = codec::RawToDisplay(cur_);
  std::wstring what = entries.size() == 1 ? codec::RawToDisplay(entries[0].name)
                                          : std::to_wstring(entries.size()) + L" archives";
  std::wstring label = L"Extract " + what + L" to:";
  std::vector<FarDialogItem> items{
      fui::Item(DI_DOUBLEBOX, 3, 1, 66, 11, L"Extract on the console"),
      fui::Item(DI_TEXT, 5, 2, 0, 2, label.c_str()),
      fui::Item(DI_EDIT, 5, 3, 64, 3, dest.c_str(), DIF_HISTORY, 0, L"PS5FM.ExtractTarget"),
      fui::Item(DI_CHECKBOX, 5, 4, 0, 4, L"Extract each archive into its own &folder", DIF_NONE, 1),
      fui::Item(DI_CHECKBOX, 5, 5, 0, 5, L"&Overwrite existing files", DIF_NONE, 0),
      fui::Item(DI_TEXT, 5, 6, 0, 6, L"&Password (if encrypted):"),
      fui::Item(DI_PSWEDIT, 5, 7, 64, 7, L""),
      fui::Item(DI_TEXT, -1, 9, 0, 9, L"", DIF_SEPARATOR),
      fui::Item(DI_BUTTON, 0, 10, 0, 10, L"E&xtract", DIF_CENTERGROUP | DIF_DEFAULTBUTTON),
      fui::Item(DI_BUTTON, 0, 10, 0, 10, L"Cancel", DIF_CENTERGROUP),
  };
  fui::DialogResult r = fui::RunDialog(ExtractDialogGuid, 70, 13, nullptr, items);
  if (r.exitCode != 8) return;
  std::wstring target = Trim(r.texts[2]);
  if (target.empty()) return;
  std::string rawDest = target == dest ? cur_ : ResolveInput(target);
  Run(L"Extract", false, [&] {
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    uint64_t id = Client().Extract(paths, rawDest, r.checks[3], r.checks[4], fui::U(r.texts[6]));
    ReportTask(WaitTask(id, L"Extract"), L"Extract");
  });
  Refresh(true);
}

void Panel::ChmodDialog(const std::vector<ps5::ListEntry>& entries) {
  std::wstring mode = W(ps5::FormatOctalMode(entries[0].mode));
  bool anyDir = std::any_of(entries.begin(), entries.end(), [](const ps5::ListEntry& e) { return e.isDir(); });
  std::wstring what = entries.size() == 1 ? codec::RawToDisplay(entries[0].name)
                                          : std::to_wstring(entries.size()) + L" items";
  std::wstring label = L"Permissions for " + what + L" (octal, e.g. 0755):";
  std::vector<FarDialogItem> items{
      fui::Item(DI_DOUBLEBOX, 3, 1, 60, 8, L"Change permissions"),
      fui::Item(DI_TEXT, 5, 2, 0, 2, label.c_str()),
      fui::Item(DI_EDIT, 5, 3, 12, 3, mode.c_str()),
      fui::Item(DI_CHECKBOX, 5, 4, 0, 4, L"Apply to folder contents (&recursive)", anyDir ? DIF_NONE : DIF_DISABLE, 0),
      fui::Item(DI_TEXT, -1, 6, 0, 6, L"", DIF_SEPARATOR),
      fui::Item(DI_BUTTON, 0, 7, 0, 7, L"OK", DIF_CENTERGROUP | DIF_DEFAULTBUTTON),
      fui::Item(DI_BUTTON, 0, 7, 0, 7, L"Cancel", DIF_CENTERGROUP),
  };
  fui::DialogResult r = fui::RunDialog(ChmodDialogGuid, 64, 10, nullptr, items);
  if (r.exitCode != 5) return;
  uint32_t value = 0;
  if (!ps5::ParseOctalMode(fui::U(Trim(r.texts[2])), value)) {
    fui::ShowError(L"Permissions", L"Enter the mode as three or four octal digits, like 0755.");
    return;
  }
  Run(L"Permissions", false, [&] {
    std::vector<std::string> paths;
    for (const auto& e : entries) paths.push_back(e.path);
    uint64_t id = Client().Chmod(paths, value, r.checks[3]);
    ReportTask(WaitTask(id, L"Permissions"), L"Permissions");
  });
  Refresh(true);
}

void Panel::ShowSpace() {
  std::vector<ps5::SpaceInfo> spaces;
  if (!Run(L"Free space", false, [&] { spaces = Client().Space(); })) return;
  std::vector<std::wstring> lines{L"Free space on " + session_->host.name};
  for (const auto& s : spaces) {
    std::wstring line = codec::RawToDisplay(s.path);
    if (line.size() < 24) line.append(24 - line.size(), L' ');
    line += fui::FormatSize(s.free) + L" free of " + fui::FormatSize(s.total);
    lines.push_back(line);
  }
  fui::Message(FMSG_LEFTALIGN, lines, {L"OK"}, MessageGuid);
}

void Panel::ShowRunningTask() {
  ps5::TaskInfo task;
  bool found = false;
  if (!Run(L"Running task", false, [&] { found = ps5::FindActiveTask(Client(), task); })) return;
  if (!found) {
    fui::ShowInfo(L"Running task", L"No task is running on the console.");
    return;
  }
  WatchBusy(L"Running task", true);
  Refresh(true);
}

void Panel::StopPayload() {
  if (!fui::Confirm(L"Stop the payload",
                    L"Stop the web file manager payload on " + session_->host.name +
                        L"?\nYou will need to load it again to reconnect.",
                    L"&Stop", true)) {
    return;
  }
  if (!Run(L"Stop the payload", false, [&] { Client().Exit(); })) return;
  PsInfo.PanelControl(PANEL_ACTIVE, FCTL_CLOSEPANEL, 0, nullptr);
}

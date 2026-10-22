#include "Config.hpp"

#include <cwchar>

#include "FarApi.hpp"
#include "Guids.hpp"

namespace config {

namespace {

class Settings {
public:
  Settings() {
    FarSettingsCreate create{sizeof(create), PluginGuid, INVALID_HANDLE_VALUE};
    if (PsInfo.SettingsControl(INVALID_HANDLE_VALUE, SCTL_CREATE, PSL_ROAMING, &create))
      handle_ = create.Handle;
  }
  ~Settings() {
    if (handle_ != INVALID_HANDLE_VALUE) PsInfo.SettingsControl(handle_, SCTL_FREE, 0, nullptr);
  }
  Settings(const Settings&) = delete;
  Settings& operator=(const Settings&) = delete;

  bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }

  std::wstring GetString(size_t root, const wchar_t* name, const std::wstring& fallback) {
    FarSettingsItem item{sizeof(item), root, name, FST_STRING};
    if (ok() && PsInfo.SettingsControl(handle_, SCTL_GET, 0, &item) && item.String) return item.String;
    return fallback;
  }

  unsigned long long GetNumber(size_t root, const wchar_t* name, unsigned long long fallback) {
    FarSettingsItem item{sizeof(item), root, name, FST_QWORD};
    if (ok() && PsInfo.SettingsControl(handle_, SCTL_GET, 0, &item)) return item.Number;
    return fallback;
  }

  void SetString(size_t root, const wchar_t* name, const std::wstring& value) {
    FarSettingsItem item{sizeof(item), root, name, FST_STRING};
    item.String = value.c_str();
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_SET, 0, &item);
  }

  void SetNumber(size_t root, const wchar_t* name, unsigned long long value) {
    FarSettingsItem item{sizeof(item), root, name, FST_QWORD};
    item.Number = value;
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_SET, 0, &item);
  }

  size_t OpenKey(size_t root, const wchar_t* name) {
    FarSettingsValue value{sizeof(value), root, name};
    return ok() ? static_cast<size_t>(PsInfo.SettingsControl(handle_, SCTL_OPENSUBKEY, 0, &value)) : 0;
  }

  size_t CreateKey(size_t root, const wchar_t* name) {
    FarSettingsValue value{sizeof(value), root, name};
    return ok() ? static_cast<size_t>(PsInfo.SettingsControl(handle_, SCTL_CREATESUBKEY, 0, &value)) : 0;
  }

  void DeleteKey(size_t root, const wchar_t* name) {
    FarSettingsValue value{sizeof(value), root, name};
    if (ok()) PsInfo.SettingsControl(handle_, SCTL_DELETE, 0, &value);
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

constexpr size_t kRoot = 0;  // FSSF_ROOT

}  // namespace

Options& Opt() {
  static Options options;
  return options;
}

void LoadOptions() {
  Settings s;
  Options& o = Opt();
  o.addToDiskMenu = s.GetNumber(kRoot, L"AddToDiskMenu", 1) != 0;
  o.confirmRemoteCopy = s.GetNumber(kRoot, L"ConfirmRemoteCopy", 1) != 0;
  o.enterOpensPkgInfo = s.GetNumber(kRoot, L"EnterOpensPkgInfo", 1) != 0;
  o.prefix = s.GetString(kRoot, L"Prefix", L"ps5");
  if (o.prefix.empty()) o.prefix = L"ps5";
}

void SaveOptions() {
  Settings s;
  const Options& o = Opt();
  s.SetNumber(kRoot, L"AddToDiskMenu", o.addToDiskMenu);
  s.SetNumber(kRoot, L"ConfirmRemoteCopy", o.confirmRemoteCopy);
  s.SetNumber(kRoot, L"EnterOpensPkgInfo", o.enterOpensPkgInfo);
  s.SetString(kRoot, L"Prefix", o.prefix);
}

std::vector<HostConfig> LoadHosts() {
  Settings s;
  std::vector<HostConfig> hosts;
  size_t list = s.OpenKey(kRoot, L"Hosts");
  if (!list) return hosts;
  size_t count = static_cast<size_t>(s.GetNumber(list, L"Count", 0));
  for (size_t i = 0; i < count; ++i) {
    std::wstring keyName = std::to_wstring(i);
    size_t key = s.OpenKey(list, keyName.c_str());
    if (!key) continue;
    HostConfig h;
    h.name = s.GetString(key, L"Name", L"");
    h.address = s.GetString(key, L"Address", L"");
    h.port = static_cast<uint16_t>(s.GetNumber(key, L"Port", 8888));
    h.codePage = static_cast<unsigned>(s.GetNumber(key, L"CodePage", 936));
    if (!h.address.empty()) {
      if (h.name.empty()) h.name = h.address;
      hosts.push_back(h);
    }
  }
  return hosts;
}

void SaveHosts(const std::vector<HostConfig>& hosts) {
  Settings s;
  s.DeleteKey(kRoot, L"Hosts");
  size_t list = s.CreateKey(kRoot, L"Hosts");
  if (!list) return;
  s.SetNumber(list, L"Count", hosts.size());
  for (size_t i = 0; i < hosts.size(); ++i) {
    std::wstring keyName = std::to_wstring(i);
    size_t key = s.CreateKey(list, keyName.c_str());
    if (!key) continue;
    s.SetString(key, L"Name", hosts[i].name);
    s.SetString(key, L"Address", hosts[i].address);
    s.SetNumber(key, L"Port", hosts[i].port);
    s.SetNumber(key, L"CodePage", hosts[i].codePage);
  }
}

namespace {
std::wstring Trim(const std::wstring& text) {
  size_t a = text.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return L"";
  size_t b = text.find_last_not_of(L" \t");
  return text.substr(a, b - a + 1);
}
}  // namespace

bool EditHost(HostConfig& host, bool isNew) {
  std::wstring port = std::to_wstring(host.port);
  std::wstring codePage = std::to_wstring(host.codePage);
  for (;;) {
    std::vector<FarDialogItem> items{
        fui::Item(DI_DOUBLEBOX, 3, 1, 60, 11, isNew ? L"Add PS5 console" : L"Edit PS5 console"),
        fui::Item(DI_TEXT, 5, 2, 0, 2, L"&Name:"),
        fui::Item(DI_EDIT, 5, 3, 58, 3, host.name.c_str(), DIF_FOCUS, 0, L"PS5FM.HostName"),
        fui::Item(DI_TEXT, 5, 4, 0, 4, L"&Address (IP or host name):"),
        fui::Item(DI_EDIT, 5, 5, 58, 5, host.address.c_str(), DIF_HISTORY, 0, L"PS5FM.HostAddress"),
        fui::Item(DI_TEXT, 5, 6, 0, 6, L"&Port:"),
        fui::Item(DI_EDIT, 5, 7, 14, 7, port.c_str()),
        fui::Item(DI_TEXT, 18, 6, 0, 6, L"&Code page for non-UTF-8 names:"),
        fui::Item(DI_EDIT, 18, 7, 28, 7, codePage.c_str()),
        fui::Item(DI_TEXT, -1, 8, 0, 8, L"", DIF_SEPARATOR),
        fui::Item(DI_BUTTON, 0, 9, 0, 9, L"OK", DIF_CENTERGROUP | DIF_DEFAULTBUTTON),
        fui::Item(DI_BUTTON, 0, 9, 0, 9, L"Cancel", DIF_CENTERGROUP),
    };
    fui::DialogResult r = fui::RunDialog(HostDialogGuid, 64, 13, nullptr, items);
    if (r.exitCode != 10) return false;
    HostConfig edited = host;
    edited.name = Trim(r.texts[2]);
    edited.address = Trim(r.texts[4]);
    port = Trim(r.texts[6]);
    codePage = Trim(r.texts[8]);
    unsigned long portValue = wcstoul(port.c_str(), nullptr, 10);
    unsigned long cpValue = wcstoul(codePage.c_str(), nullptr, 10);
    host.name = edited.name;
    host.address = edited.address;
    if (edited.address.empty()) {
      fui::ShowError(L"PS5", L"Enter the console's IP address or host name.");
      continue;
    }
    if (portValue == 0 || portValue > 65535) {
      fui::ShowError(L"PS5", L"The port must be a number from 1 to 65535.");
      continue;
    }
    if (edited.name.empty()) edited.name = edited.address;
    edited.port = static_cast<uint16_t>(portValue);
    edited.codePage = cpValue ? static_cast<unsigned>(cpValue) : 936;
    host = edited;
    return true;
  }
}

bool ConfigureDialog() {
  Options o = Opt();
  std::vector<FarDialogItem> items{
      fui::Item(DI_DOUBLEBOX, 3, 1, 56, 10, L"PS5 File Manager"),
      fui::Item(DI_CHECKBOX, 5, 2, 0, 2, L"Add to &disk menus", DIF_NONE, o.addToDiskMenu),
      fui::Item(DI_CHECKBOX, 5, 3, 0, 3, L"&Confirm copy/move on the console", DIF_NONE, o.confirmRemoteCopy),
      fui::Item(DI_CHECKBOX, 5, 4, 0, 4, L"&Enter on a .pkg file shows PKG info", DIF_NONE, o.enterOpensPkgInfo),
      fui::Item(DI_TEXT, 5, 6, 0, 6, L"Command line &prefix:"),
      fui::Item(DI_EDIT, 28, 6, 40, 6, o.prefix.c_str()),
      fui::Item(DI_TEXT, -1, 8, 0, 8, L"", DIF_SEPARATOR),
      fui::Item(DI_BUTTON, 0, 9, 0, 9, L"OK", DIF_CENTERGROUP | DIF_DEFAULTBUTTON),
      fui::Item(DI_BUTTON, 0, 9, 0, 9, L"Cancel", DIF_CENTERGROUP),
  };
  fui::DialogResult r = fui::RunDialog(ConfigDialogGuid, 60, 12, nullptr, items);
  if (r.exitCode != 7) return false;
  o.addToDiskMenu = r.checks[1];
  o.confirmRemoteCopy = r.checks[2];
  o.enterOpensPkgInfo = r.checks[3];
  o.prefix = Trim(r.texts[5]);
  if (o.prefix.empty()) o.prefix = L"ps5";
  Opt() = o;
  SaveOptions();
  return true;
}

}  // namespace config

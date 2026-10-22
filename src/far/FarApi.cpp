#include "FarApi.hpp"

#include <cwchar>

#include "../core/Text.hpp"
#include "Guids.hpp"

PluginStartupInfo PsInfo;
FarStandardFunctions FSF;

namespace far {

std::wstring W(std::string_view utf8) {
  return ps5::Utf8ToWide(utf8);
}

std::string U(std::wstring_view wide) {
  return ps5::WideToUtf8(wide);
}

intptr_t Message(FARMESSAGEFLAGS flags, const std::vector<std::wstring>& lines,
                 const std::vector<std::wstring>& buttons, const GUID& id) {
  std::vector<const wchar_t*> items;
  items.reserve(lines.size() + buttons.size());
  for (const auto& line : lines) items.push_back(line.c_str());
  for (const auto& button : buttons) items.push_back(button.c_str());
  return PsInfo.Message(&PluginGuid, &id, flags, nullptr, items.data(), items.size(),
                        static_cast<intptr_t>(buttons.size()));
}

namespace {
std::vector<std::wstring> SplitLines(const std::wstring& title, const std::wstring& text) {
  std::vector<std::wstring> lines{title};
  size_t start = 0;
  for (;;) {
    size_t nl = text.find(L'\n', start);
    lines.push_back(text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
    if (nl == std::wstring::npos) break;
    start = nl + 1;
  }
  return lines;
}
}  // namespace

void ShowError(const std::wstring& title, const std::wstring& text) {
  Message(FMSG_WARNING, SplitLines(title, text), {L"OK"}, MessageGuid);
}

void ShowInfo(const std::wstring& title, const std::wstring& text) {
  Message(FMSG_LEFTALIGN, SplitLines(title, text), {L"OK"}, MessageGuid);
}

bool Confirm(const std::wstring& title, const std::wstring& text, const std::wstring& yes,
             bool warning) {
  return Message(warning ? FMSG_WARNING : FMSG_NONE, SplitLines(title, text), {yes, L"Cancel"},
                 MessageGuid) == 0;
}

bool Input(const std::wstring& title, const std::wstring& prompt, std::wstring& value,
           const wchar_t* history, INPUTBOXFLAGS flags) {
  std::vector<wchar_t> buffer(32768, L'\0');
  if (!PsInfo.InputBox(&PluginGuid, &InputGuid, title.c_str(), prompt.c_str(), history,
                       value.c_str(), buffer.data(), buffer.size(), nullptr,
                       flags | FIB_BUTTONS | FIB_NOUSELASTHISTORY)) {
    return false;
  }
  value = buffer.data();
  return true;
}

intptr_t Menu(const std::wstring& title, const std::vector<std::wstring>& items,
              const std::wstring& bottom, intptr_t selected, const std::vector<FarKey>& breakKeys,
              intptr_t* breakKey) {
  std::vector<FarMenuItem> menu(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    menu[i] = {};
    if (items[i] == L"-") {
      menu[i].Flags = MIF_SEPARATOR;
      menu[i].Text = L"";
    } else {
      menu[i].Text = items[i].c_str();
    }
    if (static_cast<intptr_t>(i) == selected) menu[i].Flags |= MIF_SELECTED;
  }
  std::vector<FarKey> keys = breakKeys;
  keys.push_back(FarKey{0, 0});
  intptr_t code = -1;
  intptr_t result = PsInfo.Menu(&PluginGuid, &MenuGuid, -1, -1, 0, FMENU_WRAPMODE,
                                title.c_str(), bottom.empty() ? nullptr : bottom.c_str(), nullptr,
                                breakKeys.empty() ? nullptr : keys.data(), &code, menu.data(),
                                menu.size());
  if (breakKey) *breakKey = breakKeys.empty() ? -1 : code;
  return result;
}

FarDialogItem Item(FARDIALOGITEMTYPES type, intptr_t x1, intptr_t y1, intptr_t x2, intptr_t y2,
                   const wchar_t* data, FARDIALOGITEMFLAGS flags, intptr_t selected,
                   const wchar_t* history) {
  FarDialogItem item{};
  item.Type = type;
  item.X1 = x1;
  item.Y1 = y1;
  item.X2 = x2;
  item.Y2 = y2;
  item.Selected = selected;
  item.History = history;
  item.Flags = flags;
  item.Data = data;
  return item;
}

namespace {
intptr_t WINAPI DefaultDialogProc(HANDLE hDlg, intptr_t msg, intptr_t param1, void* param2) {
  return PsInfo.DefDlgProc(hDlg, msg, param1, param2);
}
}  // namespace

DialogResult RunDialog(const GUID& id, intptr_t width, intptr_t height, const wchar_t* help,
                       std::vector<FarDialogItem>& items) {
  DialogResult result;
  HANDLE dlg = PsInfo.DialogInit(&PluginGuid, &id, -1, -1, width, height, help, items.data(),
                                 items.size(), 0, FDLG_NONE, DefaultDialogProc, nullptr);
  if (dlg == INVALID_HANDLE_VALUE || !dlg) return result;
  result.exitCode = PsInfo.DialogRun(dlg);
  result.texts.resize(items.size());
  result.checks.resize(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    switch (items[i].Type) {
      case DI_EDIT:
      case DI_FIXEDIT:
      case DI_PSWEDIT: {
        auto text = reinterpret_cast<const wchar_t*>(
            PsInfo.SendDlgMessage(dlg, DM_GETCONSTTEXTPTR, static_cast<intptr_t>(i), nullptr));
        result.texts[i] = text ? text : L"";
        break;
      }
      case DI_CHECKBOX:
        result.checks[i] =
            PsInfo.SendDlgMessage(dlg, DM_GETCHECK, static_cast<intptr_t>(i), nullptr) == BSTATE_CHECKED;
        break;
      default:
        break;
    }
  }
  PsInfo.DialogFree(dlg);
  return result;
}

bool EscPressed() {
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  bool escape = false;
  DWORD count = 0;
  while (GetNumberOfConsoleInputEvents(input, &count) && count) {
    INPUT_RECORD record;
    DWORD read = 0;
    if (!ReadConsoleInputW(input, &record, 1, &read) || !read) break;
    if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
        record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
      escape = true;
    }
  }
  return escape;
}

std::wstring LongPath(const std::wstring& path) {
  if (path.rfind(L"\\\\?\\", 0) == 0) return path;
  if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
  if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    return L"\\\\?\\" + path;
  return path;
}

std::wstring SafeFileName(std::wstring name) {
  for (auto& c : name) {
    if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
        c == L'|' || c == L'?' || c == L'*') {
      c = L'_';
    }
  }
  while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.back() = L'_';
  if (name.empty()) name = L"_";
  return name;
}

std::wstring FormatSize(uint64_t bytes) {
  return W(ps5::FormatSize(bytes));
}

FILETIME UnixToFileTime(int64_t unixTime) {
  ULARGE_INTEGER value;
  value.QuadPart = static_cast<ULONGLONG>((unixTime + 11644473600LL) * 10000000LL);
  FILETIME ft;
  ft.dwLowDateTime = value.LowPart;
  ft.dwHighDateTime = value.HighPart;
  return ft;
}

std::wstring FormatTime(int64_t unixTime) {
  FILETIME utc = UnixToFileTime(unixTime), local;
  SYSTEMTIME st;
  if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
  wchar_t buf[64];
  swprintf(buf, 64, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  return buf;
}

void TaskbarProgress(uint64_t done, uint64_t total) {
  // TBPF_NORMAL = 2
  PsInfo.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, 2, nullptr);
  ProgressValue value{sizeof(value), done, total ? total : 1};
  PsInfo.AdvControl(&PluginGuid, ACTL_SETPROGRESSVALUE, 0, &value);
}

void TaskbarClear() {
  // TBPF_NOPROGRESS = 0
  PsInfo.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, 0, nullptr);
  PsInfo.AdvControl(&PluginGuid, ACTL_PROGRESSNOTIFY, 0, nullptr);
}

}  // namespace far

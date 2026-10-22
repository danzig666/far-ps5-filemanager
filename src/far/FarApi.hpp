#pragma once

#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "plugin.hpp"

// Far services and small UI helpers shared by the plugin.

extern PluginStartupInfo PsInfo;
extern FarStandardFunctions FSF;

namespace fui {

std::wstring W(std::string_view utf8);  // UTF-8 -> UTF-16
std::string U(std::wstring_view wide);  // UTF-16 -> UTF-8

// Shows a message box. `lines[0]` is the title. Returns the index of the
// pressed button, or -1 on Esc.
intptr_t Message(FARMESSAGEFLAGS flags, const std::vector<std::wstring>& lines,
                 const std::vector<std::wstring>& buttons, const GUID& id = GUID{});

void ShowError(const std::wstring& title, const std::wstring& text);
void ShowInfo(const std::wstring& title, const std::wstring& text);
bool Confirm(const std::wstring& title, const std::wstring& text, const std::wstring& yes = L"OK",
             bool warning = false);

// Single-line input box. Returns false if cancelled.
bool Input(const std::wstring& title, const std::wstring& prompt, std::wstring& value,
           const wchar_t* history = nullptr, INPUTBOXFLAGS flags = FIB_NONE);

// Vertical menu. Returns the selected index or -1. `breakKey` receives the
// index into `breakKeys` when one of them closed the menu (else -1).
intptr_t Menu(const std::wstring& title, const std::vector<std::wstring>& items,
              const std::wstring& bottom = {}, intptr_t selected = 0,
              const std::vector<FarKey>& breakKeys = {}, intptr_t* breakKey = nullptr);

// Dialog item factory with the fields the plugin uses.
FarDialogItem Item(FARDIALOGITEMTYPES type, intptr_t x1, intptr_t y1, intptr_t x2, intptr_t y2,
                   const wchar_t* data = L"", FARDIALOGITEMFLAGS flags = DIF_NONE,
                   intptr_t selected = 0, const wchar_t* history = nullptr);

// Runs a dialog and returns the pressed item index (-1 on cancel). Edit
// texts and checkbox states are read back into `texts` / `checks`, indexed by
// item number.
struct DialogResult {
  intptr_t exitCode = -1;
  std::vector<std::wstring> texts;
  std::vector<bool> checks;
};
DialogResult RunDialog(const GUID& id, intptr_t width, intptr_t height, const wchar_t* help,
                       std::vector<FarDialogItem>& items);

// Returns VK_ESCAPE or VK_F8 if one was pressed since the last call, else 0.
// Other pending input is consumed, like Far's own copy progress does.
int PollKey();

// Prefixes \\?\ so local paths longer than MAX_PATH work.
std::wstring LongPath(const std::wstring& path);

// Replaces characters Windows does not allow in file names.
std::wstring SafeFileName(std::wstring name);

std::wstring FormatSize(uint64_t bytes);
std::wstring FormatTime(int64_t unixTime);

FILETIME UnixToFileTime(int64_t unixTime);

// Taskbar progress (ACTL_SETPROGRESSSTATE / VALUE).
void TaskbarProgress(uint64_t done, uint64_t total);
void TaskbarClear();

}  // namespace fui

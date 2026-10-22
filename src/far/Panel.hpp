#pragma once

#include <windows.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../core/Operations.hpp"
#include "../core/Ps5Client.hpp"
#include "Config.hpp"
#include "plugin.hpp"

class Progress;

// Connection state shared by all panels open on the same console.
struct Session {
  HostConfig host;
  std::unique_ptr<ps5::Ps5Client> client;
  std::set<std::string> smbUnlocked;  // SMB roots unlocked in this payload session
};

class Panel {
public:
  Panel();
  ~Panel();
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;

  // Panel handles Far knows about; used to validate handles from PanelInfo.
  static Panel* FromHandle(HANDLE handle);
  static Panel* Active();
  static Panel* Passive();

  // Opens on the list of saved consoles.
  bool OpenHosts();
  // Connects and shows the volumes, or `rawPath` if given.
  bool OpenTarget(const HostConfig& host, const std::string& rawPath);
  // Parses "address[:port][/path]" (command line) or shortcut data.
  static Panel* OpenFromText(const std::wstring& text, bool shortcut);

  void GetOpenPanelInfo(OpenPanelInfo* info);
  intptr_t GetFindData(GetFindDataInfo* info);
  void FreeFindData(const FreeFindDataInfo* info);
  intptr_t SetDirectory(const SetDirectoryInfo* info);
  intptr_t GetFiles(GetFilesInfo* info);
  intptr_t PutFiles(const PutFilesInfo* info);
  intptr_t DeleteFiles(const DeleteFilesInfo* info);
  intptr_t MakeDirectory(MakeDirectoryInfo* info);
  intptr_t ProcessInput(const ProcessPanelInputInfo* info);

  // Shift+F1, or F11 while a PS5 panel is active.
  void ActionsMenu();

private:
  enum class Level { Hosts, Volumes, Dir };

  struct Volume {
    std::wstring name;
    ps5::SpaceInfo info;
  };

  struct FindData {
    std::vector<PluginPanelItem> items;
    std::deque<std::wstring> strings;
  };

  struct EditSnapshot {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
  };

  ps5::Ps5Client& Client();
  void ApplyCodePage();
  void UpdateStrings();

  // Runs `fn`, reporting errors in a message box unless `silent`. Returns
  // false on any error or cancellation.
  bool Run(const std::wstring& title, bool silent, const std::function<void()>& fn);
  void HandleBusy(const std::wstring& title);
  ps5::TaskResult WaitTask(uint64_t id, const std::wstring& title, bool cancellable = true);
  bool ReportTask(const ps5::TaskResult& result, const std::wstring& title);

  // Navigation.
  void LoadVolumes();
  void EnterPath(const std::string& rawPath, bool silent);
  void GoUp(bool silent);
  void UnlockSmb(const std::string& rawPath, bool silent);
  const Volume* VolumeFor(const std::string& rawPath) const;
  void BuildNames();
  std::string ResolveInput(const std::wstring& text) const;

  // Items.
  const ps5::ListEntry* EntryByName(const std::wstring& name) const;
  ps5::ListEntry EntryFromItem(const PluginPanelItem& item) const;
  std::vector<ps5::ListEntry> EntriesFromItems(const PluginPanelItem* items, size_t count) const;
  std::vector<std::wstring> PanelNames(HANDLE panel, bool currentOnly) const;
  std::vector<ps5::ListEntry> SelectedEntries(bool currentOnly) const;
  void Refresh(bool keepSelection);
  void GoToName(const std::wstring& name);

  // Hosts level.
  void AddHost();
  void EditCurrentHost();

  // Operations.
  bool Download(const std::vector<ps5::ListEntry>& roots, const std::wstring& dest, bool move,
                OPERATION_MODES mode);
  bool Upload(const PutFilesInfo* info);
  bool RemoteCopyMove(bool move);
  void RenameCurrent();

  // Actions menu items.
  void PkgInfo(const ps5::ListEntry& entry);
  void InstallPkgs(const std::vector<ps5::ListEntry>& entries);
  void LaunchElf(const ps5::ListEntry& entry);
  void ExtractDialog(const std::vector<ps5::ListEntry>& entries);
  void ChmodDialog(const std::vector<ps5::ListEntry>& entries);
  void ShowSpace();
  void ShowRunningTask();
  void StopPayload();

  Level level_ = Level::Hosts;
  std::shared_ptr<Session> session_;
  std::vector<HostConfig> hosts_;
  std::vector<Volume> volumes_;
  bool volumesFresh_ = false;

  std::string cur_;  // raw path at Level::Dir
  bool fromRoot_ = false;
  ps5::Listing listing_;
  bool listingFresh_ = false;
  std::vector<std::wstring> names_;  // display names, parallel to listing_.entries
  std::vector<bool> lossy_;
  std::map<std::wstring, size_t> byName_;

  std::map<std::wstring, EditSnapshot> editSnapshots_;
  std::map<PluginPanelItem*, std::unique_ptr<FindData>> findData_;

  // Strings handed to Far must outlive the call.
  std::wstring curDirW_, titleW_, shortcutW_, destPathW_, newNameW_;
  std::vector<KeyBarLabel> keyLabels_;
  KeyBarTitles keyBar_{};
};

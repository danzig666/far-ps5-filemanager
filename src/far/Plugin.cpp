// Far Manager 3 entry points. Every export catches everything: an exception
// escaping into Far would take the whole file manager down.

#include <windows.h>

#include <memory>
#include <string>

#include "Config.hpp"
#include "FarApi.hpp"
#include "Guids.hpp"
#include "Panel.hpp"
#include "Updater.hpp"
#include "Version.hpp"
#include "plugin.hpp"

namespace {

Panel* P(HANDLE h) {
  return Panel::FromHandle(h);
}

}  // namespace

extern "C" {

void WINAPI GetGlobalInfoW(GlobalInfo* info) {
  info->StructSize = sizeof(*info);
  info->MinFarVersion = MAKEFARVERSION(3, 0, 0, PS5FM_MIN_FAR_BUILD, VS_RELEASE);
  info->Version = MAKEFARVERSION(PS5FM_VERSION_MAJOR, PS5FM_VERSION_MINOR, PS5FM_VERSION_REVISION,
                                 PS5FM_VERSION_BUILD, VS_RELEASE);
  info->Guid = PluginGuid;
  info->Title = L"PS5 File Manager";
  info->Description = L"Browse and manage a PS5 through the ps5-web-file-manager payload";
  info->Author = L"far-ps5-filemanager contributors";
}

void WINAPI SetStartupInfoW(const PluginStartupInfo* info) {
  PsInfo = *info;
  FSF = *info->FSF;
  PsInfo.FSF = &FSF;
  try {
    config::LoadOptions();
  } catch (...) {
  }
  try {
    updater::CleanupOldFiles();  // leftovers of earlier updates that no Far uses any more
  } catch (...) {
  }
}

void WINAPI GetPluginInfoW(PluginInfo* info) {
  static const wchar_t* diskStrings[] = {L"PS5"};
  static const wchar_t* menuStrings[] = {L"PS5 File Manager"};
  static const wchar_t* configStrings[] = {L"PS5 File Manager"};
  static std::wstring prefix;

  info->StructSize = sizeof(*info);
  info->Flags = PF_NONE;
  if (config::Opt().addToDiskMenu) {
    info->DiskMenu.Guids = &DiskMenuGuid;
    info->DiskMenu.Strings = diskStrings;
    info->DiskMenu.Count = 1;
  }
  info->PluginMenu.Guids = &PluginMenuGuid;
  info->PluginMenu.Strings = menuStrings;
  info->PluginMenu.Count = 1;
  info->PluginConfig.Guids = &ConfigMenuGuid;
  info->PluginConfig.Strings = configStrings;
  info->PluginConfig.Count = 1;
  prefix = config::Opt().prefix;
  info->CommandPrefix = prefix.c_str();
}

HANDLE WINAPI OpenW(const OpenInfo* info) {
  try {
    switch (info->OpenFrom) {
      case OPEN_PLUGINSMENU:
        if (Panel* active = Panel::Active()) {
          active->ActionsMenu();
          return nullptr;
        }
        [[fallthrough]];
      case OPEN_LEFTDISKMENU:
      case OPEN_RIGHTDISKMENU: {
        auto panel = std::make_unique<Panel>();
        if (!panel->OpenHosts()) return nullptr;
        return panel.release();
      }
      case OPEN_COMMANDLINE: {
        auto cl = reinterpret_cast<const OpenCommandLineInfo*>(info->Data);
        return Panel::OpenFromText(cl && cl->CommandLine ? cl->CommandLine : L"", false);
      }
      case OPEN_SHORTCUT: {
        auto sc = reinterpret_cast<const OpenShortcutInfo*>(info->Data);
        return Panel::OpenFromText(sc && sc->ShortcutData ? sc->ShortcutData : L"", true);
      }
      default:
        return nullptr;
    }
  } catch (const std::exception& e) {
    fui::ShowError(L"PS5", fui::W(e.what()));
  } catch (...) {
  }
  return nullptr;
}

void WINAPI ClosePanelW(const ClosePanelInfo* info) {
  delete P(info->hPanel);
}

void WINAPI GetOpenPanelInfoW(OpenPanelInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) p->GetOpenPanelInfo(info);
  } catch (...) {
  }
}

intptr_t WINAPI GetFindDataW(GetFindDataInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->GetFindData(info);
  } catch (...) {
  }
  return FALSE;
}

void WINAPI FreeFindDataW(const FreeFindDataInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) p->FreeFindData(info);
  } catch (...) {
  }
}

intptr_t WINAPI SetDirectoryW(const SetDirectoryInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->SetDirectory(info);
  } catch (...) {
  }
  return FALSE;
}

intptr_t WINAPI GetFilesW(GetFilesInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->GetFiles(info);
  } catch (...) {
  }
  return 0;
}

intptr_t WINAPI PutFilesW(const PutFilesInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->PutFiles(info);
  } catch (...) {
  }
  return 0;
}

intptr_t WINAPI DeleteFilesW(const DeleteFilesInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->DeleteFiles(info);
  } catch (...) {
  }
  return FALSE;
}

intptr_t WINAPI MakeDirectoryW(MakeDirectoryInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->MakeDirectory(info);
  } catch (...) {
  }
  return 0;
}

intptr_t WINAPI ProcessPanelInputW(const ProcessPanelInputInfo* info) {
  try {
    if (Panel* p = P(info->hPanel)) return p->ProcessInput(info);
  } catch (...) {
  }
  return FALSE;
}

intptr_t WINAPI ConfigureW(const ConfigureInfo*) {
  try {
    return config::ConfigureDialog() ? TRUE : FALSE;
  } catch (...) {
  }
  return FALSE;
}

}  // extern "C"

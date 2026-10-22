#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HostConfig {
  std::wstring name;
  std::wstring address;
  uint16_t port = 8888;
  unsigned codePage = 936;  // fallback for file names that are not UTF-8 (GBK, like the web UI)

  std::wstring Endpoint() const { return address + L":" + std::to_wstring(port); }
};

struct Options {
  bool addToDiskMenu = true;
  bool confirmRemoteCopy = true;
  bool enterOpensPkgInfo = true;
  std::wstring prefix = L"ps5";
};

namespace config {

Options& Opt();
void LoadOptions();
void SaveOptions();

std::vector<HostConfig> LoadHosts();
void SaveHosts(const std::vector<HostConfig>& hosts);

// Host editor dialog. Returns false if cancelled.
bool EditHost(HostConfig& host, bool isNew);

// F9 > Options > Plugin configuration.
bool ConfigureDialog();

}  // namespace config

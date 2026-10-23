#pragma once

// Per-plugin settings for Updater.cpp.

#include "../core/Json.hpp"
#include "../core/Text.hpp"
#include "Version.hpp"

namespace updcfg {
namespace core = ::ps5;
inline constexpr const char* kOwner = "danzig666";
inline constexpr const char* kRepo = "far-ps5-filemanager";
inline constexpr const char* kDllName = "PS5FM.dll";  // release asset, plus "PS5FM.dll.sha256"
inline constexpr const char* kVersion = PS5FM_VERSION_STRING;
inline constexpr const wchar_t* kProduct = L"PS5 File Manager";
}  // namespace updcfg

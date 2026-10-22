#pragma once

#include <windows.h>
#include <winhttp.h>

#include "../core/Http.hpp"

namespace ps5 {

// Synchronous WinHTTP transport. One session; WinHTTP pools connections.
// The console is on the LAN, so no proxy is used.
class WinHttpTransport : public HttpTransport {
public:
  WinHttpTransport();
  ~WinHttpTransport() override;
  WinHttpTransport(const WinHttpTransport&) = delete;
  WinHttpTransport& operator=(const WinHttpTransport&) = delete;

  HttpResponse Send(const std::string& host, uint16_t port, HttpRequest& request) override;

private:
  HINTERNET session_ = nullptr;
};

}  // namespace ps5

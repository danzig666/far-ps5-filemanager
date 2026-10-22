#pragma once

#include "../src/core/Http.hpp"

namespace ps5 {

// Minimal blocking HTTP/1.1 client over POSIX sockets, used by the Linux
// integration tests in place of WinHTTP. One connection per request.
class PosixHttpTransport : public HttpTransport {
public:
  HttpResponse Send(const std::string& host, uint16_t port, HttpRequest& request) override;
};

}  // namespace ps5

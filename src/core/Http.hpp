#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ps5 {

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpResponse {
  int status = 0;
  std::vector<HttpHeader> headers;
  std::string body;  // filled unless a sink consumed a 2xx body

  // Case-insensitive lookup; empty if missing.
  std::string header(std::string_view name) const;
};

struct HttpRequest {
  std::string method = "POST";
  std::string target;  // path + query, e.g. "/api/list?path=%2F"
  std::vector<HttpHeader> headers;

  // Request body: either `body`, or `bodySize` bytes pulled from
  // `bodySource`, which fills the buffer and returns the byte count. The
  // source may throw (for example CancelledError) to abort the request.
  std::string body;
  uint64_t bodySize = 0;
  std::function<size_t(char* buffer, size_t capacity)> bodySource;

  // Called once the status line and headers of a 2xx response are known;
  // returning false aborts with CancelledError.
  std::function<bool(const HttpResponse& head)> onHeaders;

  // Receives the body of a 2xx response in chunks instead of collecting it.
  // Returning false aborts with CancelledError. Error responses are always
  // collected into HttpResponse::body.
  std::function<bool(const char* data, size_t size)> sink;

  int timeoutMs = 30000;
};

// Transport-level failure: DNS, connect, reset, timeout.
class NetError : public std::runtime_error {
public:
  NetError(const std::string& message, unsigned long code = 0)
      : std::runtime_error(message), code_(code) {}
  unsigned long code() const { return code_; }

private:
  unsigned long code_;
};

// The user (or a callback) aborted the operation.
class CancelledError : public std::runtime_error {
public:
  CancelledError() : std::runtime_error("cancelled") {}
};

class HttpTransport {
public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse Send(const std::string& host, uint16_t port, HttpRequest& request) = 0;
};

}  // namespace ps5

#include "PosixHttp.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/core/Text.hpp"

namespace ps5 {

namespace {

struct Socket {
  int fd = -1;
  ~Socket() {
    if (fd >= 0) close(fd);
  }
};

void SendAll(int fd, const char* data, size_t size) {
  while (size) {
    ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
    if (n <= 0) throw NetError("send failed");
    data += n;
    size -= static_cast<size_t>(n);
  }
}

class Reader {
public:
  explicit Reader(int fd) : fd_(fd) {}

  // Returns false at EOF.
  bool Fill() {
    char buf[65536];
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);
    if (n < 0) throw NetError("recv failed");
    if (n == 0) return false;
    data_.append(buf, static_cast<size_t>(n));
    return true;
  }

  std::string ReadLine() {
    for (;;) {
      size_t eol = data_.find("\r\n", pos_);
      if (eol != std::string::npos) {
        std::string line = data_.substr(pos_, eol - pos_);
        pos_ = eol + 2;
        Compact();
        return line;
      }
      if (!Fill()) throw NetError("connection closed in headers");
    }
  }

  // Reads up to `max` bytes; returns 0 at EOF.
  size_t Read(char* out, size_t max) {
    if (pos_ >= data_.size()) {
      data_.clear();
      pos_ = 0;
      if (!Fill()) return 0;
    }
    size_t n = std::min(max, data_.size() - pos_);
    std::memcpy(out, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }

private:
  void Compact() {
    if (pos_ > 65536) {
      data_.erase(0, pos_);
      pos_ = 0;
    }
  }

  int fd_;
  std::string data_;
  size_t pos_ = 0;
};

}  // namespace

HttpResponse PosixHttpTransport::Send(const std::string& host, uint16_t port, HttpRequest& request) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    throw NetError("cannot resolve " + host);
  Socket sock;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    sock.fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock.fd < 0) continue;
    if (connect(sock.fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(sock.fd);
    sock.fd = -1;
  }
  freeaddrinfo(res);
  if (sock.fd < 0) throw NetError("cannot connect to " + host + ":" + std::to_string(port));
  timeval tv{request.timeoutMs / 1000, (request.timeoutMs % 1000) * 1000};
  setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  uint64_t bodySize = request.bodySource ? request.bodySize : request.body.size();
  std::string head = request.method + " " + request.target + " HTTP/1.1\r\n";
  head += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  head += "Connection: close\r\n";
  for (const auto& h : request.headers) head += h.name + ": " + h.value + "\r\n";
  if (request.method != "GET" || bodySize)
    head += "Content-Length: " + std::to_string(bodySize) + "\r\n";
  head += "\r\n";
  SendAll(sock.fd, head.data(), head.size());
  if (request.bodySource) {
    std::string buf(1 << 20, '\0');
    uint64_t left = bodySize;
    while (left) {
      size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
      size_t got = request.bodySource(buf.data(), want);
      if (got == 0) throw NetError("request body source ended early");
      SendAll(sock.fd, buf.data(), got);
      left -= got;
    }
  } else if (!request.body.empty()) {
    SendAll(sock.fd, request.body.data(), request.body.size());
  }

  Reader reader(sock.fd);
  HttpResponse response;
  std::string status = reader.ReadLine();
  size_t sp = status.find(' ');
  if (sp == std::string::npos) throw NetError("bad status line");
  response.status = std::atoi(status.c_str() + sp + 1);
  for (;;) {
    std::string line = reader.ReadLine();
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    response.headers.push_back({line.substr(0, colon), value});
  }
  bool ok = response.status >= 200 && response.status < 300;
  if (ok && request.onHeaders && !request.onHeaders(response)) throw CancelledError();
  bool streaming = ok && request.sink;
  auto deliver = [&](const char* data, size_t size) {
    if (streaming) {
      if (!request.sink(data, size)) throw CancelledError();
    } else {
      response.body.append(data, size);
    }
  };

  char buf[65536];
  if (request.method == "HEAD" || response.status == 204 || response.status == 304) return response;
  if (ToLowerAscii(response.header("Transfer-Encoding")).find("chunked") != std::string::npos) {
    for (;;) {
      std::string sizeLine = reader.ReadLine();
      uint64_t chunk = std::strtoull(sizeLine.c_str(), nullptr, 16);
      if (chunk == 0) {
        while (!reader.ReadLine().empty()) {
        }
        break;
      }
      while (chunk) {
        size_t n = reader.Read(buf, static_cast<size_t>(std::min<uint64_t>(chunk, sizeof(buf))));
        if (!n) throw NetError("connection closed in chunk");
        deliver(buf, n);
        chunk -= n;
      }
      reader.ReadLine();
    }
  } else if (!response.header("Content-Length").empty()) {
    uint64_t left = std::strtoull(response.header("Content-Length").c_str(), nullptr, 10);
    while (left) {
      size_t n = reader.Read(buf, static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf))));
      if (!n) throw NetError("connection closed before end of body");
      deliver(buf, n);
      left -= n;
    }
  } else {
    for (;;) {
      size_t n = reader.Read(buf, sizeof(buf));
      if (!n) break;
      deliver(buf, n);
    }
  }
  return response;
}

}  // namespace ps5

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Http.hpp"
#include "Json.hpp"

namespace ps5 {

// Error reported by the payload: {"ok":false,"error":...,"error_code":...}.
class ApiError : public std::runtime_error {
public:
  ApiError(int status, std::string code, std::string message, std::string arg)
      : std::runtime_error(message.empty() ? code : message),
        status_(status),
        code_(std::move(code)),
        message_(std::move(message)),
        arg_(std::move(arg)) {}

  int status() const { return status_; }
  const std::string& code() const { return code_; }
  const std::string& message() const { return message_; }
  const std::string& arg() const { return arg_; }

private:
  int status_;
  std::string code_;
  std::string message_;
  std::string arg_;
};

// All path and name fields below are raw bytes as stored on the console
// (already converted from the payload's token form).
struct ListEntry {
  std::string name;
  std::string path;
  char type = '-';  // d l - c b p s
  uint32_t mode = 0;
  uint64_t size = 0;
  int64_t mtime = 0;

  bool isDir() const { return type == 'd'; }
};

struct Listing {
  std::string path;
  std::string parent;
  std::vector<ListEntry> entries;
};

struct SpaceInfo {
  std::string labelKey;  // storageInternal, storageUsb, ... (empty for custom)
  std::string label;     // custom entries only
  std::string path;
  std::string user;  // SMB entries
  uint64_t free = 0;
  uint64_t total = 0;
  bool custom = false;
  bool available = true;
  bool current = false;

  bool isSmb() const { return path.rfind("smb://", 0) == 0; }
};

struct TaskInfo {
  uint64_t id = 0;
  std::string op;     // copy move delete chmod download upload extract pkg_install
  std::string state;  // queued running done failed canceled
  std::string src, dst, current;  // raw
  std::string error, errorCode, errorArg;
  uint64_t srcCount = 0;
  uint64_t completedCount = 0;
  uint64_t total = 0;
  uint64_t done = 0;
  uint64_t speed = 0;
  uint64_t eta = 0;
  uint64_t elapsed = 0;  // seconds since the task was created
  bool cancelRequested = false;

  bool active() const { return state == "queued" || state == "running"; }
};

struct UploadItem {
  std::string rel;  // UTF-8 relative path with '/' separators
  uint64_t size = 0;
};

struct ReadResult {
  int status = 0;            // 200 or 206
  uint64_t fileSize = 0;     // full size of the remote file
  uint64_t received = 0;     // bytes passed to the sink
};

class Ps5Client {
public:
  Ps5Client(std::shared_ptr<HttpTransport> transport, std::string host, uint16_t port);

  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }

  std::vector<SpaceInfo> Space(const std::string& rawPath = {});
  Listing List(const std::string& rawPath);

  // GET /fs. With offset > 0 sends a Range header and requires a 206.
  ReadResult ReadFile(const std::string& rawPath, uint64_t offset,
                      const std::function<bool(const char*, size_t)>& sink);

  uint64_t UploadPrepare(const std::string& rawDir, const std::vector<UploadItem>& items,
                         bool overwrite);
  void UploadFile(uint64_t taskId, const std::string& rawDir, const UploadItem& item,
                  bool overwrite,
                  const std::function<size_t(char*, size_t)>& source);
  void UploadFinish(uint64_t taskId);

  uint64_t Copy(const std::vector<std::string>& rawPaths, const std::string& rawDst, bool overwrite);
  uint64_t Move(const std::vector<std::string>& rawPaths, const std::string& rawDst, bool overwrite);
  uint64_t Delete(const std::vector<std::string>& rawPaths);
  uint64_t Chmod(const std::vector<std::string>& rawPaths, uint32_t mode, bool recursive);
  uint64_t Extract(const std::vector<std::string>& rawPaths, const std::string& rawDest,
                   bool separate, bool overwrite, const std::string& password);

  std::vector<TaskInfo> Tasks();
  void Cancel(uint64_t taskId);

  void Mkdir(const std::string& rawParent, const std::string& rawName);
  void Rename(const std::string& rawPath, const std::string& rawNewName);

  JsonValue PkgInfo(const std::string& rawPath);
  std::vector<uint64_t> InstallPkg(const std::vector<std::string>& rawPaths);
  void LaunchElf(const std::string& rawPath);

  // Unlocks a saved SMB connection for this payload session.
  void SmbConnect(const std::string& rawAddress, const std::string& password);

  void Exit();

  // Encodes a raw path for a query/form parameter.
  static std::string Param(const std::string& raw);

private:
  JsonValue Call(HttpRequest& request);
  JsonValue PostQuery(const std::string& endpoint,
                      const std::vector<std::pair<std::string, std::string>>& query);
  JsonValue PostForm(const std::string& endpoint,
                     const std::vector<std::pair<std::string, std::string>>& form);
  uint64_t StartTask(const std::string& endpoint,
                     const std::vector<std::pair<std::string, std::string>>& form);
  [[noreturn]] void ThrowApiError(const HttpResponse& response);

  std::shared_ptr<HttpTransport> transport_;
  std::string host_;
  uint16_t port_;
};

std::string JoinPathParams(const std::vector<std::string>& rawPaths);

}  // namespace ps5

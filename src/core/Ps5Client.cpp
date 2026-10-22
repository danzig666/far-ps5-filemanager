#include "Ps5Client.hpp"

#include <cstdlib>

#include "PathCodec.hpp"
#include "Text.hpp"

namespace ps5 {

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string HttpResponse::header(std::string_view name) const {
  std::string wanted = ToLowerAscii(name);
  for (const auto& h : headers) {
    if (ToLowerAscii(h.name) == wanted) return h.value;
  }
  return {};
}

std::string JoinPathParams(const std::vector<std::string>& rawPaths) {
  std::string out;
  for (const auto& raw : rawPaths) {
    if (!out.empty()) out.push_back('\n');
    out += path_codec::RawToToken(raw);
  }
  return out;
}

Ps5Client::Ps5Client(std::shared_ptr<HttpTransport> transport, std::string host, uint16_t port)
    : transport_(std::move(transport)), host_(std::move(host)), port_(port) {}

std::string Ps5Client::Param(const std::string& raw) {
  return path_codec::RawToToken(raw);
}

void Ps5Client::ThrowApiError(const HttpResponse& response) {
  std::string code = "http_" + std::to_string(response.status);
  std::string message = "HTTP " + std::to_string(response.status);
  std::string arg;
  try {
    JsonValue json = ParseJson(response.body);
    if (json.isObject()) {
      if (json["error_code"].isString() && !json["error_code"].asString().empty())
        code = json["error_code"].asString();
      if (json["error"].isString()) message = json["error"].asString();
      if (json["error_arg"].isString()) arg = path_codec::TokenToRaw(json["error_arg"].asString());
    }
  } catch (const JsonError&) {
    // Non-JSON error body (for example from libmicrohttpd itself).
  }
  throw ApiError(response.status, code, path_codec::TokenToRaw(message), arg);
}

JsonValue Ps5Client::Call(HttpRequest& request) {
  HttpResponse response = transport_->Send(host_, port_, request);
  if (response.status < 200 || response.status >= 300) ThrowApiError(response);
  JsonValue json;
  try {
    json = ParseJson(response.body);
  } catch (const JsonError& e) {
    throw ApiError(response.status, "bad_response", e.what(), {});
  }
  if (json.isObject() && json.has("ok") && !json["ok"].asBool()) ThrowApiError(response);
  return json;
}

JsonValue Ps5Client::PostQuery(const std::string& endpoint, const Fields& query) {
  HttpRequest request;
  request.method = "POST";
  request.target = endpoint;
  if (!query.empty()) request.target += "?" + FormEncode(query);
  return Call(request);
}

JsonValue Ps5Client::PostForm(const std::string& endpoint, const Fields& form) {
  HttpRequest request;
  request.method = "POST";
  request.target = endpoint;
  request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
  request.body = FormEncode(form);
  return Call(request);
}

uint64_t Ps5Client::StartTask(const std::string& endpoint, const Fields& form) {
  JsonValue json = PostForm(endpoint, form);
  uint64_t id = json["task_id"].asUInt();
  if (!id) throw ApiError(200, "bad_response", "server did not return a task id", {});
  return id;
}

std::vector<SpaceInfo> Ps5Client::Space(const std::string& rawPath) {
  Fields query;
  if (!rawPath.empty()) query.emplace_back("path", Param(rawPath));
  HttpRequest request;
  request.method = "GET";
  request.target = "/api/space";
  if (!query.empty()) request.target += "?" + FormEncode(query);
  JsonValue json = Call(request);
  std::vector<SpaceInfo> spaces;
  for (const auto& item : json["spaces"].items()) {
    SpaceInfo s;
    s.labelKey = item["label_key"].asString();
    s.label = path_codec::TokenToRaw(item["label"].asString());
    s.path = path_codec::TokenToRaw(item["path"].asString());
    s.user = path_codec::TokenToRaw(item["user"].asString());
    s.free = item["free"].asUInt();
    s.total = item["total"].asUInt();
    s.custom = item["custom"].asBool();
    s.available = item["available"].asBool(true);
    s.current = item["current"].asBool();
    if (!s.path.empty()) spaces.push_back(std::move(s));
  }
  return spaces;
}

Listing Ps5Client::List(const std::string& rawPath) {
  JsonValue json = PostQuery("/api/list", {{"path", Param(rawPath)}});
  Listing listing;
  listing.path = path_codec::TokenToRaw(json["path"].asString());
  listing.parent = path_codec::TokenToRaw(json["parent"].asString());
  const JsonValue& entries = json["entries"];
  listing.entries.reserve(entries.size());
  for (const auto& item : entries.items()) {
    ListEntry e;
    e.name = path_codec::TokenToRaw(item["name"].asString());
    e.path = path_codec::TokenToRaw(item["path"].asString());
    std::string type = item["type"].asString("-");
    e.type = type.empty() ? '-' : type[0];
    e.mode = static_cast<uint32_t>(item["mode"].asUInt());
    e.size = item["size"].asUInt();
    e.mtime = item["mtime"].asInt();
    if (e.path.empty()) e.path = path_codec::Join(listing.path, e.name);
    listing.entries.push_back(std::move(e));
  }
  return listing;
}

ReadResult Ps5Client::ReadFile(const std::string& rawPath, uint64_t offset,
                               const std::function<bool(const char*, size_t)>& sink) {
  ReadResult result;
  HttpRequest request;
  request.method = "GET";
  request.target = "/fs?" + FormEncode({{"path", Param(rawPath)}});
  request.timeoutMs = 60000;
  if (offset > 0) request.headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});
  bool rangeIgnored = false;
  request.onHeaders = [&](const HttpResponse& head) {
    result.status = head.status;
    std::string range = head.header("Content-Range");
    size_t slash = range.rfind('/');
    if (slash != std::string::npos) {
      result.fileSize = std::strtoull(range.c_str() + slash + 1, nullptr, 10);
    } else {
      result.fileSize = std::strtoull(head.header("Content-Length").c_str(), nullptr, 10);
    }
    if (offset > 0 && head.status != 206) {
      rangeIgnored = true;
      return false;
    }
    return true;
  };
  request.sink = [&](const char* data, size_t size) {
    result.received += size;
    return sink(data, size);
  };
  HttpResponse response;
  try {
    response = transport_->Send(host_, port_, request);
  } catch (const CancelledError&) {
    if (rangeIgnored) throw ApiError(result.status, "range_ignored", "server ignored the Range request", {});
    throw;
  }
  if (response.status == 416) {
    // Requested offset is at or past the end: nothing more to read.
    std::string range = response.header("Content-Range");
    size_t slash = range.rfind('/');
    if (slash != std::string::npos) {
      result.fileSize = std::strtoull(range.c_str() + slash + 1, nullptr, 10);
      if (result.fileSize == offset) {
        result.status = 206;
        return result;
      }
    }
  }
  if (response.status < 200 || response.status >= 300) ThrowApiError(response);
  result.status = response.status;
  return result;
}

uint64_t Ps5Client::UploadPrepare(const std::string& rawDir, const std::vector<UploadItem>& items,
                                  bool overwrite) {
  uint64_t total = 0;
  std::string rels, sizes;
  for (const auto& item : items) {
    total += item.size;
    if (!rels.empty()) {
      rels.push_back('\n');
      sizes.push_back('\n');
    }
    rels += item.rel;
    sizes += std::to_string(item.size);
  }
  return StartTask("/api/upload/prepare",
                   {{"path", Param(rawDir)},
                    {"src", items.empty() ? std::string() : items[0].rel},
                    {"total", std::to_string(total)},
                    {"count", std::to_string(items.size())},
                    {"rels", rels},
                    {"sizes", sizes},
                    {"overwrite", overwrite ? "1" : "0"}});
}

void Ps5Client::UploadFile(uint64_t taskId, const std::string& rawDir, const UploadItem& item,
                           bool overwrite, const std::function<size_t(char*, size_t)>& source) {
  HttpRequest request;
  request.method = "POST";
  request.target = "/api/upload-file";
  request.timeoutMs = 120000;
  request.headers.push_back({"Content-Type", "application/octet-stream"});
  request.headers.push_back({"X-WFM-Task-ID", std::to_string(taskId)});
  // X-WFM-Path goes through fs_path_value() on the server, X-WFM-Rel does not.
  request.headers.push_back({"X-WFM-Path", PercentEncode(Param(rawDir))});
  request.headers.push_back({"X-WFM-Rel", PercentEncode(item.rel)});
  request.headers.push_back({"X-WFM-Size", std::to_string(item.size)});
  request.headers.push_back({"X-WFM-Overwrite", overwrite ? "1" : "0"});
  request.bodySize = item.size;
  request.bodySource = source;
  Call(request);
}

void Ps5Client::UploadFinish(uint64_t taskId) {
  PostQuery("/api/upload/finish", {{"task_id", std::to_string(taskId)}});
}

uint64_t Ps5Client::Copy(const std::vector<std::string>& rawPaths, const std::string& rawDst,
                         bool overwrite) {
  return StartTask("/api/copy", {{"paths", JoinPathParams(rawPaths)},
                                 {"dst", Param(rawDst)},
                                 {"overwrite", overwrite ? "1" : "0"}});
}

uint64_t Ps5Client::Move(const std::vector<std::string>& rawPaths, const std::string& rawDst,
                         bool overwrite) {
  return StartTask("/api/move", {{"paths", JoinPathParams(rawPaths)},
                                 {"dst", Param(rawDst)},
                                 {"overwrite", overwrite ? "1" : "0"}});
}

uint64_t Ps5Client::Delete(const std::vector<std::string>& rawPaths) {
  return StartTask("/api/delete", {{"paths", JoinPathParams(rawPaths)}});
}

uint64_t Ps5Client::Chmod(const std::vector<std::string>& rawPaths, uint32_t mode, bool recursive) {
  return StartTask("/api/chmod", {{"paths", JoinPathParams(rawPaths)},
                                  {"mode", FormatOctalMode(mode)},
                                  {"recursive", recursive ? "1" : "0"}});
}

uint64_t Ps5Client::Extract(const std::vector<std::string>& rawPaths, const std::string& rawDest,
                            bool separate, bool overwrite, const std::string& password) {
  Fields form{{"paths", JoinPathParams(rawPaths)},
              {"destination", Param(rawDest)},
              {"separate", separate ? "1" : "0"},
              {"overwrite", overwrite ? "1" : "0"}};
  if (!password.empty()) form.emplace_back("password", password);
  return StartTask("/api/extract", form);
}

std::vector<TaskInfo> Ps5Client::Tasks() {
  JsonValue json = PostQuery("/api/tasks", {});
  std::vector<TaskInfo> tasks;
  for (const auto& item : json["tasks"].items()) {
    TaskInfo t;
    t.id = item["id"].asUInt();
    t.op = item["op"].asString();
    t.state = item["state"].asString();
    t.src = path_codec::TokenToRaw(item["src"].asString());
    t.dst = path_codec::TokenToRaw(item["dst"].asString());
    t.current = path_codec::TokenToRaw(item["current"].asString());
    t.error = path_codec::TokenToRaw(item["error"].asString());
    t.errorCode = item["error_code"].asString();
    t.errorArg = path_codec::TokenToRaw(item["error_arg"].asString());
    t.srcCount = item["src_count"].asUInt();
    t.completedCount = item["completed_count"].asUInt();
    t.total = item["total"].asUInt();
    t.done = item["done"].asUInt();
    t.speed = item["speed"].asUInt();
    t.eta = item["eta"].asUInt();
    t.cancelRequested = item["cancel_requested"].asBool();
    tasks.push_back(std::move(t));
  }
  return tasks;
}

void Ps5Client::Cancel(uint64_t taskId) {
  PostQuery("/api/cancel", {{"id", std::to_string(taskId)}});
}

void Ps5Client::Mkdir(const std::string& rawParent, const std::string& rawName) {
  PostQuery("/api/mkdir", {{"path", Param(rawParent)}, {"name", Param(rawName)}});
}

void Ps5Client::Rename(const std::string& rawPath, const std::string& rawNewName) {
  PostQuery("/api/rename", {{"path", Param(rawPath)}, {"name", Param(rawNewName)}});
}

JsonValue Ps5Client::PkgInfo(const std::string& rawPath) {
  HttpRequest request;
  request.method = "GET";
  request.target = "/api/pkg-info?" + FormEncode({{"path", Param(rawPath)}});
  return Call(request);
}

std::vector<uint64_t> Ps5Client::InstallPkg(const std::vector<std::string>& rawPaths) {
  JsonValue json = PostForm("/api/install-pkg", {{"paths", JoinPathParams(rawPaths)}});
  std::vector<uint64_t> ids;
  for (const auto& id : json["task_ids"].items()) ids.push_back(id.asUInt());
  return ids;
}

void Ps5Client::LaunchElf(const std::string& rawPath) {
  PostForm("/api/launch-elf", {{"path", Param(rawPath)}});
}

void Ps5Client::SmbConnect(const std::string& rawAddress, const std::string& password) {
  PostForm("/api/smb/connect", {{"address", Param(rawAddress)}, {"password", password}});
}

void Ps5Client::Exit() {
  PostQuery("/api/exit", {});
}

}  // namespace ps5

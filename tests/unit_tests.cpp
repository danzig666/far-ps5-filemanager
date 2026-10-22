// Unit tests for the platform-neutral core. No network, no Far.

#include <algorithm>
#include <memory>

#include "../src/core/Json.hpp"
#include "../src/core/Operations.hpp"
#include "../src/core/PathCodec.hpp"
#include "../src/core/Ps5Client.hpp"
#include "../src/core/Text.hpp"
#include "TestMain.hpp"

using namespace ps5;

// ---------------------------------------------------------------------------
// JSON

TEST("json: scalars and nesting") {
  JsonValue v = ParseJson(R"({"ok":true,"n":-5,"big":18446744073709551615,"s":"a\"b","a":[1,{"x":null}]})");
  CHECK(v["ok"].asBool());
  CHECK_EQ(v["n"].asInt(), -5);
  CHECK_EQ(v["big"].asUInt(), 18446744073709551615ULL);
  CHECK_EQ(v["s"].asString(), std::string("a\"b"));
  CHECK_EQ(v["a"].size(), 2u);
  CHECK(v["a"][1]["x"].isNull());
  CHECK(v["missing"]["deeper"].isNull());
}

TEST("json: unicode escapes become UTF-8") {
  JsonValue v = ParseJson(R"("é日😀")");
  CHECK_EQ(v.asString(), std::string("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"));
}

TEST("json: errors") {
  CHECK_THROWS(ParseJson("{"), JsonError);
  CHECK_THROWS(ParseJson("[1,]"), JsonError);
  CHECK_THROWS(ParseJson("{} x"), JsonError);
  CHECK_THROWS(ParseJson("\"abc"), JsonError);
}

// ---------------------------------------------------------------------------
// Text helpers

TEST("utf8: wide round trip including astral plane") {
  std::string utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
  std::wstring wide = Utf8ToWide(utf8);
  CHECK_EQ(WideToUtf8(wide), utf8);
}

TEST("utf8: validation") {
  CHECK(IsValidUtf8("plain"));
  CHECK(IsValidUtf8("\xE6\x97\xA5"));
  CHECK(!IsValidUtf8("\xE9"));          // Latin-1 byte
  CHECK(!IsValidUtf8("\xC0\xAF"));      // overlong
  CHECK(!IsValidUtf8("\xED\xA0\x80"));  // surrogate
  CHECK(!IsValidUtf8("\xC4\xE3"));      // GBK pair
}

TEST("percent encoding") {
  CHECK_EQ(PercentEncode("a b+c/d~e"), std::string("a%20b%2Bc%2Fd~e"));
  CHECK_EQ(PercentEncode("\xC3\xA9"), std::string("%C3%A9"));
  CHECK_EQ(FormEncode({{"path", "/a b"}, {"x", "1&2"}}), std::string("path=%2Fa%20b&x=1%262"));
}

TEST("formatting") {
  CHECK_EQ(FormatSize(512), std::string("512 B"));
  CHECK_EQ(FormatSize(1536), std::string("1.50 KB"));
  CHECK_EQ(FormatDuration(61), std::string("01:01"));
  CHECK_EQ(FormatDuration(3723), std::string("1:02:03"));
  CHECK_EQ(FormatMode(0755), std::string("rwxr-xr-x"));
  CHECK_EQ(FormatOctalMode(0644), std::string("0644"));
  uint32_t mode = 0;
  CHECK(ParseOctalMode("0777", mode));
  CHECK_EQ(mode, 0777u);
  CHECK(ParseOctalMode("640", mode));
  CHECK_EQ(mode, 0640u);
  CHECK(!ParseOctalMode("0778", mode));
  CHECK(!ParseOctalMode("12", mode));
}

// ---------------------------------------------------------------------------
// Path codec: the payload's byte-token scheme

TEST("codec: token -> raw collapses U+0080..U+00FF to bytes") {
  // "cafÃ©" as the payload sends UTF-8 "café".
  std::string token = "caf\xC3\x83\xC2\xA9";
  CHECK_EQ(path_codec::TokenToRaw(token), std::string("caf\xC3\xA9"));
}

TEST("codec: raw -> token for Latin-1 range name") {
  // Sending plain UTF-8 "café" would be collapsed by the server to "caf\xE9".
  std::string raw = "caf\xC3\xA9";
  std::string token = path_codec::RawToToken(raw);
  CHECK_EQ(token, std::string("caf\xC3\x83\xC2\xA9"));
  CHECK_EQ(path_codec::TokenToRaw(token), raw);
}

TEST("codec: round trips CJK, invalid bytes and ASCII") {
  const std::string samples[] = {
      "/data/\xE6\x97\xA5\xE6\x9C\xAC",  // UTF-8 CJK
      "/mnt/usb0/\xC4\xE3\xBA\xC3",      // GBK bytes, not UTF-8
      "/data/\xFF\xFE\x80",              // arbitrary bytes
      "/data/plain name.txt",
  };
  for (const auto& raw : samples) {
    CHECK_EQ(path_codec::TokenToRaw(path_codec::RawToToken(raw)), raw);
  }
  CHECK_EQ(path_codec::RawToToken("/plain"), std::string("/plain"));
}

TEST("codec: display decoding") {
  bool lossy = true;
  CHECK(path_codec::RawToDisplay("\xE6\x97\xA5", &lossy) == std::wstring(1, wchar_t(0x65E5)));
  CHECK(!lossy);
  std::wstring fallback = path_codec::RawToDisplay("\xE9", &lossy);
  CHECK(lossy);
  CHECK(fallback == std::wstring(1, wchar_t(0xE9)));
  path_codec::SetFallbackDecoder([](std::string_view) { return std::wstring(L"X"); });
  CHECK(path_codec::RawToDisplay("\xE9") == std::wstring(L"X"));
  path_codec::SetFallbackDecoder(nullptr);
}

TEST("codec: path helpers") {
  CHECK_EQ(path_codec::Join("/data", "a"), std::string("/data/a"));
  CHECK_EQ(path_codec::Join("/data/", "/a"), std::string("/data/a"));
  CHECK_EQ(path_codec::Join("/", "a"), std::string("/a"));
  CHECK_EQ(path_codec::Parent("/data/a/b"), std::string("/data/a"));
  CHECK_EQ(path_codec::Parent("/data"), std::string("/"));
  CHECK_EQ(path_codec::Parent("/"), std::string("/"));
  CHECK_EQ(path_codec::Leaf("/data/a/"), std::string("a"));
}

// ---------------------------------------------------------------------------
// Client request formation, against a scripted transport

namespace {

struct FakeTransport : HttpTransport {
  std::vector<HttpRequest> requests;
  std::vector<std::string> sentBodies;
  std::vector<HttpResponse> replies;

  HttpResponse Send(const std::string&, uint16_t, HttpRequest& request) override {
    requests.push_back(request);
    std::string body = request.body;
    if (request.bodySource) {
      body.clear();
      char buf[7];
      uint64_t left = request.bodySize;
      while (left) {
        size_t n = request.bodySource(buf, static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf))));
        body.append(buf, n);
        left -= n;
      }
    }
    sentBodies.push_back(body);
    HttpResponse reply = replies.empty() ? HttpResponse{200, {}, R"({"ok":true})"} : replies.front();
    if (!replies.empty()) replies.erase(replies.begin());
    if (reply.status >= 200 && reply.status < 300) {
      if (request.onHeaders && !request.onHeaders(reply)) throw CancelledError();
      if (request.sink) {
        if (!request.sink(reply.body.data(), reply.body.size())) throw CancelledError();
        reply.body.clear();
      }
    }
    return reply;
  }

  std::string header(size_t i, const std::string& name) const {
    for (const auto& h : requests[i].headers)
      if (h.name == name) return h.value;
    return {};
  }
};

}  // namespace

TEST("client: list decodes entries and sends token path") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({200, {},
      R"({"ok":true,"path":"/data/cafÃ©","parent":"/data","entries":[)"
      R"({"name":"æ\u0097¥","path":"/data/cafÃ©/æ\u0097¥","type":"d","mode":511,"size":4096,"mtime":1700000000},)"
      R"({"name":"x.bin","path":"/data/cafÃ©/x.bin","type":"-","mode":420,"size":5000000000,"mtime":1})"
      R"(]})"});
  Ps5Client client(t, "h", 8888);
  Listing l = client.List("/data/caf\xC3\xA9");
  CHECK_EQ(t->requests[0].method, std::string("POST"));
  // "café" raw bytes -> tokens U+00C3 U+00A9 -> UTF-8 C3 83 C2 A9 -> percent-encoded.
  CHECK_EQ(t->requests[0].target, std::string("/api/list?path=%2Fdata%2Fcaf%C3%83%C2%A9"));
  CHECK_EQ(l.path, std::string("/data/caf\xC3\xA9"));
  CHECK_EQ(l.entries.size(), 2u);
  CHECK_EQ(l.entries[0].name, std::string("\xE6\x97\xA5"));
  CHECK(l.entries[0].isDir());
  CHECK_EQ(l.entries[0].mode, 0777u);
  CHECK_EQ(l.entries[1].size, 5000000000ULL);
}

TEST("client: API errors carry the payload's code") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({409, {}, R"({"ok":false,"error":"another task is running","error_code":"active_task","error_arg":"another task is running"})"});
  Ps5Client client(t, "h", 8888);
  try {
    client.List("/data");
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.status(), 409);
    CHECK_EQ(e.code(), std::string("active_task"));
    CHECK_EQ(DescribeApiError(e), std::string("Another task is already running on the console"));
  }
  t->replies.push_back({404, {}, "not json"});
  try {
    client.List("/data");
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.code(), std::string("http_404"));
  }
}

TEST("client: copy sends form with newline-joined token paths") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({200, {}, R"({"ok":true,"task_id":42})"});
  Ps5Client client(t, "h", 8888);
  uint64_t id = client.Copy({"/data/a b", "/data/\xE9"}, "/mnt/usb0", true);
  CHECK_EQ(id, 42u);
  CHECK_EQ(t->requests[0].target, std::string("/api/copy"));
  CHECK_EQ(t->requests[0].body,
           std::string("paths=%2Fdata%2Fa%20b%0A%2Fdata%2F%C3%A9&dst=%2Fmnt%2Fusb0&overwrite=1"));
}

TEST("client: upload headers and streamed body") {
  auto t = std::make_shared<FakeTransport>();
  Ps5Client client(t, "h", 8888);
  std::string data = "hello world, this is the body";
  size_t offset = 0;
  client.UploadFile(7, "/data/caf\xC3\xA9", {"dir/caf\xC3\xA9.txt", data.size()}, false,
                    [&](char* buf, size_t cap) {
                      size_t n = std::min(cap, data.size() - offset);
                      std::copy(data.begin() + offset, data.begin() + offset + n, buf);
                      offset += n;
                      return n;
                    });
  CHECK_EQ(t->sentBodies[0], data);
  CHECK_EQ(t->header(0, "X-WFM-Task-ID"), std::string("7"));
  // Path is token form; Rel is plain UTF-8.
  CHECK_EQ(t->header(0, "X-WFM-Path"), std::string("%2Fdata%2Fcaf%C3%83%C2%A9"));
  CHECK_EQ(t->header(0, "X-WFM-Rel"), std::string("dir%2Fcaf%C3%A9.txt"));
  CHECK_EQ(t->header(0, "X-WFM-Size"), std::to_string(data.size()));
  CHECK_EQ(t->header(0, "X-WFM-Overwrite"), std::string("0"));
}

TEST("client: upload prepare manifest") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({200, {}, R"({"ok":true,"task_id":3})"});
  Ps5Client client(t, "h", 8888);
  CHECK_EQ(client.UploadPrepare("/data", {{"a.txt", 1}, {"d/b.txt", 22}}, true), 3u);
  CHECK_EQ(t->requests[0].body,
           std::string("path=%2Fdata&src=a.txt&total=23&count=2&rels=a.txt%0Ad%2Fb.txt&sizes=1%0A22&overwrite=1"));
}

TEST("client: read file with range") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({206, {{"Content-Range", "bytes 10-14/15"}, {"Content-Length", "5"}}, "abcde"});
  Ps5Client client(t, "h", 8888);
  std::string got;
  ReadResult r = client.ReadFile("/data/f", 10, [&](const char* d, size_t n) {
    got.append(d, n);
    return true;
  });
  CHECK_EQ(t->requests[0].method, std::string("GET"));
  CHECK_EQ(t->header(0, "Range"), std::string("bytes=10-"));
  CHECK_EQ(r.fileSize, 15u);
  CHECK_EQ(got, std::string("abcde"));

  // A 200 to a range request must not be appended to a partial file.
  t->replies.push_back({200, {{"Content-Length", "15"}}, "0123456789abcde"});
  CHECK_THROWS(client.ReadFile("/data/f", 10, [](const char*, size_t) { return true; }), ApiError);
}

TEST("client: mkdir and rename encode names as tokens") {
  auto t = std::make_shared<FakeTransport>();
  Ps5Client client(t, "h", 8888);
  client.Mkdir("/data", "caf\xC3\xA9");
  CHECK_EQ(t->requests[0].target, std::string("/api/mkdir?path=%2Fdata&name=caf%C3%83%C2%A9"));
  client.Rename("/data/a", "b c");
  CHECK_EQ(t->requests[1].target, std::string("/api/rename?path=%2Fdata%2Fa&name=b%20c"));
}

TEST("tasks: wait follows a task to completion and cancels once") {
  auto t = std::make_shared<FakeTransport>();
  const char* running = R"({"ok":true,"tasks":[{"id":5,"op":"copy","state":"running","src":"/a","dst":"/b","current":"/a/x","error":"","error_code":"","error_arg":"","src_count":1,"completed_count":0,"total":100,"done":40,"speed":10,"eta":6,"cancel_requested":false}],"now":0,"completion":null})";
  const char* canceled = R"({"ok":true,"tasks":[{"id":5,"op":"copy","state":"canceled","src":"/a","dst":"/b","current":"","error":"canceled","error_code":"","error_arg":"","src_count":1,"completed_count":0,"total":100,"done":50,"speed":0,"eta":0,"cancel_requested":true}],"now":0,"completion":null})";
  t->replies = {{200, {}, running}, {200, {}, R"({"ok":true})"}, {200, {}, running}, {200, {}, canceled}};
  Ps5Client client(t, "h", 8888);
  int polls = 0;
  TaskResult r = WaitForTask(client, 5, [&](const TaskInfo* task) {
    ++polls;
    return !(task && task->done >= 40);  // ask to cancel on first poll
  }, [](int) {});
  CHECK(r.outcome == TaskOutcome::Canceled);
  CHECK_EQ(r.last.done, 50u);
  int cancels = 0;
  for (const auto& req : t->requests)
    if (req.target.rfind("/api/cancel", 0) == 0) ++cancels;
  CHECK_EQ(cancels, 1);
}

TEST("tasks: vanished task after being seen") {
  auto t = std::make_shared<FakeTransport>();
  const char* running = R"({"ok":true,"tasks":[{"id":9,"op":"delete","state":"running"}]})";
  t->replies = {{200, {}, running}, {200, {}, R"({"ok":true,"tasks":[]})"}};
  Ps5Client client(t, "h", 8888);
  TaskResult r = WaitForTask(client, 9, [](const TaskInfo*) { return true; }, [](int) {});
  CHECK(r.outcome == TaskOutcome::Vanished);
}

TEST("walk: directories before contents, special files skipped") {
  auto t = std::make_shared<FakeTransport>();
  t->replies.push_back({200, {}, R"({"ok":true,"path":"/d","parent":"/","entries":[)"
      R"({"name":"f","path":"/d/f","type":"-","mode":420,"size":3,"mtime":0},)"
      R"({"name":"sub","path":"/d/sub","type":"d","mode":493,"size":0,"mtime":0},)"
      R"({"name":"fifo","path":"/d/fifo","type":"p","mode":420,"size":0,"mtime":0}]})"});
  t->replies.push_back({200, {}, R"({"ok":true,"path":"/d/sub","parent":"/d","entries":[]})"});
  Ps5Client client(t, "h", 8888);
  ListEntry root;
  root.name = "d";
  root.path = "/d";
  root.type = 'd';
  auto nodes = WalkRemote(client, {root}, nullptr);
  CHECK_EQ(nodes.size(), 3u);
  CHECK_EQ(nodes[0].rel, std::string("d"));
  CHECK_EQ(nodes[1].rel, std::string("d/f"));
  CHECK_EQ(nodes[2].rel, std::string("d/sub"));
}

int main() {
  return test::RunAll();
}

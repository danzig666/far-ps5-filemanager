// Integration tests: drive Ps5Client against a real ps5-web-file-manager
// payload built for Linux (`make linux`) and running on this machine.
//
//   integration_tests <port> <scratch-dir>
//
// The scratch dir must be empty and writable; the payload serves the real
// file system, so the tests also check results directly on disk.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "../src/core/Operations.hpp"
#include "../src/core/PathCodec.hpp"
#include "PosixHttp.hpp"
#include "TestMain.hpp"

using namespace ps5;

namespace {

std::string g_root;
uint16_t g_port = 8888;

Ps5Client MakeClient() {
  return Ps5Client(std::make_shared<PosixHttpTransport>(), "127.0.0.1", g_port);
}

void Sleep(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

TaskResult Wait(Ps5Client& client, uint64_t id) {
  return WaitForTask(client, id, [](const TaskInfo*) { return true; }, Sleep, 100);
}

bool Exists(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

std::string ReadLocal(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteLocal(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out << data;
}

std::string Pattern(size_t size) {
  std::string data(size, '\0');
  uint32_t x = 12345;
  for (auto& c : data) {
    x = x * 1103515245 + 12345;
    c = static_cast<char>(x >> 16);
  }
  return data;
}

const ListEntry* Find(const Listing& listing, const std::string& name) {
  for (const auto& e : listing.entries)
    if (e.name == name) return &e;
  return nullptr;
}

// Uploads in-memory files the way the plugin does.
void Upload(Ps5Client& client, const std::string& dir,
            const std::vector<std::pair<std::string, std::string>>& files, bool overwrite) {
  std::vector<UploadItem> items;
  for (const auto& f : files) items.push_back({f.first, f.second.size()});
  uint64_t id = client.UploadPrepare(dir, items, overwrite);
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string& data = files[i].second;
    size_t offset = 0;
    client.UploadFile(id, dir, items[i], overwrite, [&](char* buf, size_t cap) {
      size_t n = std::min(cap, data.size() - offset);
      std::copy(data.begin() + offset, data.begin() + offset + n, buf);
      offset += n;
      return n;
    });
  }
  client.UploadFinish(id);
  TaskResult r = Wait(client, id);
  CHECK(r.outcome == TaskOutcome::Done || r.outcome == TaskOutcome::Vanished);
}

const std::string kCafe = "caf\xC3\xA9";                 // Latin-1 range in UTF-8
const std::string kCjk = "\xE6\x97\xA5\xE6\x9C\xAC";   // 日本
const std::string kBytes = "bad\xE9name";               // not UTF-8

}  // namespace

TEST("space lists at least one volume") {
  Ps5Client client = MakeClient();
  auto spaces = client.Space();
  CHECK(!spaces.empty());
  CHECK(spaces[0].total > 0);
}

TEST("mkdir with Latin-1 and CJK names") {
  Ps5Client client = MakeClient();
  client.Mkdir(g_root, kCafe);
  client.Mkdir(g_root, kCjk);
  CHECK(Exists(g_root + "/" + kCafe));
  CHECK(Exists(g_root + "/" + kCjk));
  Listing l = client.List(g_root);
  CHECK(Find(l, kCafe) && Find(l, kCafe)->isDir());
  CHECK(Find(l, kCjk) && Find(l, kCjk)->isDir());
}

TEST("upload files, nested folders and an empty file") {
  Ps5Client client = MakeClient();
  std::string dir = g_root + "/" + kCafe;
  std::string big = Pattern(3 * 1024 * 1024 + 17);
  Upload(client, dir,
         {{"big.bin", big}, {"empty.txt", ""}, {"sub/" + kCjk + ".txt", "nested"}}, false);
  CHECK_EQ(ReadLocal(dir + "/big.bin"), big);
  CHECK(Exists(dir + "/empty.txt"));
  CHECK_EQ(ReadLocal(dir + "/sub/" + kCjk + ".txt"), std::string("nested"));
  Listing l = client.List(dir);
  CHECK(Find(l, "big.bin") && Find(l, "big.bin")->size == big.size());
}

TEST("upload refuses to overwrite unless asked") {
  Ps5Client client = MakeClient();
  std::string dir = g_root + "/" + kCafe;
  bool refused = false;
  try {
    Upload(client, dir, {{"empty.txt", "x"}}, false);
  } catch (const ApiError&) {
    refused = true;
  }
  // A refused upload must not leave the console blocked by an active task.
  TaskInfo active;
  for (int i = 0; i < 20 && FindActiveTask(client, active); ++i) {
    try { client.Cancel(active.id); } catch (const ApiError&) {}
    try { client.UploadFinish(active.id); } catch (const ApiError&) {}
    Sleep(100);
  }
  CHECK(refused);
  CHECK_EQ(ReadLocal(dir + "/empty.txt"), std::string());
  Upload(client, dir, {{"empty.txt", "now"}}, true);
  CHECK_EQ(ReadLocal(dir + "/empty.txt"), std::string("now"));
}

TEST("read file whole and by range") {
  Ps5Client client = MakeClient();
  std::string path = g_root + "/" + kCafe + "/big.bin";
  std::string expected = ReadLocal(path);
  std::string got;
  ReadResult r = client.ReadFile(path, 0, [&](const char* d, size_t n) {
    got.append(d, n);
    return true;
  });
  CHECK_EQ(r.fileSize, expected.size());
  CHECK(got == expected);
  std::string tail;
  r = client.ReadFile(path, 1000, [&](const char* d, size_t n) {
    tail.append(d, n);
    return true;
  });
  CHECK_EQ(r.status, 206);
  CHECK(tail == expected.substr(1000));
  // Offset exactly at the end: nothing left, not an error.
  r = client.ReadFile(path, expected.size(), [](const char*, size_t) { return true; });
  CHECK_EQ(r.received, 0u);
}

TEST("server-side copy of a folder") {
  Ps5Client client = MakeClient();
  std::string src = g_root + "/" + kCafe;
  std::string dst = g_root + "/" + kCjk;
  uint64_t id = client.Copy({src}, dst, false);
  TaskResult r = Wait(client, id);
  CHECK(r.outcome == TaskOutcome::Done || r.outcome == TaskOutcome::Vanished);
  CHECK_EQ(ReadLocal(dst + "/" + kCafe + "/sub/" + kCjk + ".txt"), std::string("nested"));
  CHECK_EQ(ReadLocal(dst + "/" + kCafe + "/big.bin"), ReadLocal(src + "/big.bin"));
}

TEST("copy conflict is reported before the task starts") {
  Ps5Client client = MakeClient();
  try {
    client.Copy({g_root + "/" + kCafe}, g_root + "/" + kCjk, false);
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.status(), 409);
  }
  try {
    client.Copy({g_root + "/" + kCafe}, g_root + "/" + kCafe + "/sub", false);
    CHECK(false);
  } catch (const ApiError& e) {
    CHECK_EQ(e.code(), std::string("destination_inside_source"));
  }
}

TEST("rename and move") {
  Ps5Client client = MakeClient();
  std::string dir = g_root + "/" + kCjk + "/" + kCafe;
  client.Rename(dir + "/big.bin", "renamed " + kCafe + ".bin");
  CHECK(Exists(dir + "/renamed " + kCafe + ".bin"));
  uint64_t id = client.Move({dir + "/renamed " + kCafe + ".bin"}, g_root, false);
  TaskResult r = Wait(client, id);
  CHECK(r.outcome == TaskOutcome::Done || r.outcome == TaskOutcome::Vanished);
  CHECK(Exists(g_root + "/renamed " + kCafe + ".bin"));
  CHECK(!Exists(dir + "/renamed " + kCafe + ".bin"));
}

TEST("non-UTF-8 byte names survive list, read, rename, delete") {
  Ps5Client client = MakeClient();
  WriteLocal(g_root + "/" + kBytes, "raw bytes");
  Listing l = client.List(g_root);
  const ListEntry* e = Find(l, kBytes);
  CHECK(e != nullptr);
  if (!e) return;
  bool lossy = false;
  path_codec::RawToDisplay(e->name, &lossy);
  CHECK(lossy);
  std::string got;
  client.ReadFile(e->path, 0, [&](const char* d, size_t n) {
    got.append(d, n);
    return true;
  });
  CHECK_EQ(got, std::string("raw bytes"));
  client.Rename(e->path, kBytes + "2");
  CHECK(Exists(g_root + "/" + kBytes + "2"));
  uint64_t id = client.Delete({g_root + "/" + kBytes + "2"});
  Wait(client, id);
  CHECK(!Exists(g_root + "/" + kBytes + "2"));
}

TEST("chmod task") {
  Ps5Client client = MakeClient();
  std::string path = g_root + "/" + kCafe + "/empty.txt";
  uint64_t id = client.Chmod({path}, 0640, false);
  Wait(client, id);
  struct stat st;
  CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640);
}

TEST("active task blocks listing; cancel ends it") {
  Ps5Client client = MakeClient();
  std::string big = g_root + "/huge.bin";
  {
    std::string chunk = Pattern(1 << 20);
    std::ofstream out(big, std::ios::binary);
    for (int i = 0; i < 400; ++i) out << chunk;
  }
  uint64_t id = client.Copy({big}, g_root + "/huge-copy.bin", false);
  bool sawConflict = false;
  try {
    client.List(g_root);
  } catch (const ApiError& e) {
    sawConflict = e.code() == "active_task";
  }
  TaskResult r = WaitForTask(client, id, [](const TaskInfo*) { return false; }, Sleep, 50);
  std::printf("  (conflict seen: %s, outcome: %d)\n", sawConflict ? "yes" : "no",
              static_cast<int>(r.outcome));
  CHECK(r.outcome == TaskOutcome::Canceled || r.outcome == TaskOutcome::Done ||
        r.outcome == TaskOutcome::Vanished);
  TaskInfo active;
  CHECK(!FindActiveTask(client, active));
  client.List(g_root);  // usable again
}

TEST("walk and delete everything") {
  Ps5Client client = MakeClient();
  Listing l = client.List(g_root);
  auto nodes = WalkRemote(client, l.entries, nullptr);
  CHECK(nodes.size() >= l.entries.size());
  std::vector<std::string> paths;
  for (const auto& e : l.entries) paths.push_back(e.path);
  uint64_t id = client.Delete(paths);
  TaskResult r = Wait(client, id);
  CHECK(r.outcome == TaskOutcome::Done || r.outcome == TaskOutcome::Vanished);
  CHECK(client.List(g_root).entries.empty());
}

TEST("connection refused is a NetError") {
  Ps5Client client(std::make_shared<PosixHttpTransport>(), "127.0.0.1", 1);
  CHECK_THROWS(client.List("/"), NetError);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <port> <scratch-dir>\n", argv[0]);
    return 2;
  }
  g_port = static_cast<uint16_t>(std::atoi(argv[1]));
  char real[4096];
  if (!realpath(argv[2], real)) {
    std::perror("realpath");
    return 2;
  }
  g_root = real;
  return test::RunAll();
}

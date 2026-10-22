#include "Operations.hpp"

#include <cstdlib>

#include "PathCodec.hpp"
#include "Text.hpp"

namespace ps5 {

TaskResult WaitForTask(Ps5Client& client, uint64_t taskId,
                       const std::function<bool(const TaskInfo* task)>& onPoll,
                       const std::function<void(int ms)>& sleepMs, int pollMs) {
  TaskResult result;
  bool seen = false;
  bool cancelSent = false;
  int missing = 0;
  for (;;) {
    std::vector<TaskInfo> tasks = client.Tasks();
    const TaskInfo* task = nullptr;
    for (const auto& t : tasks) {
      if (t.id == taskId) {
        task = &t;
        break;
      }
    }
    if (task) {
      seen = true;
      missing = 0;
      result.last = *task;
      if (task->state == "done") {
        result.outcome = TaskOutcome::Done;
        onPoll(task);
        return result;
      }
      if (task->state == "failed") {
        result.outcome = TaskOutcome::Failed;
        onPoll(task);
        return result;
      }
      if (task->state == "canceled") {
        result.outcome = TaskOutcome::Canceled;
        onPoll(task);
        return result;
      }
    } else {
      // A finished task is dropped after one report, so if something else
      // (a browser tab) polled first we only see it disappear.
      if (seen || ++missing >= 3) {
        result.outcome = cancelSent ? TaskOutcome::Canceled : TaskOutcome::Vanished;
        return result;
      }
    }
    bool keepGoing = onPoll(task);
    if (!keepGoing && !cancelSent) {
      cancelSent = true;
      try {
        client.Cancel(taskId);
      } catch (const ApiError&) {
        // Already finished, or not cancellable (PKG install).
      }
    }
    sleepMs(pollMs);
  }
}

bool FindActiveTask(Ps5Client& client, TaskInfo& out) {
  for (const auto& t : client.Tasks()) {
    if (t.active()) {
      out = t;
      return true;
    }
  }
  return false;
}

std::vector<RemoteNode> WalkRemote(Ps5Client& client, const std::vector<ListEntry>& roots,
                                   const std::function<bool(const std::string& rawDir)>& onDir) {
  std::vector<RemoteNode> out;
  struct Pending {
    ListEntry entry;
    std::string rel;
  };
  std::vector<Pending> stack;
  for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back({*it, it->name});
  while (!stack.empty()) {
    Pending item = std::move(stack.back());
    stack.pop_back();
    char t = item.entry.type;
    if (t == 'c' || t == 'b' || t == 'p' || t == 's') continue;
    out.push_back({item.entry, item.rel});
    if (!item.entry.isDir()) continue;
    if (onDir && !onDir(item.entry.path)) throw CancelledError();
    Listing listing = client.List(item.entry.path);
    for (auto it = listing.entries.rbegin(); it != listing.entries.rend(); ++it) {
      stack.push_back({*it, item.rel + "/" + it->name});
    }
  }
  return out;
}

namespace {

std::string SpaceMessage(const std::string& arg) {
  size_t comma = arg.find(',');
  if (comma == std::string::npos) return "Not enough free space on the target";
  uint64_t need = std::strtoull(arg.c_str(), nullptr, 10);
  uint64_t have = std::strtoull(arg.c_str() + comma + 1, nullptr, 10);
  return "Not enough free space: need " + FormatSize(need) + ", " + FormatSize(have) + " free";
}

std::string Describe(const std::string& code, const std::string& message, const std::string& arg) {
  if (code == "active_task")
    return "Another task is already running on the console";
  if (code == "file_not_found") return "File not found on the console";
  if (code == "no_space") return SpaceMessage(arg);
  if (code == "source_destination_same") return "Source and destination are the same";
  if (code == "destination_inside_source") return "Cannot copy or move a folder into itself";
  if (code == "destination_must_be_directory") return "Destination must be a folder";
  if (code == "file_already_exists") return "A file with that name already exists";
  if (code == "invalid_path") return "Invalid path";
  if (code == "pkg_install_unsupported") return "PKG install is only available on a real PS5";
  if (code == "pkg_type_invalid") return "The file is not a PKG package";
  if (code == "elf_type_invalid") return "The file is not an ELF payload";
  if (code == "elf_launch_unsupported") return "ELF launching is only available on a real PS5";
  if (code == "elf_launch_failed") return "Could not send the ELF payload to elfldr";
  if (code == "smb_password_required") return "The SMB share needs a password";
  if (code == "smb_connect_failed") return "Could not connect to the SMB share" + (arg.empty() ? std::string() : ": " + arg);
  if (code == "unknown_api") return "This payload version does not support the operation";
  if (code == "range_ignored") return "The server does not support resuming";
  std::string text = message.empty() ? code : message;
  if (!code.empty() && code != "system_error" && code != text) text += " [" + code + "]";
  return text;
}

}  // namespace

std::string DescribeApiError(const ApiError& error) {
  return Describe(error.code(), error.message(), error.arg());
}

std::string DescribeTaskError(const TaskInfo& task) {
  if (task.errorCode.empty() && task.error.empty()) return "The operation failed";
  return Describe(task.errorCode, task.error, task.errorArg);
}

}  // namespace ps5

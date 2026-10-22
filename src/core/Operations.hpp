#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Ps5Client.hpp"

namespace ps5 {

enum class TaskOutcome { Done, Failed, Canceled, Vanished };

struct TaskResult {
  TaskOutcome outcome = TaskOutcome::Vanished;
  TaskInfo last;  // last state seen (empty if the task was never seen)
};

// Polls /api/tasks until the task reaches a terminal state. `onPoll` is
// called after every poll with the task (nullptr if not listed) and returns
// false to request cancellation; the cancel is sent once and polling goes on
// until the server confirms. `sleepMs` lets the caller keep its UI alive.
TaskResult WaitForTask(Ps5Client& client, uint64_t taskId,
                       const std::function<bool(const TaskInfo* task)>& onPoll,
                       const std::function<void(int ms)>& sleepMs, int pollMs = 300);

// Returns the active task, if any, so callers can offer to wait or cancel.
bool FindActiveTask(Ps5Client& client, TaskInfo& out);

struct RemoteNode {
  ListEntry entry;  // entry.path is the absolute raw path
  std::string rel;  // raw path relative to the walk root's parent, '/' separated
};

// Expands `roots` depth-first. Directories come before their contents.
// Special files (devices, sockets, fifos) are skipped. `onDir` is called
// before each directory is listed and returns false to cancel (throws
// CancelledError).
std::vector<RemoteNode> WalkRemote(Ps5Client& client, const std::vector<ListEntry>& roots,
                                   const std::function<bool(const std::string& rawDir)>& onDir);

// Human-readable text for an ApiError, using the payload's error codes.
std::string DescribeApiError(const ApiError& error);
std::string DescribeTaskError(const TaskInfo& task);

}  // namespace ps5

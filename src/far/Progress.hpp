#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "../core/Ps5Client.hpp"

// Far-style progress window drawn with a button-less Message. Throttled to
// ~10 redraws per second. Esc asks for confirmation; CheckCancel() returns
// true when the user confirmed.
class Progress {
public:
  explicit Progress(std::wstring title, bool visible = true);
  ~Progress();
  Progress(const Progress&) = delete;
  Progress& operator=(const Progress&) = delete;

  // Local transfers: byte and file totals, then per-file updates.
  void SetTotals(uint64_t bytes, uint64_t files);
  void BeginFile(const std::wstring& from, const std::wstring& to, uint64_t size, uint64_t startAt = 0);
  void AddBytes(uint64_t bytes);
  void EndFile();

  // One-line status ("Scanning ...").
  void Status(const std::wstring& text);

  // Server-side task (copy, move, delete, ...).
  void ServerTask(const ps5::TaskInfo& task);

  // Extra line at the bottom of the window (e.g. which keys do what).
  void SetHint(std::wstring hint) { hint_ = std::move(hint); }

  // Polls Esc. Returns true if the user confirmed cancelling.
  bool CheckCancel();
  // CheckCancel() + throws ps5::CancelledError.
  void ThrowIfCancelled();

  void Draw(bool force = false);

private:
  std::wstring title_;
  bool visible_;
  HANDLE screen_ = nullptr;
  ULONGLONG lastDraw_ = 0;
  ULONGLONG started_ = 0;

  enum class Mode { Status, Transfer, Task } mode_ = Mode::Status;
  std::wstring status_;
  std::wstring from_, to_;
  uint64_t fileSize_ = 0, fileDone_ = 0;
  uint64_t totalBytes_ = 0, doneBytes_ = 0;
  uint64_t totalFiles_ = 0, doneFiles_ = 0;
  uint64_t sessionBytes_ = 0;  // bytes moved in this run, for speed
  ps5::TaskInfo task_;
  std::wstring hint_;
};

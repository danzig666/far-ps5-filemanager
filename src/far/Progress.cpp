#include "Progress.hpp"

#include <algorithm>
#include <vector>

#include "../core/Http.hpp"
#include "../core/PathCodec.hpp"
#include "../core/Text.hpp"
#include "FarApi.hpp"
#include "Guids.hpp"

namespace {

constexpr size_t kWidth = 50;

std::wstring Bar(uint64_t done, uint64_t total) {
  size_t filled = total ? static_cast<size_t>((done > total ? total : done) * kWidth / total) : 0;
  int percent = total ? static_cast<int>((done > total ? total : done) * 100 / total) : 0;
  std::wstring bar(filled, L'\x2588');
  bar.append(kWidth - filled, L'\x2591');
  wchar_t pct[8];
  swprintf(pct, 8, L"%3d%%", percent);
  return bar + L" " + pct;
}

std::wstring Fit(const std::wstring& text, size_t width = kWidth + 5) {
  if (text.size() <= width) return text;
  size_t keep = width - 3;
  return text.substr(0, keep / 2) + L"..." + text.substr(text.size() - (keep - keep / 2));
}

std::wstring OpTitle(const std::string& op) {
  if (op == "copy") return L"Copying on the console";
  if (op == "move") return L"Moving on the console";
  if (op == "delete") return L"Deleting";
  if (op == "chmod") return L"Changing permissions";
  if (op == "extract") return L"Extracting";
  if (op == "pkg_install") return L"Installing package";
  if (op == "upload") return L"Uploading";
  if (op == "download") return L"Downloading";
  return fui::W(op);
}

}  // namespace

Progress::Progress(std::wstring title, bool visible)
    : title_(std::move(title)), visible_(visible), hint_(L"Esc or F8: stop the transfer (you will be asked first)") {
  if (visible_) screen_ = PsInfo.SaveScreen(0, 0, -1, -1);
  started_ = GetTickCount64();
}

Progress::~Progress() {
  if (visible_) {
    fui::TaskbarClear();
    if (screen_) PsInfo.RestoreScreen(screen_);
  }
}

void Progress::SetTotals(uint64_t bytes, uint64_t files) {
  mode_ = Mode::Transfer;
  totalBytes_ = bytes;
  totalFiles_ = files;
}

void Progress::BeginFile(const std::wstring& from, const std::wstring& to, uint64_t size, uint64_t startAt) {
  mode_ = Mode::Transfer;
  from_ = from;
  to_ = to;
  fileSize_ = size;
  fileDone_ = startAt;
  doneBytes_ += startAt;
  Draw(true);
}

void Progress::AddBytes(uint64_t bytes) {
  fileDone_ += bytes;
  doneBytes_ += bytes;
  sessionBytes_ += bytes;
  Draw();
}

void Progress::EndFile() {
  ++doneFiles_;
  if (fileDone_ < fileSize_) doneBytes_ += fileSize_ - fileDone_;
  fileDone_ = fileSize_;
}

void Progress::Status(const std::wstring& text) {
  mode_ = Mode::Status;
  status_ = text;
  Draw();
}

void Progress::ServerTask(const ps5::TaskInfo& task) {
  mode_ = Mode::Task;
  task_ = task;
  Draw();
}

bool Progress::CheckCancel() {
  if (!fui::PollKey()) return false;
  // "Continue" is the first, default button: Enter or Esc keeps going.
  bool stop = fui::Message(FMSG_WARNING,
                           {title_, L"Stop the transfer?", L"The file being copied right now will not be finished.",
                            L"Files that are already finished are kept."},
                           {L"&Continue", L"&Stop the transfer"}, MessageGuid) == 1;
  Draw(true);
  return stop;
}

void Progress::ThrowIfCancelled() {
  if (CheckCancel()) throw ps5::CancelledError();
}

void Progress::Draw(bool force) {
  if (!visible_) return;
  ULONGLONG now = GetTickCount64();
  if (!force && now - lastDraw_ < 100) return;
  lastDraw_ = now;

  std::vector<std::wstring> lines{title_};
  uint64_t elapsed = (now - started_) / 1000;
  switch (mode_) {
    case Mode::Status:
      lines.push_back(Fit(status_));
      break;
    case Mode::Transfer: {
      lines.push_back(Fit(from_));
      lines.push_back(L"to");
      lines.push_back(Fit(to_));
      lines.push_back(Bar(fileDone_, fileSize_));
      lines.push_back(L"\x1");  // separator
      wchar_t files[64];
      swprintf(files, 64, L"Files: %llu of %llu", static_cast<unsigned long long>(doneFiles_),
               static_cast<unsigned long long>(totalFiles_));
      lines.push_back(std::wstring(files) + L"   " + fui::FormatSize(doneBytes_) + L" of " +
                      fui::FormatSize(totalBytes_));
      lines.push_back(Bar(doneBytes_, totalBytes_));
      uint64_t speed = elapsed ? sessionBytes_ / elapsed : 0;
      std::wstring timing = L"Time " + fui::W(ps5::FormatDuration(elapsed));
      if (speed) {
        timing += L"   " + fui::FormatSize(speed) + L"/s";
        if (totalBytes_ > doneBytes_)
          timing += L"   Left " + fui::W(ps5::FormatDuration((totalBytes_ - doneBytes_) / speed));
      }
      lines.push_back(timing);
      fui::TaskbarProgress(doneBytes_, totalBytes_);
      break;
    }
    case Mode::Task: {
      lines[0] = OpTitle(task_.op);
      std::wstring src = ps5::path_codec::RawToDisplay(task_.src);
      std::wstring current = ps5::path_codec::RawToDisplay(task_.current);
      lines.push_back(Fit(src));
      if (!task_.dst.empty() && task_.op != "delete" && task_.op != "chmod") {
        lines.push_back(L"to");
        lines.push_back(Fit(ps5::path_codec::RawToDisplay(task_.dst)));
      }
      if (!current.empty() && current != src) {
        lines.push_back(L"\x1");
        lines.push_back(Fit(L"Now: " + current));
      }
      lines.push_back(Bar(task_.done, task_.total));
      std::wstring info = fui::FormatSize(task_.done) + L" of " + fui::FormatSize(task_.total);
      if (task_.srcCount > 1)
        info += L"   Item " + std::to_wstring(std::min(task_.completedCount + 1, task_.srcCount)) + L" of " +
                std::to_wstring(task_.srcCount);
      lines.push_back(info);
      std::wstring timing = L"Speed " + (task_.speed ? fui::FormatSize(task_.speed) + L"/s" : std::wstring(L"-"));
      timing += L"   Elapsed " + fui::W(ps5::FormatDuration(task_.elapsed));
      timing += L"   Left " + (task_.eta ? fui::W(ps5::FormatDuration(task_.eta)) : std::wstring(L"-"));
      lines.push_back(timing);
      if (task_.state == "queued") lines.push_back(L"Queued...");
      if (task_.cancelRequested) lines.push_back(L"Cancelling...");
      fui::TaskbarProgress(task_.done, task_.total);
      break;
    }
  }
  if (!hint_.empty()) {
    lines.push_back(L"\x1");
    lines.push_back(Fit(hint_, 76));
  }
  // Far draws "\x1" lines as separators in messages.
  fui::Message(FMSG_LEFTALIGN, lines, {}, ProgressGuid);
}

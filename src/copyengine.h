// Silnik operacji na plikach: kopiowanie / przenoszenie / trwałe usuwanie (Linux).
//  - przenoszenie w obrębie systemu plików = natychmiastowy rename(2)
//  - kopiowanie równoległe (liczba wątków zależy od typu dysków), reflink (FICLONE) -> copy_file_range -> read/write,
//    duże pliki bez zaśmiecania cache (sync_file_range + posix_fadvise)
//  - konflikty rozstrzygane z góry (w fazie planowania), z opcją "zastosuj do wszystkich"
//  - pauza / wznawianie / anulowanie, prędkość, ETA
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "util.h"

namespace nv {

enum class JobOp { Copy, Move, Delete };
enum class JobState { Scanning = 0, Running = 1, Done = 2, DoneErrors = 3, Cancelled = 4 };
enum class Resolution { Replace, Skip, KeepBoth, Cancel };

struct ConflictInfo {
  std::wstring src, dst;
  bool isDir = false;
  u64 srcSize = 0, dstSize = 0;
  i64 srcTime = 0, dstTime = 0;
};
using ConflictFn = std::function<Resolution(const ConflictInfo&, bool& applyAll)>;

struct JobError {
  std::wstring path;
  u32 code = 0;
  std::wstring msg;
};

class CopyJob {
 public:
  u32 id = 0;
  JobOp op = JobOp::Copy;
  std::wstring destDir;
  std::vector<std::wstring> sources;

  std::atomic<u64> totalBytes{0}, doneBytes{0};
  std::atomic<u32> totalFiles{0}, doneFiles{0}, skipped{0};
  std::atomic<int> state{(int)JobState::Scanning};
  std::atomic<bool> cancel{false}, pause{false};

  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point finishedAt;

  // wątkowo-bezpieczne
  std::wstring Current();
  void SetCurrent(const std::wstring& s);
  std::vector<JobError> Errors();
  void AddError(const std::wstring& path, u32 code);
  void AddErrorMsg(const std::wstring& path, const std::wstring& msg);

  // wywoływane z wątku UI
  void Tick();
  double speed = 0;  // B/s (wygładzone)
  double Eta() const;
  double Fraction() const;
  bool Finished() const { return state.load() >= (int)JobState::Done; }
  JobState State() const { return (JobState)state.load(); }

  ConflictFn conflict;
  std::thread th;

 private:
  std::mutex mx_;
  std::wstring current_;
  std::vector<JobError> errors_;
  std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
  u64 lastBytes_ = 0;
};

class JobManager {
 public:
  ~JobManager();
  void SetNotify(std::function<void(u32)> n) { notify_ = std::move(n); }
  std::shared_ptr<CopyJob> Start(JobOp op, std::vector<std::wstring> sources, std::wstring destDir, ConflictFn conflict);
  std::vector<std::shared_ptr<CopyJob>> Jobs();
  void Dismiss(u32 id);
  bool AnyActive();
  void CancelAll();

 private:
  void Run(std::shared_ptr<CopyJob> j);
  std::mutex mx_;
  std::vector<std::shared_ptr<CopyJob>> jobs_;
  u32 nextId_ = 1;
  std::function<void(u32)> notify_;
  friend struct JobRunner;
};

std::wstring UniqueName(const std::wstring& dir, const std::wstring& name, bool copySuffix, bool isDir);
std::wstring SysErrorText(u32 code);  // errno -> tekst

}  // namespace nv

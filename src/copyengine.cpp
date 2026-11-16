#include "copyengine.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "fastfs.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

namespace nv {

static void SleepMs(int ms) { usleep((useconds_t)ms * 1000); }

// ------------------------------------------------------------------ CopyJob
std::wstring CopyJob::Current() {
  std::lock_guard<std::mutex> l(mx_);
  return current_;
}
void CopyJob::SetCurrent(const std::wstring& s) {
  std::lock_guard<std::mutex> l(mx_);
  current_ = s;
}
std::vector<JobError> CopyJob::Errors() {
  std::lock_guard<std::mutex> l(mx_);
  return errors_;
}
void CopyJob::AddError(const std::wstring& path, u32 code) { AddErrorMsg(path, SysErrorText(code)); }
void CopyJob::AddErrorMsg(const std::wstring& path, const std::wstring& msg) {
  std::lock_guard<std::mutex> l(mx_);
  errors_.push_back({path, 0, msg});
}

void CopyJob::Tick() {
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - lastTick_).count();
  if (dt < 0.25) return;
  u64 d = doneBytes.load();
  double inst = (double)(d >= lastBytes_ ? d - lastBytes_ : 0) / dt;
  speed = speed <= 0 ? inst : speed * 0.75 + inst * 0.25;
  lastBytes_ = d;
  lastTick_ = now;
}

double CopyJob::Eta() const {
  u64 t = totalBytes.load(), d = doneBytes.load();
  if (speed < 1 || t <= d) return -1;
  return (double)(t - d) / speed;
}

double CopyJob::Fraction() const {
  u64 t = totalBytes.load();
  if (t == 0) {
    u32 tf = totalFiles.load();
    return tf ? (double)doneFiles.load() / tf : 0;
  }
  double f = (double)doneBytes.load() / (double)t;
  return f > 1 ? 1 : f;
}

std::wstring SysErrorText(u32 code) { return ErrnoText(code); }

std::wstring UniqueName(const std::wstring& dir, const std::wstring& name, bool copySuffix, bool isDir) {
  std::wstring stem = name, ext;
  if (!isDir) {
    size_t k = name.rfind(L'.');
    if (k != std::wstring::npos && k > 0) {
      stem = name.substr(0, k);
      ext = name.substr(k);
    }
  }
  FileStat fs;
  for (int n = copySuffix ? 1 : 2; n < 100000; n++) {
    std::wstring cand;
    if (copySuffix) cand = stem + L" - Kopia" + (n > 1 ? L" (" + std::to_wstring(n) + L")" : L"") + ext;
    else cand = stem + L" (" + std::to_wstring(n) + L")" + ext;
    if (!StatPath(PathJoin(dir, cand), fs)) return cand;
  }
  return name;
}

// ------------------------------------------------------------------ JobManager
JobManager::~JobManager() {
  CancelAll();
  std::vector<std::shared_ptr<CopyJob>> v;
  {
    std::lock_guard<std::mutex> l(mx_);
    v = jobs_;
  }
  for (auto& j : v) {
    j->pause = false;
    if (j->th.joinable()) j->th.join();
  }
}

std::shared_ptr<CopyJob> JobManager::Start(JobOp op, std::vector<std::wstring> sources, std::wstring destDir, ConflictFn conflict) {
  auto j = std::make_shared<CopyJob>();
  {
    std::lock_guard<std::mutex> l(mx_);
    j->id = nextId_++;
    jobs_.push_back(j);
  }
  j->op = op;
  j->sources = std::move(sources);
  j->destDir = destDir.empty() ? destDir : PathNormalize(std::move(destDir));
  j->conflict = std::move(conflict);
  j->th = std::thread([this, j] { Run(j); });
  return j;
}

std::vector<std::shared_ptr<CopyJob>> JobManager::Jobs() {
  std::lock_guard<std::mutex> l(mx_);
  return jobs_;
}

void JobManager::Dismiss(u32 id) {
  std::shared_ptr<CopyJob> j;
  {
    std::lock_guard<std::mutex> l(mx_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
      if ((*it)->id == id) {
        j = *it;
        jobs_.erase(it);
        break;
      }
  }
  if (j) {
    j->cancel = true;
    j->pause = false;
    if (j->th.joinable()) {
      if (j->Finished()) j->th.join();
      else j->th.detach();
    }
  }
}

bool JobManager::AnyActive() {
  std::lock_guard<std::mutex> l(mx_);
  for (auto& j : jobs_)
    if (!j->Finished()) return true;
  return false;
}

void JobManager::CancelAll() {
  std::lock_guard<std::mutex> l(mx_);
  for (auto& j : jobs_) {
    j->cancel = true;
    j->pause = false;
  }
}

// ------------------------------------------------------------------ wykonanie
namespace {

struct FileTask {
  std::wstring src, dst;
  u64 size;
  bool replace;
  bool link;  // dowiązanie symboliczne: odtwarzane jako dowiązanie
};
struct DirTask {
  std::wstring src, dst;
  u32 attrs;
  bool create;
};

struct Ent {
  std::wstring name;
  u32 attrs;
  u64 size;
  i64 mtime;
  u32 tag;
};

static bool ListDirEnts(const std::wstring& dir, std::vector<Ent>& out, u32* err = nullptr) {
  out.clear();
  unsigned e = EnumDir(dir, [&](const FsEntry& f) {
    if (f.nameLen == 1 && f.name[0] == L'.') return;
    if (f.nameLen == 2 && f.name[0] == L'.' && f.name[1] == L'.') return;
    out.push_back({std::wstring(f.name, f.nameLen), f.attrs, f.size, f.mtime, f.reparseTag});
  });
  if (err) *err = e;
  return e == 0;
}

static bool IsLink(const Ent& e) { return e.tag == TAG_SYMLINK; }

static bool Exists(const std::wstring& p, FileStat& fs) { return StatPath(p, fs); }

static bool RenameNoReplace(const char* a, const char* b, int* err) {
  long r = syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE);
  if (r == 0) return true;
  *err = errno;
  if (errno == ENOSYS || errno == EINVAL) {  // stare jądro / system plików bez RENAME_NOREPLACE
    struct stat st;
    if (lstat(b, &st) == 0) { *err = EEXIST; return false; }
    if (rename(a, b) == 0) return true;
    *err = errno;
  }
  return false;
}

static void CopyXattrs(int in, int out) {
  ssize_t n = flistxattr(in, nullptr, 0);
  if (n <= 0) return;
  std::vector<char> names((size_t)n);
  n = flistxattr(in, names.data(), names.size());
  for (ssize_t i = 0; i < n;) {
    const char* nm = names.data() + i;
    size_t l = strlen(nm);
    i += (ssize_t)l + 1;
    if (strncmp(nm, "user.", 5) != 0) continue;
    ssize_t vs = fgetxattr(in, nm, nullptr, 0);
    if (vs < 0) continue;
    std::vector<char> v((size_t)vs + 1);
    vs = fgetxattr(in, nm, v.data(), (size_t)vs);
    if (vs >= 0) fsetxattr(out, nm, v.data(), (size_t)vs, 0);
  }
}

}  // namespace

struct JobRunner {
  JobManager* mgr;
  std::shared_ptr<CopyJob> job;
  std::vector<FileTask> files;
  std::vector<DirTask> dirs;
  bool applyAll = false;
  Resolution allRes = Resolution::Skip;

  void Notify() {
    if (mgr->notify_) mgr->notify_(job->id);
  }

  Resolution Ask(const ConflictInfo& ci) {
    if (applyAll) return allRes;
    if (!job->conflict) return Resolution::Replace;
    bool all = false;
    Resolution r = job->conflict(ci, all);
    if (r == Resolution::Cancel) job->cancel = true;
    if (all) {
      applyAll = true;
      allRes = r;
    }
    return r;
  }

  // ---------------- planowanie kopiowania / przenoszenia między systemami plików
  void PlanFile(const std::wstring& src, const std::wstring& dstIn, const Ent& e, bool checkExisting) {
    std::wstring dst = dstIn;
    bool replace = false;
    if (checkExisting) {
      FileStat ds;
      if (Exists(dst, ds)) {
        ConflictInfo ci;
        ci.src = src;
        ci.dst = dst;
        ci.srcSize = e.size;
        ci.srcTime = e.mtime;
        ci.dstSize = ds.size;
        ci.dstTime = ds.mtime;
        ci.isDir = false;
        if (ds.attrs & A_DIR) {
          job->AddErrorMsg(dst, L"W miejscu docelowym istnieje folder o tej samej nazwie");
          job->skipped++;
          return;
        }
        Resolution r = Ask(ci);
        if (r == Resolution::Cancel) return;
        if (r == Resolution::Skip) {
          job->skipped++;
          return;
        }
        if (r == Resolution::KeepBoth) dst = PathJoin(PathParent(dst), UniqueName(PathParent(dst), PathName(dst), false, false));
        else replace = true;
      }
    }
    bool link = IsLink(e);
    files.push_back({src, dst, link ? 0 : e.size, replace, link});
    if (!link) job->totalBytes += e.size;
    job->totalFiles++;
  }

  void PlanDir(const std::wstring& src, const std::wstring& dst, u32 attrs, bool dstExists) {
    if (job->cancel) return;
    dirs.push_back({src, dst, attrs, !dstExists});
    std::vector<Ent> ents;
    u32 err = 0;
    if (!ListDirEnts(src, ents, &err)) {
      job->AddError(src, err);
      return;
    }
    job->SetCurrent(PathName(src));
    for (auto& e : ents) {
      if (job->cancel) return;
      std::wstring s = PathJoin(src, e.name), d = PathJoin(dst, e.name);
      if ((e.attrs & A_DIR) && !IsLink(e)) {
        bool childExists = false;
        if (dstExists) {
          FileStat ds;
          if (Exists(d, ds)) {
            if (!(ds.attrs & A_DIR)) {
              job->AddErrorMsg(d, L"W miejscu docelowym istnieje plik o tej samej nazwie co folder");
              continue;
            }
            childExists = true;
          }
        }
        PlanDir(s, d, e.attrs, childExists);
      } else {
        PlanFile(s, d, e, dstExists);
      }
    }
  }

  // ---------------- szybkie przenoszenie (ten sam system plików)
  // zwraca false, gdy trzeba przejść na kopiowanie (EXDEV)
  bool FastMove(const std::wstring& src, const std::wstring& dst, bool isDir, bool isLink, u64 size, i64 mtime) {
    if (job->cancel) return true;
    FileStat ds;
    job->SetCurrent(PathName(src));
    std::string ps = WToUtf8(src), pd = WToUtf8(dst);
    if (!Exists(dst, ds)) {
      int err = 0;
      if (RenameNoReplace(ps.c_str(), pd.c_str(), &err)) {
        job->totalFiles++;
        job->doneFiles++;
        job->totalBytes += size;
        job->doneBytes += size;
        return true;
      }
      if (err == EXDEV) return false;
      job->AddError(src, (u32)err);
      return true;
    }
    if (isDir && !isLink && (ds.attrs & A_DIR)) {  // scal foldery
      std::vector<Ent> ents;
      u32 err = 0;
      if (!ListDirEnts(src, ents, &err)) {
        job->AddError(src, err);
        return true;
      }
      for (auto& e : ents) {
        bool l = IsLink(e);
        if (!FastMove(PathJoin(src, e.name), PathJoin(dst, e.name), (e.attrs & A_DIR) != 0 && !l, l, e.size, e.mtime)) {
          // inny system plików wewnątrz drzewa: zaplanuj zwykłe kopiowanie tego elementu
          if ((e.attrs & A_DIR) && !l) PlanDir(PathJoin(src, e.name), PathJoin(dst, e.name), e.attrs, false);
          else PlanFile(PathJoin(src, e.name), PathJoin(dst, e.name), e, true);
        }
      }
      rmdir(ps.c_str());
      return true;
    }
    if ((isDir && !isLink) || (ds.attrs & A_DIR)) {
      job->AddErrorMsg(dst, L"Nie można zastąpić pliku folderem (lub odwrotnie)");
      return true;
    }
    ConflictInfo ci;
    ci.src = src;
    ci.dst = dst;
    ci.srcSize = size;
    ci.srcTime = mtime;
    ci.dstSize = ds.size;
    ci.dstTime = ds.mtime;
    Resolution r = Ask(ci);
    if (r == Resolution::Cancel) return true;
    if (r == Resolution::Skip) {
      job->skipped++;
      return true;
    }
    std::string target = pd;
    if (r == Resolution::KeepBoth) target = WToUtf8(PathJoin(PathParent(dst), UniqueName(PathParent(dst), PathName(dst), false, false)));
    if (rename(ps.c_str(), target.c_str()) == 0) {
      job->totalFiles++;
      job->doneFiles++;
      job->totalBytes += size;
      job->doneBytes += size;
    } else if (errno == EXDEV)
      return false;
    else
      job->AddError(src, (u32)errno);
    return true;
  }

  // ---------------- planowanie usuwania
  void PlanDelete(const std::wstring& path, const Ent& e) {
    if (job->cancel) return;
    if (!(e.attrs & A_DIR) || IsLink(e)) {
      files.push_back({path, L"", e.size, false, IsLink(e)});
      job->totalBytes += e.size;
      job->totalFiles++;
      return;
    }
    dirs.push_back({path, L"", e.attrs, false});
    std::vector<Ent> ents;
    u32 err = 0;
    if (!ListDirEnts(path, ents, &err)) {
      if (err == EACCES) {  // folder bez praw odczytu: spróbuj nadać prawa właścicielowi
        std::string p = WToUtf8(path);
        struct stat st;
        if (lstat(p.c_str(), &st) == 0) chmod(p.c_str(), (st.st_mode & 07777) | 0700);
        if (!ListDirEnts(path, ents, &err)) {
          job->AddError(path, err);
          return;
        }
      } else {
        job->AddError(path, err);
        return;
      }
    }
    for (auto& c : ents) PlanDelete(PathJoin(path, c.name), c);
  }

  // ---------------- wykonanie
  // 0 = OK, w przeciwnym razie errno; -1 = anulowano
  int CopyData(const FileTask& t, u64& counted) {
    std::string ps = WToUtf8(t.src), pd = WToUtf8(t.dst);
    int in = open(ps.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOATIME);
    if (in < 0 && errno == EPERM) in = open(ps.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (in < 0) return errno;
    struct stat st;
    if (fstat(in, &st) != 0) {
      int e = errno;
      close(in);
      return e;
    }
    if (S_ISDIR(st.st_mode)) {
      close(in);
      return EISDIR;
    }
    u64 size = S_ISREG(st.st_mode) ? (u64)st.st_size : 0;
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY | (t.replace ? O_TRUNC : O_EXCL);
    mode_t mode = st.st_mode & 07777;
    int out = open(pd.c_str(), flags, (mode & 0777) | 0200);
    if (out < 0 && t.replace && (errno == EACCES || errno == ETXTBSY || errno == EPERM)) {
      unlink(pd.c_str());  // plik tylko do odczytu / uruchomiony program: zastąp nowym i-węzłem
      out = open(pd.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, (mode & 0777) | 0200);
    }
    if (out < 0) {
      int e = errno;
      close(in);
      return e;
    }
    int rc = 0;
    const bool big = size >= (64ull << 20);
    if (big) posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
    auto onChunk = [&](u64 n) {
      job->doneBytes += n;
      counted += n;
    };
    auto waitPause = [&]() -> bool {
      while (job->pause.load() && !job->cancel.load()) SleepMs(40);
      return !job->cancel.load();
    };
    bool cloned = false;
    if (size > 0 && S_ISREG(st.st_mode) && ioctl(out, FICLONE, in) == 0) {  // reflink (btrfs, xfs, bcachefs): natychmiast
      cloned = true;
      onChunk(size);
    }
    if (!cloned && size > 0) {
      u64 off = 0;
      bool useCfr = true;
      const size_t CH = big ? (16u << 20) : (4u << 20);
      thread_local std::vector<char> buf;
      u64 syncedTo = 0;
      while (off < size) {
        if (!waitPause()) { rc = -1; break; }
        size_t want = (size_t)std::min<u64>(CH, size - off);
        ssize_t n = -1;
        if (useCfr) {
          loff_t oi = (loff_t)off, oo = (loff_t)off;
          n = copy_file_range(in, &oi, out, &oo, want, 0);
          if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP || errno == EBADF || errno == EPERM || errno == EISDIR)) {
            useCfr = false;
            continue;
          }
        }
        if (!useCfr) {
          if (buf.size() < (1u << 20)) buf.resize(1u << 20);
          want = std::min<size_t>(want, buf.size());
          ssize_t r = pread(in, buf.data(), want, (off_t)off);
          if (r < 0) {
            if (errno == EINTR) continue;
            rc = errno;
            break;
          }
          if (r == 0) break;
          size_t w = 0;
          while (w < (size_t)r) {
            ssize_t k = pwrite(out, buf.data() + w, (size_t)r - w, (off_t)(off + w));
            if (k < 0) {
              if (errno == EINTR) continue;
              rc = errno;
              break;
            }
            w += (size_t)k;
          }
          if (rc) break;
          n = r;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          rc = errno;
          break;
        }
        if (n == 0) break;  // plik się skrócił
        off += (u64)n;
        onChunk((u64)n);
        if (big && off - syncedTo >= (64ull << 20)) {  // nie zaśmiecaj cache dla bardzo dużych plików
          sync_file_range(out, (off64_t)syncedTo, (off64_t)(off - syncedTo), SYNC_FILE_RANGE_WRITE);
          if (syncedTo) {
            posix_fadvise(out, 0, (off_t)syncedTo, POSIX_FADV_DONTNEED);
            posix_fadvise(in, 0, (off_t)syncedTo, POSIX_FADV_DONTNEED);
          }
          syncedTo = off;
        }
      }
    }
    if (rc == 0) {
      struct timespec ts[2] = {st.st_atim, st.st_mtim};
      futimens(out, ts);
      if (geteuid() == 0) {
        if (fchown(out, st.st_uid, st.st_gid) != 0) {}
      }
      fchmod(out, mode);
      CopyXattrs(in, out);
    }
    close(in);
    if (close(out) != 0 && rc == 0) rc = errno;
    if (rc != 0) unlink(pd.c_str());  // nie zostawiaj plików częściowych
    return rc;
  }

  void CopyOne(const FileTask& t) {
    job->SetCurrent(PathName(t.src));
    std::string ps = WToUtf8(t.src), pd = WToUtf8(t.dst);
    if (t.link) {
      char target[4096];
      ssize_t n = readlink(ps.c_str(), target, sizeof target - 1);
      if (n < 0) {
        job->AddError(t.src, (u32)errno);
        job->doneFiles++;
        return;
      }
      target[n] = 0;
      if (t.replace) unlink(pd.c_str());
      if (symlink(target, pd.c_str()) != 0) {
        job->AddError(t.src, (u32)errno);
        job->doneFiles++;
        return;
      }
      job->doneFiles++;
      if (job->op == JobOp::Move && unlink(ps.c_str()) != 0) job->AddErrorMsg(t.src, L"Skopiowano, ale nie udało się usunąć źródła: " + SysErrorText((u32)errno));
      return;
    }
    u64 counted = 0;
    int rc = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
      counted = 0;
      rc = CopyData(t, counted);
      if (rc == 0 || rc == -1) break;
      if ((rc == EBUSY || rc == EAGAIN) && attempt < 3) {
        job->doneBytes -= std::min<u64>(job->doneBytes.load(), counted);
        SleepMs(150 * (attempt + 1));
        continue;
      }
      break;
    }
    if (rc == -1) return;  // anulowano (plik częściowy usunięty)
    if (rc != 0) {
      job->AddErrorMsg(t.src, SysErrorText((u32)rc));
      if (t.size > counted) job->doneBytes += t.size - counted;
      job->doneFiles++;
      return;
    }
    if (t.size > counted) job->doneBytes += t.size - counted;  // plik się skrócił / zero bajtów
    job->doneFiles++;
    if (job->op == JobOp::Move) {
      if (unlink(ps.c_str()) != 0) job->AddErrorMsg(t.src, L"Skopiowano, ale nie udało się usunąć źródła: " + SysErrorText((u32)errno));
    }
  }

  void DeleteOne(const FileTask& t) {
    job->SetCurrent(PathName(t.src));
    std::string ps = WToUtf8(t.src);
    for (int attempt = 0; attempt < 3; attempt++) {
      if (unlink(ps.c_str()) == 0) break;
      int err = errno;
      if (err == ENOENT) break;
      if ((err == EACCES || err == EPERM) && attempt == 0) {  // katalog nadrzędny bez prawa zapisu
        std::string par = WToUtf8(PathParent(t.src));
        struct stat st;
        if (stat(par.c_str(), &st) == 0) chmod(par.c_str(), (st.st_mode & 07777) | 0700);
        continue;
      }
      if (err == EBUSY && attempt < 2) {
        SleepMs(120);
        continue;
      }
      job->AddErrorMsg(t.src, SysErrorText((u32)err));
      break;
    }
    job->doneBytes += t.size;
    job->doneFiles++;
  }

  unsigned ChooseThreads(const std::wstring& a, const std::wstring& b) {
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    bool ssdA = IsSolidState(a), ssdB = b.empty() ? true : IsSolidState(b);
    unsigned t = (ssdA && ssdB) ? std::min(8u, hw) : 2u;
    if (IsNetworkPath(a) || (!b.empty() && IsNetworkPath(b))) t = 4;
    return t;
  }

  void RunPool(unsigned T, bool del) {
    if (files.empty()) return;
    T = (unsigned)std::min<size_t>(T, files.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= files.size() || job->cancel) return;
        while (job->pause && !job->cancel) SleepMs(40);
        if (del) DeleteOne(files[i]);
        else CopyOne(files[i]);
      }
    };
    std::vector<std::thread> th;
    for (unsigned i = 1; i < T; i++) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
  }

  static void CopyDirMeta(const DirTask& d) {
    std::string ps = WToUtf8(d.src), pd = WToUtf8(d.dst);
    struct stat st;
    if (stat(ps.c_str(), &st) != 0) return;
    chmod(pd.c_str(), st.st_mode & 07777);
    struct timespec ts[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, pd.c_str(), ts, 0);
    if (geteuid() == 0) {
      if (chown(pd.c_str(), st.st_uid, st.st_gid) != 0) {}
    }
  }

  static bool MkdirAll(const std::wstring& path, JobRunner* self) {
    FileStat dst;
    if (StatPath(path, dst)) return true;
    std::wstring p = path;
    std::vector<std::wstring> chain;
    while (!p.empty() && !StatPath(p, dst)) {
      chain.push_back(p);
      p = PathParent(p);
    }
    for (size_t i = chain.size(); i-- > 0;)
      if (mkdir(WToUtf8(chain[i]).c_str(), 0777) != 0 && errno != EEXIST) {
        self->job->AddError(chain[i], (u32)errno);
        return false;
      }
    return true;
  }

  void Execute() {
    job->state = (int)JobState::Scanning;
    Notify();
    const bool isDelete = job->op == JobOp::Delete;
    std::wstring dest = job->destDir;

    if (isDelete) {
      for (auto& s : job->sources) {
        if (job->cancel) break;
        FileStat st;
        if (!StatPath(s, st)) {
          job->AddError(s, ENOENT);
          continue;
        }
        Ent e{PathName(s), st.attrs, st.size, st.mtime, (st.attrs & A_REPARSE) ? TAG_SYMLINK : 0u};
        PlanDelete(s, e);
      }
      job->state = (int)JobState::Running;
      Notify();
      RunPool(ChooseThreads(job->sources.empty() ? L"" : PathParent(job->sources[0]), L""), true);
      for (size_t i = dirs.size(); i-- > 0;) {
        if (job->cancel) break;
        std::string pd = WToUtf8(dirs[i].src);
        int rc = rmdir(pd.c_str());
        if (rc != 0 && errno == EACCES) {
          std::string par = WToUtf8(PathParent(dirs[i].src));
          struct stat st;
          if (stat(par.c_str(), &st) == 0) chmod(par.c_str(), (st.st_mode & 07777) | 0700);
          rc = rmdir(pd.c_str());
        }
        if (rc != 0 && errno != ENOENT) job->AddErrorMsg(dirs[i].src, SysErrorText((u32)errno));
      }
      return;
    }

    // --- cel
    if (!MkdirAll(dest, this)) return;
    u64 destSerial = VolumeSerial(dest);
    for (auto& src : job->sources) {
      if (job->cancel) break;
      FileStat ss;
      if (!StatPath(src, ss)) {
        job->AddError(src, ENOENT);
        continue;
      }
      bool isLink = (ss.attrs & A_REPARSE) != 0;
      bool isDir = (ss.attrs & A_DIR) != 0 && !isLink;
      std::wstring name = PathName(src);
      std::wstring dstPath = PathJoin(dest, name);
      bool sameFolder = PathNormalize(PathParent(src)) == dest;
      if (sameFolder) {
        if (job->op == JobOp::Move) {
          job->skipped++;
          continue;
        }
        dstPath = PathJoin(dest, UniqueName(dest, name, true, isDir));
      }
      if (isDir && PathIsUnder(dest, src)) {
        job->AddErrorMsg(src, L"Nie można skopiować/przenieść folderu do niego samego ani jego podfolderu");
        continue;
      }
      u64 srcSerial = VolumeSerial(PathParent(src));
      Ent e{name, ss.attrs, ss.size, ss.mtime, isLink ? TAG_SYMLINK : 0u};
      if (job->op == JobOp::Move && srcSerial && srcSerial == destSerial) {
        if (sameFolder) continue;
        if (FastMove(src, dstPath, isDir, isLink, ss.size, ss.mtime)) continue;
        // EXDEV (np. podwolumeny btrfs): zwykłe kopiowanie + usunięcie źródła
      }
      if (isDir) {
        FileStat ds;
        bool exists = !sameFolder && Exists(dstPath, ds);
        if (exists && !(ds.attrs & A_DIR)) {
          job->AddErrorMsg(dstPath, L"W miejscu docelowym istnieje plik o tej samej nazwie co folder");
          continue;
        }
        PlanDir(src, dstPath, ss.attrs, exists);
      } else {
        PlanFile(src, dstPath, e, !sameFolder);
      }
    }
    if (job->cancel) return;

    job->state = (int)JobState::Running;
    Notify();
    for (auto& d : dirs)
      if (d.create && mkdir(WToUtf8(d.dst).c_str(), 0777) != 0 && errno != EEXIST) job->AddError(d.dst, (u32)errno);
    RunPool(ChooseThreads(job->sources.empty() ? L"" : PathParent(job->sources[0]), dest), false);
    for (size_t i = dirs.size(); i-- > 0;) {
      if (dirs[i].create) CopyDirMeta(dirs[i]);
      if (job->op == JobOp::Move && !job->cancel) {
        std::string ps = WToUtf8(dirs[i].src);
        if (rmdir(ps.c_str()) != 0 && errno == EACCES) {
          std::string par = WToUtf8(PathParent(dirs[i].src));
          struct stat st;
          if (stat(par.c_str(), &st) == 0) chmod(par.c_str(), (st.st_mode & 07777) | 0700);
          rmdir(ps.c_str());  // powiedzie się tylko dla pustych
        }
      }
    }
  }
};

void JobManager::Run(std::shared_ptr<CopyJob> j) {
  JobRunner r{this, j, {}, {}, false, Resolution::Skip};
  try {
    r.Execute();
  } catch (...) {
    j->AddErrorMsg(L"", L"Nieoczekiwany błąd");
  }
  j->finishedAt = std::chrono::steady_clock::now();
  if (j->cancel) j->state = (int)JobState::Cancelled;
  else if (!j->Errors().empty()) j->state = (int)JobState::DoneErrors;
  else j->state = (int)JobState::Done;
  if (notify_) notify_(j->id);
}

}  // namespace nv

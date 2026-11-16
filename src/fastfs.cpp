#include "fastfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace nv {

std::wstring LongPath(const std::wstring& p) { return p; }

void LowerThreadPriority() { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5); }

static inline i64 FtOf(const struct timespec& ts) { return UnixToFileTime((i64)ts.tv_sec, ts.tv_nsec); }

// ------------------------------------------------------------------ getdents64
struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

unsigned EnumDirRaw(const std::wstring& dir, FsEnumCb cb, void* ctx) {
  std::string p = WToUtf8(dir);
  int fd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return (unsigned)errno ? (unsigned)errno : EACCES;
  struct stat dst;
  u64 parentDev = 0;
  if (fstat(fd, &dst) == 0) parentDev = (u64)dst.st_dev;

  thread_local std::vector<char> buf;
  if (buf.size() < 256 * 1024) buf.resize(256 * 1024);
  thread_local std::wstring wname;

  unsigned result = 0;
  for (;;) {
    long n = syscall(SYS_getdents64, fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      result = (unsigned)errno;
      break;
    }
    if (n == 0) break;
    long off = 0;
    while (off < n) {
      auto* d = (linux_dirent64*)(buf.data() + off);
      off += d->d_reclen;
      const char* nm = d->d_name;
      if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
      struct stat st;
      if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;  // zniknął w trakcie
      FsEntry e;
      e.attrs = 0;
      e.reparseTag = 0;
      e.size = 0;
      bool isLink = S_ISLNK(st.st_mode);
      struct stat tg = st;
      bool tgOk = true;
      if (isLink) {
        e.attrs |= A_REPARSE;
        e.reparseTag = TAG_SYMLINK;
        if (fstatat(fd, nm, &tg, 0) != 0) tgOk = false;  // zerwane dowiązanie: traktuj jak plik
      }
      bool isDir = tgOk && S_ISDIR(tg.st_mode);
      if (isDir) {
        e.attrs |= A_DIR;
        e.size = 0;
        if (!isLink && parentDev && (u64)st.st_dev != parentDev) {  // inny system plików
          e.attrs |= A_REPARSE;
          e.reparseTag = TAG_MOUNT;
        }
      } else {
        e.size = tgOk && S_ISREG(tg.st_mode) ? (u64)tg.st_size : (S_ISREG(st.st_mode) ? (u64)st.st_size : 0);
      }
      if (nm[0] == '.') e.attrs |= A_HIDDEN;
      e.mtime = FtOf(tgOk ? tg.st_mtim : st.st_mtim);
      wname = Utf8ToW(std::string(nm));
      e.name = wname.c_str();
      e.nameLen = (u32)wname.size();
      cb(ctx, e);
    }
  }
  close(fd);
  return result;
}

bool StatPath(const std::wstring& path, FileStat& out) {
  struct stat st;
  std::string p = WToUtf8(path);
  if (lstat(p.c_str(), &st) != 0) {
    out.ok = false;
    return false;
  }
  out = FileStat();
  out.ok = true;
  struct stat tg = st;
  if (S_ISLNK(st.st_mode)) {
    out.attrs |= A_REPARSE;
    if (stat(p.c_str(), &tg) != 0) tg = st;
  }
  if (S_ISDIR(tg.st_mode)) out.attrs |= A_DIR;
  else out.size = S_ISREG(tg.st_mode) ? (u64)tg.st_size : 0;
  if (!(tg.st_mode & 0222)) out.attrs |= A_RO;
  size_t k = path.rfind(L'/');
  if (path.size() > k + 1 && k != std::wstring::npos && path[k + 1] == L'.') out.attrs |= A_HIDDEN;
  out.mtime = FtOf(tg.st_mtim);
  out.ctime = FtOf(tg.st_ctim);
  out.mode = tg.st_mode;
  out.dev = (u64)st.st_dev;
  out.uid = st.st_uid;
  out.gid = st.st_gid;
  return true;
}

u64 VolumeSerial(const std::wstring& path) {
  struct stat st;
  if (stat(WToUtf8(path).c_str(), &st) != 0) return 0;
  return (u64)st.st_dev + 1;  // +1 żeby 0 oznaczało błąd
}

// ------------------------------------------------------------------ typ nośnika
static std::string ReadFirstLine(const std::string& p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return s;
}

// Dla urządzenia blokowego major:minor zwraca ścieżkę sysfs dysku fizycznego (rozwiązuje partycje, dm/md przez "slaves").
static std::string SysDiskDir(unsigned maj, unsigned min, int depth = 0) {
  char link[64];
  snprintf(link, sizeof link, "/sys/dev/block/%u:%u", maj, min);
  char real[PATH_MAX];
  if (!realpath(link, real)) return "";
  std::string d = real;
  struct stat st;
  if (stat((d + "/partition").c_str(), &st) == 0) d = d.substr(0, d.rfind('/'));
  if (depth < 4) {
    std::string sl = d + "/slaves";
    if (DIR* dd = opendir(sl.c_str())) {
      std::string first;
      while (auto* e = readdir(dd))
        if (e->d_name[0] != '.') { first = e->d_name; break; }
      closedir(dd);
      if (!first.empty()) {
        std::string dev = ReadFirstLine(sl + "/" + first + "/dev");
        unsigned a, b;
        if (sscanf(dev.c_str(), "%u:%u", &a, &b) == 2) {
          std::string r = SysDiskDir(a, b, depth + 1);
          if (!r.empty()) return r;
        }
      }
    }
  }
  return d;
}

bool IsNetworkPath(const std::wstring& path) {
  struct statfs sf;
  if (statfs(WToUtf8(path).c_str(), &sf) != 0) return false;
  switch ((unsigned long)sf.f_type) {
    case 0x6969:      // NFS
    case 0x517B:      // SMB
    case 0xFF534D42:  // CIFS
    case 0x65735546:  // FUSE
    case 0x01021997:  // 9p
    case 0x00C36400:  // ceph
      return true;
  }
  return false;
}

bool IsSolidState(const std::wstring& path) {
  struct stat st;
  if (stat(WToUtf8(path).c_str(), &st) != 0) return true;
  unsigned maj = major(st.st_dev), min = minor(st.st_dev);
  if (maj == 0) {  // brak urządzenia blokowego: tmpfs/ramfs => szybkie; sieć i FUSE => nie
    return !IsNetworkPath(path);
  }
  std::string d = SysDiskDir(maj, min);
  if (d.empty()) return true;
  std::string rot = ReadFirstLine(d + "/queue/rotational");
  if (rot.empty()) return true;
  return rot[0] == '0';
}

// ------------------------------------------------------------------ błędy
std::wstring ErrnoText(unsigned e) {
  switch (e) {
    case ENOENT: return L"Nie znaleziono pliku lub folderu";
    case EACCES: return L"Brak dostępu";
    case EPERM: return L"Operacja niedozwolona";
    case EEXIST: return L"Element już istnieje";
    case ENOSPC: return L"Brak wolnego miejsca na dysku";
    case EROFS: return L"System plików jest tylko do odczytu";
    case ENOTDIR: return L"To nie jest folder";
    case EISDIR: return L"To jest folder";
    case ENOTEMPTY: return L"Folder nie jest pusty";
    case EBUSY: return L"Zasób jest zajęty";
    case EXDEV: return L"Różne systemy plików";
    case ENAMETOOLONG: return L"Nazwa jest za długa";
    case EIO: return L"Błąd wejścia/wyjścia";
    case EDQUOT: return L"Przekroczono limit miejsca (quota)";
    case EFBIG: return L"Plik jest za duży dla tego systemu plików";
    case EINVAL: return L"Niepoprawna nazwa lub parametr";
    case ELOOP: return L"Zbyt wiele dowiązań symbolicznych";
    default: break;
  }
  return Utf8ToW(strerror((int)e));
}

// ------------------------------------------------------------------ zamontowane systemy plików
static std::string Unescape(const std::string& s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 3 < s.size() + 0 && isdigit((unsigned char)s[i + 1])) {
      r.push_back((char)strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
      i += 3;
    } else
      r.push_back(s[i]);
  }
  return r;
}

static std::unordered_map<std::string, std::string> LabelsByDevice() {
  std::unordered_map<std::string, std::string> m;
  if (DIR* d = opendir("/dev/disk/by-label")) {
    while (auto* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string link = std::string("/dev/disk/by-label/") + e->d_name;
      char real[PATH_MAX];
      if (realpath(link.c_str(), real)) {
        // nazwy mają escapy \x20
        std::string n = e->d_name, dec;
        for (size_t i = 0; i < n.size(); i++) {
          if (n[i] == '\\' && i + 3 < n.size() + 1 && n[i + 1] == 'x') {
            dec.push_back((char)strtol(n.substr(i + 2, 2).c_str(), nullptr, 16));
            i += 3;
          } else
            dec.push_back(n[i]);
        }
        m[real] = dec;
      }
    }
    closedir(d);
  }
  return m;
}

static bool IsRealFs(const std::string& t) {
  static const std::set<std::string> ok = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "jfs", "reiserfs", "reiser4", "nilfs2", "bcachefs",
                                           "zfs", "ntfs", "ntfs3", "fuseblk", "vfat", "msdos", "exfat", "hfs", "hfsplus", "apfs", "udf",
                                           "iso9660", "9p", "drvfs", "virtiofs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "afs", "ceph", "ufs",
                                           "lustre", "glusterfs", "beegfs", "overlay"};
  if (ok.count(t)) return true;
  if (t.compare(0, 5, "fuse.") == 0) {
    static const std::set<std::string> bad = {"fuse.portal", "fuse.gvfsd-fuse", "fuse.gvfs-fuse-daemon", "fuse.snapfuse", "fuse.lxcfs",
                                              "fuse.xwayland", "fuse.appimage", "fuse.rofiles-fuse"};
    return !bad.count(t);
  }
  return false;
}

static bool HiddenMountPath(const std::string& p) {
  static const char* pre[] = {"/snap", "/var/snap", "/var/lib/docker", "/var/lib/containers", "/var/lib/kubelet", "/var/lib/flatpak",
                              "/boot", "/run/user", "/proc", "/sys", "/dev", "/run/snapd", "/usr/lib/wsl", "/mnt/wslg", "/etc/", "/var/lib/lxc"};
  for (auto* x : pre) {
    size_t n = strlen(x);
    if (p.compare(0, n, x) == 0 && (p.size() == n || p[n] == '/' || x[n - 1] == '/')) return true;
  }
  return false;
}

std::vector<MountEntry> ListMounts(bool withStats) {
  std::vector<MountEntry> out;
  std::ifstream f("/proc/self/mountinfo");
  std::string line;
  auto labels = LabelsByDevice();
  std::set<std::string> seenPaths;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::string id, parent, mm, root, mp, opts, tok;
    is >> id >> parent >> mm >> root >> mp >> opts;
    while (is >> tok && tok != "-") {}
    std::string fs, src, sopts;
    is >> fs >> src >> sopts;
    mp = Unescape(mp);
    if (!IsRealFs(fs)) continue;
    if (fs == "overlay" && mp != "/") continue;
    if (mp != "/" && HiddenMountPath(mp)) continue;
    if (!seenPaths.insert(mp).second) continue;
    MountEntry e;
    e.path = Utf8ToW(mp);
    e.fs = Utf8ToW(fs);
    e.source = Utf8ToW(src);
    unsigned maj = 0, min = 0;
    sscanf(mm.c_str(), "%u:%u", &maj, &min);
    e.dev = makedev(maj, min);
    if (src.compare(0, 5, "/dev/") == 0) {
      char real[PATH_MAX];
      if (realpath(src.c_str(), real)) {
        auto it = labels.find(real);
        if (it != labels.end()) e.label = Utf8ToW(it->second);
      }
    }
    // rodzaj
    static const std::set<std::string> net = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "afs", "ceph", "9p", "drvfs", "virtiofs", "lustre", "glusterfs", "beegfs"};
    if (net.count(fs) || fs == "fuse.sshfs" || fs == "fuse.rclone" || fs == "fuse.davfs" || fs == "fuse.s3fs" || fs == "fuse.curlftpfs") e.kind = MountEntry::Remote;
    else if (fs == "iso9660" || fs == "udf") e.kind = MountEntry::Optical;
    else if (mp.compare(0, 7, "/media/") == 0 || mp.compare(0, 11, "/run/media/") == 0) e.kind = MountEntry::Removable;
    else if (maj != 0) {
      std::string d = SysDiskDir(maj, min);
      if (!d.empty() && ReadFirstLine(d + "/removable") == "1") e.kind = MountEntry::Removable;
    }
    if (withStats) {
      struct statvfs sv;
      if (statvfs(mp.c_str(), &sv) == 0) {
        e.total = (u64)sv.f_blocks * sv.f_frsize;
        e.freeB = (u64)sv.f_bavail * sv.f_frsize;
      }
    }
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const MountEntry& a, const MountEntry& b) {
    if (a.path == L"/" || b.path == L"/") return a.path == L"/" && b.path != L"/";
    return a.path < b.path;
  });
  return out;
}

// ------------------------------------------------------------------ DirWatcher (inotify)
struct DirWatcher::Impl {
  int fd = -1, stopFd = -1;
  std::thread th;
  std::mutex mx;
  std::unordered_map<int, std::string> wd2path;
  std::unordered_map<std::string, int> path2wd;
  uint32_t mask = 0;
  ChangeFn onChange;
  OverflowFn onOverflow;
  std::atomic<bool> limit{false};

  void Loop() {
    std::vector<char> buf(64 * 1024);
    for (;;) {
      pollfd pf[2] = {{fd, POLLIN, 0}, {stopFd, POLLIN, 0}};
      int r = poll(pf, 2, -1);
      if (r < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pf[1].revents) break;
      if (!(pf[0].revents & POLLIN)) continue;
      ssize_t n = read(fd, buf.data(), buf.size());
      if (n <= 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        break;
      }
      ssize_t off = 0;
      while (off < n) {
        auto* ev = (inotify_event*)(buf.data() + off);
        off += (ssize_t)sizeof(inotify_event) + ev->len;
        if (ev->mask & IN_Q_OVERFLOW) {
          if (onOverflow) onOverflow();
          continue;
        }
        std::string dir;
        {
          std::lock_guard<std::mutex> l(mx);
          auto it = wd2path.find(ev->wd);
          if (it == wd2path.end()) continue;
          dir = it->second;
          if (ev->mask & IN_IGNORED) {
            path2wd.erase(dir);
            wd2path.erase(it);
            continue;
          }
        }
        std::string full = dir;
        if (ev->len && ev->name[0]) {
          if (full.empty() || full.back() != '/') full += '/';
          full += ev->name;
        }
        if (onChange) onChange(Utf8ToW(full), ev->mask);
      }
    }
  }
};

DirWatcher::DirWatcher() {}
DirWatcher::~DirWatcher() { Stop(); }

bool DirWatcher::StartMulti(ChangeFn onChange, OverflowFn onOverflow, bool reportModify) {
  Stop();
  auto* im = new Impl();
  im->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  im->stopFd = eventfd(0, EFD_CLOEXEC);
  if (im->fd < 0 || im->stopFd < 0) {
    if (im->fd >= 0) close(im->fd);
    if (im->stopFd >= 0) close(im->stopFd);
    delete im;
    return false;
  }
  im->mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR |
             IN_EXCL_UNLINK;
  if (reportModify) im->mask |= IN_MODIFY;
  im->onChange = std::move(onChange);
  im->onOverflow = std::move(onOverflow);
  p_.reset(im);
  im->th = std::thread([im] { im->Loop(); });
  return true;
}

bool DirWatcher::Start(const std::wstring& dir, bool, ChangeFn onChange, OverflowFn onOverflow) {
  if (!StartMulti(std::move(onChange), std::move(onOverflow), true)) return false;
  if (!AddDir(dir)) {
    Stop();
    return false;
  }
  return true;
}

bool DirWatcher::AddDir(const std::wstring& dir) {
  if (!p_) return false;
  std::string p = WToUtf8(dir);
  int wd = inotify_add_watch(p_->fd, p.c_str(), p_->mask);
  if (wd < 0) {
    if (errno == ENOSPC) p_->limit = true;
    return false;
  }
  std::lock_guard<std::mutex> l(p_->mx);
  auto old = p_->wd2path.find(wd);
  if (old != p_->wd2path.end() && old->second != p) p_->path2wd.erase(old->second);
  p_->wd2path[wd] = p;
  p_->path2wd[p] = wd;
  return true;
}

bool DirWatcher::LimitHit() const { return p_ && p_->limit.load(); }

void DirWatcher::Stop() {
  if (!p_) return;
  uint64_t one = 1;
  if (write(p_->stopFd, &one, sizeof one) < 0) {}
  if (p_->th.joinable()) p_->th.join();
  close(p_->fd);
  close(p_->stopFd);
  p_.reset();
}

}  // namespace nv

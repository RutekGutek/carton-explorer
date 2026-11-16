// Szybki dostęp do systemu plików (Linux): wyliczanie katalogów dużymi porcjami (getdents64),
// obserwator zmian (inotify), lista zamontowanych systemów plików, drobne helpery.
#pragma once
#include <functional>
#include <memory>

#include "util.h"

namespace nv {

struct FsEntry {
  const wchar_t* name;  // wskaźnik ważny tylko w czasie callbacku
  u32 nameLen;
  u32 attrs;
  u64 size;   // rozmiar logiczny (st_size)
  i64 mtime;  // FILETIME (100 ns od 1601) - ta sama reprezentacja co w wersji Windows
  u32 reparseTag;  // TAG_SYMLINK dla dowiązań, TAG_MOUNT dla punktów montowania innego systemu plików
};

typedef void (*FsEnumCb)(void* ctx, const FsEntry& e);

// Zwraca 0 gdy OK, w przeciwnym razie errno.
unsigned EnumDirRaw(const std::wstring& dir, FsEnumCb cb, void* ctx);

template <class F>
unsigned EnumDir(const std::wstring& dir, F&& f) {
  return EnumDirRaw(
      dir, [](void* c, const FsEntry& e) { (*(typename std::remove_reference<F>::type*)c)(e); }, &f);
}

struct FileStat {
  bool ok = false;
  u32 attrs = 0;
  u64 size = 0;
  i64 mtime = 0;
  u32 mode = 0;  // st_mode
  u64 dev = 0;
  u32 uid = 0, gid = 0;
  i64 ctime = 0;
};
// Jak GetFileAttributesEx: dla dowiązania zwraca A_REPARSE (+A_DIR gdy wskazuje na katalog), rozmiar celu.
bool StatPath(const std::wstring& path, FileStat& out);

std::wstring LongPath(const std::wstring& p);  // na Linuksie: bez zmian
bool IsSolidState(const std::wstring& anyPathOnVolume);  // brak "rotational" => SSD/NVMe/RAM
bool IsNetworkPath(const std::wstring& anyPath);         // nfs/cifs/fuse/9p
void LowerThreadPriority();
u64 VolumeSerial(const std::wstring& path);  // st_dev lub 0 przy błędzie
std::wstring ErrnoText(unsigned err);        // tekst błędu po polsku (strerror + tłumaczenia najczęstszych)

// ---- zamontowane systemy plików ("dyski")
struct MountEntry {
  std::wstring path;    // punkt montowania
  std::wstring fs;      // typ systemu plików
  std::wstring source;  // urządzenie
  std::wstring label;   // etykieta (jeśli znana)
  enum Kind { Fixed, Removable, Remote, Optical } kind = Fixed;
  u64 total = 0, freeB = 0;
  u64 dev = 0;
};
std::vector<MountEntry> ListMounts(bool withStats);

// Obserwator zmian (inotify). Tryb pojedynczy: Start(dir) obserwuje jeden katalog (bez podkatalogów).
// Tryb wielokrotny (indeks): StartMulti(), a potem AddDir() dla każdego wylistowanego katalogu.
class DirWatcher {
 public:
  using ChangeFn = std::function<void(const std::wstring& fullPath, u32 mask)>;
  using OverflowFn = std::function<void()>;
  DirWatcher();
  ~DirWatcher();
  bool Start(const std::wstring& dir, bool subtreeIgnored, ChangeFn onChange, OverflowFn onOverflow);
  bool StartMulti(ChangeFn onChange, OverflowFn onOverflow, bool reportModify);
  bool AddDir(const std::wstring& dir);  // false gdy limit inotify wyczerpany lub brak dostępu
  bool LimitHit() const;
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace nv

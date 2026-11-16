// Indeks plików w pamięci: równoległy crawler (priorytetowy dla otwartych folderów), rozmiary folderów
// liczone przyrostowo (w trakcie skanowania widać już sumy częściowe), błyskawiczne wyszukiwanie
// wielowątkowe oraz żywa aktualizacja przez obserwatory zmian.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "fastfs.h"
#include "query.h"
#include "util.h"

namespace nv {

enum : u32 {
  ST_CLAIMED = 1,   // ktoś już wylistował / listuje ten katalog
  ST_LISTED = 2,    // dzieci opublikowane
  ST_SIZED = 4,     // suma całego poddrzewa jest ostateczna
  ST_DENIED = 8,    // brak dostępu
  ST_DELETED = 16,  // usunięty (ukryty w wynikach)
  ST_LEAF = 32,     // link symboliczny / junction - nie wchodzimy
  ST_ROOT = 64,
  ST_NOBG = 128,    // nie skanuj w tle (np. sieć), tylko na żądanie
  ST_PRIO = 256,    // skanowanie na żądanie - potomkowie też priorytetowo
  ST_MOUNT = 512,   // punkt montowania innego systemu plików (liść; własny korzeń tworzony na żądanie)
};

struct Node {
  u32 parent, nameOff, firstChild, childCount;
  std::atomic<u64> size;  // plik: rozmiar; katalog: bieżąca suma poddrzewa
  i64 mtime;
  std::atomic<u32> state;
  u32 attrs;
  std::atomic<u32> files, dirs;  // liczniki rekurencyjne (dla katalogów)
  std::atomic<i32> open;         // ile katalogów w poddrzewie czeka jeszcze na wylistowanie
  u16 nameLen;
  u16 pad;
  bool IsDir() const { return (attrs & A_DIR) != 0; }
};

struct SizeInfo {
  bool known = false;     // węzeł istnieje w indeksie
  bool complete = false;  // suma ostateczna
  u64 size = 0, files = 0, dirs = 0;
};

struct SearchResult {
  std::vector<u32> ids;
  bool truncated = false;
  bool indexComplete = false;
  double ms = 0;
};

struct IndexStatus {
  u64 nodes = 0;
  u64 queued = 0;
  bool bgComplete = false;
  int rootsTotal = 0, rootsDone = 0;
};

class FileIndex {
 public:
  FileIndex();
  ~FileIndex();

  // Roots: punkty montowania skanowane w tle (po starcie). Inne korzenie dodawane są na żądanie.
  void Start(const std::vector<std::wstring>& bgRoots, int workers = 0);
  void Stop();
  void SetNotify(std::function<void()> cb) { notify_ = std::move(cb); }

  // --- nawigacja po indeksie
  u32 Resolve(const std::wstring& path);   // może synchronicznie wylistować katalogi pośrednie
  u32 ResolveNoList(const std::wstring& path) const;  // tylko to co już jest w indeksie
  void EnsureListed(u32 id);
  u32 FindChild(u32 parent, const wchar_t* name, size_t len) const;
  void Children(u32 id, std::vector<u32>& out) const;
  void GetInfo(u32 id, SizeInfo& out) const;
  void Prioritize(u32 id);  // przesuń skanowanie poddrzewa na początek kolejki

  // --- odczyt
  u32 Count() const { return count_.load(std::memory_order_acquire); }
  const Node& N(u32 id) const { return pages_[id >> PSHIFT][id & PMASK]; }
  const wchar_t* Name(const Node& n) const { return npages_[n.nameOff >> NSHIFT] + (n.nameOff & NMASK); }
  std::wstring Path(u32 id) const;
  bool IsUnder(u32 id, u32 ancestor) const;
  IndexStatus GetStatus() const;

  // --- wyszukiwanie
  SearchResult Search(const Query& q, u32 scope, size_t cap, const std::atomic<u64>* cancelGen, u64 myGen) const;

  // --- aktualizacje
  void MarkDirty(const std::wstring& dirPath);  // katalog zmienił się - przeskanuj go ponownie
  void SetWatching(bool on) { watching_ = on; }
  bool WatchLimited() const { return watchLimit_ || !watchStarted_.load(); }  // brak inotify albo wyczerpany limit watchy

 private:
  static constexpr u32 PSHIFT = 16, PSIZE = 1u << PSHIFT, PMASK = PSIZE - 1, MAXPAGES = 1u << 16;
  static constexpr u32 NSHIFT = 20, NSIZE = 1u << NSHIFT, NMASK = NSIZE - 1, MAXNPAGES = 1u << 12;

  Node& NM(u32 id) { return pages_[id >> PSHIFT][id & PMASK]; }
  void EnsurePagesLocked(u32 upto);
  u32 AllocNameLocked(const wchar_t* s, size_t len);
  u32 AddRoot(const std::wstring& name, bool bg);
  u32 FindRoot(const std::wstring& name) const;
  u32 BestRoot(const std::wstring& path, size_t& consumed) const;  // najdłuższy korzeń będący prefiksem ścieżki

  void ListDir(u32 id, bool inheritPrio);
  void AddDelta(u32 from, i64 bytes, i64 files, i64 dirs, i32 open);
  void Enqueue(u32 id, bool prio);
  void Worker();
  void Bump(u32 id);
  void MaybeNotify(bool force = false);
  void RescanLoop();
  void RescanDir(u32 id);
  void RemoveSubtree(u32 id, u32 parent);
  void MarkDeletedRec(u32 id);
  u32 AppendChild(u32 parent, const wchar_t* name, size_t len, u32 attrs, u64 size, i64 mtime, u32 tag);

  std::unique_ptr<Node*[]> pages_;
  std::unique_ptr<wchar_t*[]> npages_;
  std::atomic<u32> count_{0};
  u32 nextId_ = 0;  // pod wmx_
  u32 nameCur_ = 0;
  mutable std::mutex wmx_;

  std::vector<u32> roots_;
  mutable std::mutex rmx_;
  mutable std::mutex emx_;
  std::atomic<bool> hasExtras_{false};
  std::unordered_map<u32, std::vector<u32>> extras_;  // dzieci dodane po wylistowaniu (pod emx_)

  // kolejka zadań
  std::mutex qmx_;
  std::condition_variable qcv_;
  std::vector<u32> prioQ_;
  std::deque<u32> bgQ_;
  std::atomic<int> busy_{0};
  std::atomic<u64> queued_{0};
  std::atomic<bool> stop_{false};
  std::vector<std::thread> workers_;

  std::function<void()> notify_;
  std::atomic<i64> lastNotify_{0};

  // przeskanowania
  std::mutex dmx_;
  std::condition_variable dcv_;
  std::unordered_set<std::wstring> dirty_;
  std::thread rescanTh_;
  bool watching_ = true;
  bool watchLimit_ = false;

  DirWatcher watcher_;  // jeden inotify dla całego indeksu; katalogi dodawane przy listowaniu
  std::atomic<bool> watchStarted_{false};
};

}  // namespace nv

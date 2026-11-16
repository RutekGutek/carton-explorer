#include "index.h"

#include <algorithm>

namespace nv {

static i64 NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

FileIndex::FileIndex() {
  pages_.reset(new Node*[MAXPAGES]());
  npages_.reset(new wchar_t*[MAXNPAGES]());
}

FileIndex::~FileIndex() {
  Stop();
  for (u32 i = 0; i < MAXPAGES; i++) delete[] pages_[i];
  for (u32 i = 0; i < MAXNPAGES; i++) delete[] npages_[i];
}

// ------------------------------------------------------------------ pamięć
void FileIndex::EnsurePagesLocked(u32 upto) {
  u32 lastPage = upto ? (upto - 1) >> PSHIFT : 0;
  for (u32 p = 0; p <= lastPage; p++)
    if (!pages_[p]) pages_[p] = new Node[PSIZE]();
}

u32 FileIndex::AllocNameLocked(const wchar_t* s, size_t len) {
  if ((nameCur_ & NMASK) + len + 1 > NSIZE) nameCur_ = (nameCur_ | NMASK) + 1;
  u32 page = nameCur_ >> NSHIFT;
  if (!npages_[page]) npages_[page] = new wchar_t[NSIZE];
  u32 off = nameCur_;
  memcpy(npages_[page] + (off & NMASK), s, len * sizeof(wchar_t));
  nameCur_ += (u32)len;
  return off;
}

// ------------------------------------------------------------------ korzenie
u32 FileIndex::AddRoot(const std::wstring& name, bool bg) {
  std::lock_guard<std::mutex> rl(rmx_);
  for (u32 r : roots_) {
    const Node& n = N(r);
    if (n.nameLen == name.size() && wmemcmp(Name(n), name.data(), n.nameLen) == 0) return r;
  }
  u32 id;
  {
    std::lock_guard<std::mutex> wl(wmx_);
    id = nextId_++;
    EnsurePagesLocked(id + 1);
    Node& n = NM(id);
    n.parent = NONE;
    n.nameOff = AllocNameLocked(name.data(), name.size());
    n.nameLen = (u16)name.size();
    n.attrs = A_DIR;
    n.mtime = 0;
    n.size = 0;
    n.files = 0;
    n.dirs = 0;
    n.open = 1;
    n.firstChild = 0;
    n.childCount = 0;
    n.state = (u32)ST_ROOT | (bg ? 0u : (u32)ST_NOBG);
    count_.store(nextId_, std::memory_order_release);
  }
  roots_.push_back(id);
  if (bg) Enqueue(id, false);
  return id;
}

u32 FileIndex::FindRoot(const std::wstring& name) const {
  std::lock_guard<std::mutex> rl(rmx_);
  for (u32 r : roots_) {
    const Node& n = N(r);
    if (n.nameLen == name.size() && wmemcmp(Name(n), name.data(), n.nameLen) == 0) return r;
  }
  return NONE;
}

u32 FileIndex::BestRoot(const std::wstring& path, size_t& consumed) const {
  std::lock_guard<std::mutex> rl(rmx_);
  u32 best = NONE;
  size_t bl = 0;
  for (u32 r : roots_) {
    const Node& n = N(r);
    size_t len = n.nameLen;
    if (len > path.size() || len < bl || wmemcmp(Name(n), path.data(), len) != 0) continue;
    const wchar_t* nm = Name(n);
    if (len == path.size() || nm[len - 1] == L'/' || path[len] == L'/') {
      best = r;
      bl = len;
    }
  }
  consumed = bl;
  return best;
}

// ------------------------------------------------------------------ kolejka / wątki
void FileIndex::Enqueue(u32 id, bool prio) {
  {
    std::lock_guard<std::mutex> l(qmx_);
    if (prio) prioQ_.push_back(id);
    else bgQ_.push_back(id);
    queued_++;
  }
  qcv_.notify_one();
}

void FileIndex::MaybeNotify(bool force) {
  if (!notify_) return;
  i64 now = NowMs(), last = lastNotify_.load();
  if (!force && now - last < 120) return;
  if (lastNotify_.compare_exchange_strong(last, now) || force) notify_();
}

void FileIndex::Worker() {
  LowerThreadPriority();
  for (;;) {
    u32 id;
    bool prio = false;
    {
      std::unique_lock<std::mutex> lk(qmx_);
      qcv_.wait(lk, [&] { return stop_.load() || !prioQ_.empty() || !bgQ_.empty(); });
      if (stop_) return;
      if (!prioQ_.empty()) {
        id = prioQ_.back();
        prioQ_.pop_back();
        prio = true;
      } else {
        id = bgQ_.front();
        bgQ_.pop_front();
      }
      busy_++;
    }
    ListDir(id, prio);
    queued_--;
    busy_--;
    if (queued_.load() == 0 && busy_.load() == 0) MaybeNotify(true);
    else MaybeNotify();
  }
}

void FileIndex::Start(const std::vector<std::wstring>& bgRoots, int workers) {
  stop_ = false;
  int nt = workers > 0 ? workers : (int)std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
  if (watching_) {
    std::vector<std::wstring> rootsCopy = bgRoots;
    bool ok = watcher_.StartMulti(
        [this](const std::wstring& full, u32 mask) {
          (void)mask;
          MarkDirty(PathParent(full));
        },
        [this, rootsCopy] {
          for (auto& r : rootsCopy) MarkDirty(PathNormalize(r));
        },
        false);
    watchStarted_ = ok;
  }
  for (int i = 0; i < nt; i++) workers_.emplace_back([this] { Worker(); });
  rescanTh_ = std::thread([this] { RescanLoop(); });
  for (auto& r : bgRoots) {
    std::wstring p = PathNormalize(r);
    if (!RootLen(p)) continue;
    AddRoot(p, true);
  }
}

void FileIndex::Stop() {
  if (stop_.exchange(true)) {
    if (workers_.empty() && !rescanTh_.joinable()) return;
  }
  qcv_.notify_all();
  dcv_.notify_all();
  for (auto& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
  if (rescanTh_.joinable()) rescanTh_.join();
  watcher_.Stop();
  watchStarted_ = false;
}

// ------------------------------------------------------------------ listowanie katalogu
struct TmpEnt {
  u32 nameOff;
  u16 nameLen;
  u32 attrs;
  u64 size;
  i64 mtime;
  u32 tag;
};

static bool IsLeafDir(u32 attrs, u32 tag) {
  return (attrs & A_DIR) && (attrs & A_REPARSE) && (tag == TAG_SYMLINK || tag == TAG_MOUNT);
}

void FileIndex::AddDelta(u32 from, i64 bytes, i64 files, i64 dirs, i32 open) {
  for (u32 a = from; a != NONE; a = N(a).parent) {
    Node& A = NM(a);
    if (A.state.load(std::memory_order_relaxed) & ST_DELETED) break;
    if (bytes) A.size.fetch_add((u64)bytes);
    if (files) A.files.fetch_add((u32)files);
    if (dirs) A.dirs.fetch_add((u32)dirs);
    if (open) {
      i32 nw = A.open.fetch_add(open) + open;
      if (nw <= 0) A.state.fetch_or(ST_SIZED);
      else if (open > 0) A.state.fetch_and(~ST_SIZED);
    }
  }
}

void FileIndex::ListDir(u32 id, bool inheritPrio) {
  Node& n = NM(id);
  if (n.state.load() & ST_DELETED) return;
  if (n.state.fetch_or(ST_CLAIMED) & ST_CLAIMED) return;
  bool prio = inheritPrio || (n.state.load() & ST_PRIO);

  std::wstring path = Path(id);
  if (watchStarted_.load() && !watchLimit_) {
    if (!watcher_.AddDir(path) && watcher_.LimitHit()) watchLimit_ = true;  // najpierw watch, potem listowanie: brak okna na utracone zdarzenia
  }
  thread_local std::vector<TmpEnt> ents;
  thread_local std::vector<wchar_t> nm;
  ents.clear();
  nm.clear();
  unsigned err = EnumDir(path, [&](const FsEntry& e) {
    if (e.nameLen == 1 && e.name[0] == L'.') return;
    if (e.nameLen == 2 && e.name[0] == L'.' && e.name[1] == L'.') return;
    TmpEnt t{(u32)nm.size(), (u16)e.nameLen, e.attrs, e.size, e.mtime, e.reparseTag};
    nm.insert(nm.end(), e.name, e.name + e.nameLen);
    ents.push_back(t);
  });
  if (err) n.state.fetch_or(ST_DENIED);

  u64 bytes = 0;
  u32 nf = 0, nd = 0, crawl = 0;
  for (auto& t : ents) {
    if (t.attrs & A_DIR) {
      nd++;
      if (!IsLeafDir(t.attrs, t.tag)) crawl++;
    } else {
      nf++;
      bytes += t.size;
    }
  }
  // 1) sumy do łańcucha przodków - zanim dzieci staną się widoczne
  AddDelta(id, (i64)bytes, nf, nd, (i32)crawl - 1);

  // 2) publikacja dzieci
  u32 first;
  u32 parentNoBg = n.state.load() & ST_NOBG;
  {
    std::lock_guard<std::mutex> wl(wmx_);
    first = nextId_;
    EnsurePagesLocked(first + (u32)ents.size());
    for (size_t i = 0; i < ents.size(); i++) {
      const TmpEnt& t = ents[i];
      Node& c = NM(first + (u32)i);
      c.parent = id;
      c.nameOff = AllocNameLocked(&nm[t.nameOff], t.nameLen);
      c.nameLen = t.nameLen;
      c.attrs = t.attrs;
      c.mtime = t.mtime;
      c.firstChild = 0;
      c.childCount = 0;
      c.files = 0;
      c.dirs = 0;
      if (t.attrs & A_DIR) {
        c.size = 0;
        if (IsLeafDir(t.attrs, t.tag)) {
          c.open = 0;
          c.state = ST_CLAIMED | ST_LISTED | ST_SIZED | ST_LEAF | (t.tag == TAG_MOUNT ? (u32)ST_MOUNT : 0u);
        } else {
          c.open = 1;
          c.state = parentNoBg | (prio ? (u32)ST_PRIO : 0u);
        }
      } else {
        c.size = t.size;
        c.open = 0;
        c.state = 0;
      }
    }
    n.firstChild = first;
    n.childCount = (u32)ents.size();
    nextId_ += (u32)ents.size();
    count_.store(nextId_, std::memory_order_release);
  }
  n.state.fetch_or(ST_LISTED);

  // 3) dalsze skanowanie
  if (!(parentNoBg && !prio)) {
    for (size_t i = 0; i < ents.size(); i++)
      if ((ents[i].attrs & A_DIR) && !IsLeafDir(ents[i].attrs, ents[i].tag)) Enqueue(first + (u32)i, prio);
  }
}

void FileIndex::EnsureListed(u32 id) {
  Node& n = NM(id);
  if (n.state.load() & ST_LISTED) return;
  ListDir(id, false);
  while (!(n.state.load() & ST_LISTED)) {
    if (stop_) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ------------------------------------------------------------------ nawigacja
u32 FileIndex::FindChild(u32 parent, const wchar_t* name, size_t len) const {
  const Node& p = N(parent);
  u32 first = p.firstChild, cnt = p.childCount;
  for (u32 i = 0; i < cnt; i++) {
    const Node& c = N(first + i);
    if (c.nameLen != len) continue;
    if (c.state.load(std::memory_order_relaxed) & ST_DELETED) continue;
    if (wmemcmp(Name(c), name, len) == 0) return first + i;
  }
  if (hasExtras_.load()) {
    std::lock_guard<std::mutex> l(emx_);
    auto it = extras_.find(parent);
    if (it != extras_.end())
      for (u32 id : it->second) {
        const Node& c = N(id);
        if (c.nameLen != len || (c.state.load() & ST_DELETED)) continue;
        if (wmemcmp(Name(c), name, len) == 0) return id;
      }
  }
  return NONE;
}

void FileIndex::Children(u32 id, std::vector<u32>& out) const {
  out.clear();
  const Node& p = N(id);
  for (u32 i = 0; i < p.childCount; i++)
    if (!(N(p.firstChild + i).state.load(std::memory_order_relaxed) & ST_DELETED)) out.push_back(p.firstChild + i);
  if (hasExtras_.load()) {
    std::lock_guard<std::mutex> l(emx_);
    auto it = extras_.find(id);
    if (it != extras_.end())
      for (u32 c : it->second)
        if (!(N(c).state.load() & ST_DELETED)) out.push_back(c);
  }
}

u32 FileIndex::Resolve(const std::wstring& pathIn) {
  std::wstring path = PathNormalize(pathIn);
  if (!RootLen(path)) return NONE;
  size_t used = 0;
  u32 cur = BestRoot(path, used);
  if (cur == NONE) {
    cur = AddRoot(L"/", false);
    used = 1;
  }
  std::wstring so = path.substr(0, used);
  if (so.size() > 1 && so.back() == L'/') so.pop_back();
  for (auto& c : PathSplit(path.substr(used))) {
    EnsureListed(cur);
    u32 ch = FindChild(cur, c.data(), c.size());
    if (ch == NONE) return NONE;
    so = PathJoin(so, c);
    cur = ch;
    if (N(cur).state.load() & ST_MOUNT) cur = AddRoot(so, false);  // inny system plików: własny korzeń na żądanie
  }
  return cur;
}

u32 FileIndex::ResolveNoList(const std::wstring& pathIn) const {
  std::wstring path = PathNormalize(pathIn);
  if (!RootLen(path)) return NONE;
  size_t used = 0;
  u32 cur = BestRoot(path, used);
  if (cur == NONE) return NONE;
  std::wstring so = path.substr(0, used);
  if (so.size() > 1 && so.back() == L'/') so.pop_back();
  for (auto& c : PathSplit(path.substr(used))) {
    if (!(N(cur).state.load() & ST_LISTED)) return NONE;
    u32 ch = FindChild(cur, c.data(), c.size());
    if (ch == NONE) return NONE;
    so = PathJoin(so, c);
    cur = ch;
    if (N(cur).state.load() & ST_MOUNT) {
      cur = FindRoot(so);
      if (cur == NONE) return NONE;
    }
  }
  return cur;
}

void FileIndex::GetInfo(u32 id, SizeInfo& out) const {
  const Node& n = N(id);
  out.known = true;
  out.size = n.size.load();
  out.files = n.files.load();
  out.dirs = n.dirs.load();
  out.complete = !n.IsDir() || (n.state.load() & ST_SIZED);
}

void FileIndex::Prioritize(u32 id) {
  std::vector<u32> stack{id}, toQ, ch;
  while (!stack.empty()) {
    u32 x = stack.back();
    stack.pop_back();
    Node& n = NM(x);
    u32 st = n.state.load();
    if (!n.IsDir() || (st & (ST_DELETED | ST_SIZED | ST_LEAF))) continue;
    n.state.fetch_or(ST_PRIO);
    if (!(st & ST_LISTED)) {
      if (!(st & ST_CLAIMED)) toQ.push_back(x);
      continue;
    }
    Children(x, ch);
    for (u32 c : ch)
      if (N(c).IsDir()) stack.push_back(c);
  }
  if (toQ.empty()) return;
  {
    std::lock_guard<std::mutex> l(qmx_);
    for (auto it = toQ.rbegin(); it != toQ.rend(); ++it) {
      prioQ_.push_back(*it);
      queued_++;
    }
  }
  qcv_.notify_all();
}

std::wstring FileIndex::Path(u32 id) const {
  thread_local std::vector<u32> chain;
  chain.clear();
  for (u32 c = id; c != NONE; c = N(c).parent) chain.push_back(c);
  std::wstring r;
  r.reserve(64);
  for (size_t i = chain.size(); i-- > 0;) {
    const Node& n = N(chain[i]);
    if (i + 1 != chain.size() && (r.empty() || r.back() != L'/')) r.push_back(L'/');
    r.append(Name(n), n.nameLen);
  }
  return r;
}

bool FileIndex::IsUnder(u32 id, u32 anc) const {
  for (u32 p = N(id).parent; p != NONE; p = N(p).parent)
    if (p == anc) return true;
  return false;
}

IndexStatus FileIndex::GetStatus() const {
  IndexStatus s;
  s.nodes = Count();
  s.queued = queued_.load();
  s.bgComplete = queued_.load() == 0 && busy_.load() == 0;
  std::lock_guard<std::mutex> l(rmx_);
  for (u32 r : roots_) {
    const Node& n = N(r);
    if (n.state.load() & ST_NOBG) continue;
    s.rootsTotal++;
    if (n.state.load() & ST_SIZED) s.rootsDone++;
  }
  return s;
}

// ------------------------------------------------------------------ wyszukiwanie
struct PathCtx {
  const FileIndex* ix;
  u32 id;
  std::wstring s;
  bool built;
};
static const std::wstring& PathCb(void* c) {
  auto* p = (PathCtx*)c;
  if (!p->built) {
    p->s = p->ix->Path(p->id);
    p->built = true;
  }
  return p->s;
}

SearchResult FileIndex::Search(const Query& q, u32 scope, size_t cap, const std::atomic<u64>* cancel, u64 myGen) const {
  auto t0 = std::chrono::steady_clock::now();
  SearchResult res;
  res.indexComplete = GetStatus().bgComplete;

  // lista kandydatów: cały indeks albo poddrzewo zakresu
  std::vector<u32> cand;
  u32 n = Count();
  bool useCand = scope != NONE;
  if (useCand) {
    std::vector<u32> stack{scope}, ch;
    while (!stack.empty()) {
      u32 x = stack.back();
      stack.pop_back();
      Children(x, ch);
      for (u32 c : ch) {
        cand.push_back(c);
        if (N(c).IsDir()) stack.push_back(c);
      }
    }
    std::sort(cand.begin(), cand.end());
    n = (u32)cand.size();
  }

  unsigned T = n < 30000 ? 1 : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  std::vector<std::vector<u32>> part(T);
  std::atomic<bool> over{false};

  auto work = [&](unsigned ti) {
    u32 lo = (u32)((u64)n * ti / T), hi = (u32)((u64)n * (ti + 1) / T);
    auto& out = part[ti];
    PathCtx pc{this, 0, {}, false};
    for (u32 i = lo; i < hi; i++) {
      if (((i - lo) & 2047) == 0) {
        if (cancel && cancel->load(std::memory_order_relaxed) != myGen) return;
        if (over.load(std::memory_order_relaxed)) return;
      }
      u32 id = useCand ? cand[i] : i;
      const Node& nd = N(id);
      if (nd.state.load(std::memory_order_relaxed) & (ST_DELETED | ST_ROOT)) continue;
      pc.built = false;
      pc.id = id;
      if (!q.Match(Name(nd), nd.nameLen, nd.IsDir(), nd.size.load(std::memory_order_relaxed), nd.mtime, PathCb, &pc)) continue;
      out.push_back(id);
      if (out.size() >= cap) {
        over = true;
        return;
      }
    }
  };

  if (T == 1) work(0);
  else {
    std::vector<std::thread> th;
    for (unsigned t = 1; t < T; t++) th.emplace_back(work, t);
    work(0);
    for (auto& t : th) t.join();
  }
  for (auto& p : part) {
    if (res.ids.size() + p.size() > cap) {
      p.resize(cap - res.ids.size());
      res.truncated = true;
    }
    res.ids.insert(res.ids.end(), p.begin(), p.end());
  }
  if (over) res.truncated = true;
  res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

// ------------------------------------------------------------------ żywe aktualizacje
void FileIndex::MarkDirty(const std::wstring& dirPath) {
  if (dirPath.empty()) return;
  {
    std::lock_guard<std::mutex> l(dmx_);
    dirty_.insert(PathNormalize(dirPath));
  }
  dcv_.notify_one();
}

void FileIndex::RescanLoop() {
  for (;;) {
    std::unordered_set<std::wstring> batch;
    {
      std::unique_lock<std::mutex> lk(dmx_);
      dcv_.wait(lk, [&] { return stop_.load() || !dirty_.empty(); });
      if (stop_) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));  // zbieraj zdarzenia w paczki
    {
      std::lock_guard<std::mutex> l(dmx_);
      batch.swap(dirty_);
    }
    for (auto& p : batch) {
      if (stop_) return;
      u32 id = ResolveNoList(p);
      if (id != NONE) RescanDir(id);
    }
    MaybeNotify(true);
  }
}

u32 FileIndex::AppendChild(u32 parent, const wchar_t* name, size_t len, u32 attrs, u64 size, i64 mtime, u32 tag) {
  u32 id;
  bool leaf = IsLeafDir(attrs, tag);
  {
    std::lock_guard<std::mutex> wl(wmx_);
    id = nextId_++;
    EnsurePagesLocked(id + 1);
    Node& c = NM(id);
    c.parent = parent;
    c.nameOff = AllocNameLocked(name, len);
    c.nameLen = (u16)len;
    c.attrs = attrs;
    c.mtime = mtime;
    c.firstChild = 0;
    c.childCount = 0;
    c.files = 0;
    c.dirs = 0;
    if (attrs & A_DIR) {
      c.size = 0;
      c.open = leaf ? 0 : 1;
      c.state = leaf ? (ST_CLAIMED | ST_LISTED | ST_SIZED | ST_LEAF | (tag == TAG_MOUNT ? (u32)ST_MOUNT : 0u)) : (u32)ST_PRIO;
    } else {
      c.size = size;
      c.open = 0;
      c.state = 0;
    }
    count_.store(nextId_, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> l(emx_);
    extras_[parent].push_back(id);
    hasExtras_ = true;
  }
  return id;
}

void FileIndex::MarkDeletedRec(u32 id) {
  std::vector<u32> stack{id}, ch;
  while (!stack.empty()) {
    u32 x = stack.back();
    stack.pop_back();
    NM(x).state.fetch_or(ST_DELETED);
    Children(x, ch);
    for (u32 c : ch) stack.push_back(c);
    if (hasExtras_) {
      std::lock_guard<std::mutex> l(emx_);
      extras_.erase(x);
    }
  }
}

void FileIndex::RemoveSubtree(u32 id, u32 parent) {
  Node& r = NM(id);
  if (r.state.load() & ST_DELETED) return;
  i64 bytes = (i64)r.size.load();
  bool isDir = r.IsDir();
  i64 files = isDir ? (i64)r.files.load() : 1;
  i64 dirs = isDir ? (i64)r.dirs.load() + 1 : 0;
  i32 open = isDir ? r.open.load() : 0;
  MarkDeletedRec(id);
  AddDelta(parent, -bytes, -files, -dirs, -open);
}

void FileIndex::RescanDir(u32 id) {
  Node& d = NM(id);
  u32 st = d.state.load();
  if ((st & ST_DELETED) || !(st & ST_LISTED) || (st & ST_LEAF)) return;
  std::wstring path = Path(id);

  struct T {
    std::wstring name;
    u32 attrs;
    u64 size;
    i64 mtime;
    u32 tag;
  };
  std::vector<T> fresh;
  unsigned err = EnumDir(path, [&](const FsEntry& e) {
    if (e.nameLen == 1 && e.name[0] == L'.') return;
    if (e.nameLen == 2 && e.name[0] == L'.' && e.name[1] == L'.') return;
    fresh.push_back({std::wstring(e.name, e.nameLen), e.attrs, e.size, e.mtime, e.reparseTag});
  });
  if (err) {
    FileStat fs;
    if (!StatPath(path, fs) && d.parent != NONE) RemoveSubtree(id, d.parent);
    return;
  }

  std::vector<u32> ex;
  Children(id, ex);
  std::unordered_map<std::wstring, u32> map;
  map.reserve(ex.size() * 2 + 1);
  for (u32 c : ex) map.emplace(std::wstring(Name(N(c)), N(c).nameLen), c);
  std::unordered_set<u32> seen;

  for (auto& t : fresh) {
    bool tIsDir = (t.attrs & A_DIR) != 0;
    auto it = map.find(t.name);
    if (it != map.end()) {
      Node& c = NM(it->second);
      seen.insert(it->second);
      if (c.IsDir() == tIsDir) {
        if (!tIsDir) {
          i64 delta = (i64)t.size - (i64)c.size.load();
          if (delta) {
            c.size.store(t.size);
            AddDelta(id, delta, 0, 0, 0);
          }
        }
        c.mtime = t.mtime;
        c.attrs = t.attrs;
        continue;
      }
      RemoveSubtree(it->second, id);  // zmienił się typ
    }
    u32 nid = AppendChild(id, t.name.data(), t.name.size(), t.attrs, t.size, t.mtime, t.tag);
    if (tIsDir) {
      bool leaf = IsLeafDir(t.attrs, t.tag);
      AddDelta(id, 0, 0, 1, leaf ? 0 : 1);
      if (!leaf) Enqueue(nid, true);
    } else {
      AddDelta(id, (i64)t.size, 1, 0, 0);
    }
  }
  for (u32 c : ex)
    if (!seen.count(c)) RemoveSubtree(c, id);
}

}  // namespace nv

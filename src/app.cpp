#include "app.h"
#include <QAbstractItemView>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QCloseEvent>
#include <QDir>
#include <QGuiApplication>
#include <QPainterPath>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace nv {

double NowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const wchar_t* kUnset = L"\x1";

static QString Q(const std::wstring& s) { return QString::fromStdWString(s); }
static std::wstring W(const QString& s) { return Utf8ToW(s.toUtf8().toStdString()); }
static std::wstring FromQPath(const QString& s) { return Utf8ToW(s.toUtf8().toStdString()); }

App::App() {}
App::~App() {}

void App::Post(std::function<void()> f) {
  if (closing_) return;
  QMetaObject::invokeMethod(this, [this, f]() { if (!closing_) f(); }, Qt::QueuedConnection);
}

// ------------------------------------------------------------------ ikona aplikacji
static QIcon MakeAppIcon() {
  QIcon ic;
  for (int size : {16, 24, 32, 48, 64, 128, 256}) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    float s = (float)size;
    QLinearGradient g(0, 0, s, s);
    g.setColorAt(0, th::accent);
    g.setColorAt(1, th::accent2);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(QRectF(0, 0, s, s), s * 0.24f, s * 0.24f);
    float u = s / 32.f;
    p.setBrush(QColor(255, 255, 255, 242));
    p.drawRoundedRect(QRectF(6 * u, 9 * u, 9 * u, 5 * u), 1.5f * u, 1.5f * u);
    p.setBrush(QColor(255, 255, 255, 230));
    p.drawRoundedRect(QRectF(6 * u, 12 * u, 20 * u, 12 * u), 2.5f * u, 2.5f * u);
    ic.addPixmap(pm);
  }
  return ic;
}

// ------------------------------------------------------------------ okno
bool App::Create(const std::wstring& startPath) {
  cmdPath_ = startPath;
  LoadSettings();
  if (!cmdPath_.empty()) startPath_ = cmdPath_;
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground, true);
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  setMouseTracking(true);
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(820, 520);
  resize(1360, 840);
  setWindowIcon(MakeAppIcon());
  setWindowTitle("Carton Explorer");
  if (QScreen* sc = QGuiApplication::primaryScreen()) {
    QRect a = sc->availableGeometry();
    int w = std::min(1360, a.width() - 40), h = std::min(840, a.height() - 40);
    resize(w, h);
    move(a.center() - QPoint(w / 2, h / 2));
  }
  W_ = (float)width();
  H_ = (float)height();
  InitUI();
  show();
  return true;
}

void App::InitUI() {
  auto edit = [&](bool alt) {
    auto* e = new QLineEdit(this);
    e->setFrame(false);
    e->setStyleSheet(QString("QLineEdit{background:%1;border:none;color:#ececf8;selection-background-color:#6a5df0;selection-color:#ffffff;font-family:'%2';font-size:14px;%3}")
                         .arg(alt ? "#2e2e5a" : "transparent", g_.FamilyName(), alt ? "border-radius:6px;padding:0 6px;" : ""));
    e->setAttribute(Qt::WA_TranslucentBackground, !alt);
    e->installEventFilter(this);
    return e;
  };
  hSearch_ = edit(false);
  hSearch_->setPlaceholderText("Szukaj (np. *.txt, ext:png, size:>10mb)");
  hAddr_ = edit(false);
  hAddr_->hide();
  hRename_ = edit(true);
  hRename_->hide();
  connect(hSearch_, &QLineEdit::textChanged, this, [this] { OnSearchTextChanged(); });

  auto* fsm = new QFileSystemModel(this);
  fsm->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Hidden);
  fsm->setRootPath("/");
  auto* comp = new QCompleter(fsm, this);
  comp->setCompletionMode(QCompleter::PopupCompletion);
  comp->setCaseSensitivity(Qt::CaseInsensitive);
  comp->popup()->setStyleSheet(
      "QListView{background:#1c1c36;border:1px solid #2a2a4a;color:#ececf8;font-size:13px;padding:4px;outline:0;}"
      "QListView::item{padding:5px 8px;border-radius:6px;}QListView::item:selected{background:#35306d;}");
  hAddr_->setCompleter(comp);

  connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
    clipHasFiles_ = ClipboardHasFiles();
    if (!ownClip_ && !cutSet_.empty()) cutSet_.clear();
    ownClip_ = false;
    update();
  });
  clipHasFiles_ = ClipboardHasFiles();

  jobs_.SetNotify([this](u32 id) { Post([this, id] { OnJobEvent(id); }); });

  sth_ = std::thread([this] { SearchThread(); });
  RefreshDrivesAsync();
  BuildSidebar();
  StartIndex();
  Layout();
  PositionEdits();

  // zmiany montowań (pendrive'y itp.)
  mountFd_ = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
  if (mountFd_ >= 0) {
    auto* sn = new QSocketNotifier(mountFd_, QSocketNotifier::Exception, this);
    connect(sn, &QSocketNotifier::activated, this, [this] { RefreshDrivesAsync(); });
  }

  connect(&timer_, &QTimer::timeout, this, [this] { OnTimer(); });
  timer_.start(33);
  dirTimer_.setSingleShot(true);
  connect(&dirTimer_, &QTimer::timeout, this, [this] {
    if (!searchActive_ && !thisPC_ && renameIdx_ < 0 && !curPath_.empty()) Reload(true);
  });

  tabs_.clear();
  NewTab(startPath_);
}

void App::Shutdown() {
  if (closing_) return;
  closing_ = true;
  timer_.stop();
  SaveSettings();
  watcher_.Stop();
  {
    std::lock_guard<std::mutex> l(smx_);
    sStop_ = true;
  }
  scv_.notify_all();
  {
    std::lock_guard<std::mutex> l(cmx_);
    for (auto* r : creqs_) {
      std::lock_guard<std::mutex> l2(r->m);
      r->res = Resolution::Cancel;
      r->done = true;
      r->cv.notify_all();
    }
    creqs_.clear();
  }
  jobs_.CancelAll();
  for (int i = 0; i < 400 && loaders_.load() > 0; i++) usleep(5000);
  if (sth_.joinable()) sth_.join();
  idx_.Stop();
  SetLauncherProgress(false, 0);
  if (mountFd_ >= 0) ::close(mountFd_);
}

void App::closeEvent(QCloseEvent* e) {
  if (jobs_.AnyActive() && !closing_) {
    Modal md;
    md.title = L"Trwają operacje na plikach";
    md.lines = {L"Zamknięcie programu przerwie kopiowanie/przenoszenie."};
    md.btns = {L"Zamknij mimo to", L"Wróć"};
    md.def = 1;
    md.cancelBtn = 1;
    md.danger = true;
    md.cb = [this](int b, bool) {
      if (b == 0) {
        jobs_.CancelAll();
        Shutdown();
        QApplication::quit();
      }
    };
    ShowModal(md);
    e->ignore();
    return;
  }
  Shutdown();
  e->accept();
  QApplication::quit();
}

void App::changeEvent(QEvent* e) {
  if (e->type() == QEvent::WindowStateChange) {
    maximized_ = isMaximized();
    update();
  }
  QWidget::changeEvent(e);
}

void App::resizeEvent(QResizeEvent*) {
  W_ = (float)width();
  H_ = (float)height();
  maximized_ = isMaximized();
  Layout();
  PositionEdits();
  update();
}

void App::leaveEvent(QEvent*) {
  hover_ = Hit();
  edgeHover_ = {};
  update();
}

// ------------------------------------------------------------------ ustawienia
std::wstring App::SettingsPath() {
  QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/carton";
  QDir().mkpath(dir);
  return FromQPath(dir + "/settings.ini");
}

void App::LoadSettings() {
  startPath_ = FromQPath(QDir::homePath());
  std::ifstream f(WToUtf8(SettingsPath()));
  std::string line;
  while (std::getline(f, line)) {
    size_t k = line.find('=');
    if (k == std::string::npos) continue;
    std::string key = line.substr(0, k);
    std::wstring val = Utf8ToW(line.substr(k + 1));
    while (!val.empty() && (val.back() == L'\r' || val.back() == L'\n')) val.pop_back();
    auto num = [&](float lo, float hi) { return std::clamp((float)wcstod(val.c_str(), nullptr), lo, hi); };
    if (key == "pin") pinned_.push_back(val);
    else if (key == "hidden") showHidden_ = val == L"1";
    else if (key == "sideW") sideW_ = num(170.f, 420.f);
    else if (key == "last" && !val.empty()) {
      FileStat fs;
      if (StatPath(val, fs) && (fs.attrs & A_DIR)) startPath_ = val;
    } else if (key == "colDate") colW_[1] = num(90.f, 300.f);
    else if (key == "colType") colW_[2] = num(80.f, 400.f);
    else if (key == "colSize") colW_[3] = num(70.f, 250.f);
  }
}

void App::SaveSettings() {
  std::ofstream f(WToUtf8(SettingsPath()), std::ios::trunc);
  if (!f) return;
  for (auto& p : pinned_) f << "pin=" << WToUtf8(p) << "\n";
  f << "hidden=" << (showHidden_ ? 1 : 0) << "\n";
  f << "sideW=" << (int)sideW_ << "\n";
  if (!tabs_.empty() && !T().path.empty() && T().path != kUnset) f << "last=" << WToUtf8(T().path) << "\n";
  f << "colDate=" << (int)colW_[1] << "\ncolType=" << (int)colW_[2] << "\ncolSize=" << (int)colW_[3] << "\n";
}

// ------------------------------------------------------------------ zakładki / nawigacja
void App::NewTab(const std::wstring& path) {
  Tab t;
  t.path = kUnset;
  tabs_.push_back(t);
  cur_ = (int)tabs_.size() - 1;
  Navigate(path, false);
  Layout();
}

void App::CloseTab(int i) {
  if (tabs_.size() <= 1) {
    close();
    return;
  }
  tabs_.erase(tabs_.begin() + i);
  if (cur_ >= (int)tabs_.size()) cur_ = (int)tabs_.size() - 1;
  else if (i < cur_) cur_--;
  Tab& t = T();
  restoreScroll_ = t.scroll;
  std::wstring p = t.path;
  Navigate(p, false);
  Layout();
}

void App::SwitchTab(int i) {
  if (i < 0 || i >= (int)tabs_.size() || i == cur_) return;
  T().scroll = scroll_;
  cur_ = i;
  restoreScroll_ = T().scroll;
  std::wstring p = T().path;
  Navigate(p, false);
}

void App::Navigate(const std::wstring& pathIn, bool push) {
  std::wstring path = pathIn == kUnset ? L"" : pathIn;
  if (!path.empty()) path = PathNormalize(path);
  Tab& t = T();
  if (searchActive_ || !t.query.empty()) ExitSearch(true);
  if (push && t.path != kUnset && t.path != path) {
    t.back.push_back(t.path);
    t.fwd.clear();
    if (t.back.size() > 200) t.back.erase(t.back.begin());
  }
  t.path = path;
  curPath_ = path;
  ShowAddrEdit(false);
  if (renameIdx_ >= 0) RenameCommit(false);
  if (path.empty()) {
    thisPC_ = true;
    items_.clear();
    dirNode_ = NONE;
    watcher_.Stop();
    loading_ = false;
    loadError_.clear();
    ++loadGen_;
    RefreshDrivesAsync();
    UpdateTitle();
    Layout();
    SetScroll(0, false);
    update();
    return;
  }
  thisPC_ = false;
  UpdateTitle();
  Layout();
  LoadAsync(path, false);
}

void App::GoBack() {
  Tab& t = T();
  if (t.back.empty()) return;
  std::wstring p = t.back.back();
  t.back.pop_back();
  t.fwd.push_back(t.path);
  Navigate(p, false);
}

void App::GoFwd() {
  Tab& t = T();
  if (t.fwd.empty()) return;
  std::wstring p = t.fwd.back();
  t.fwd.pop_back();
  t.back.push_back(t.path);
  Navigate(p, false);
}

void App::GoUp() {
  if (curPath_.empty()) return;
  Navigate(PathParent(curPath_), true);  // pusty = Ten komputer
}

void App::Reload(bool keepSel) {
  if (curPath_.empty() || searchActive_) return;
  pendingSelect_.clear();
  if (keepSel)
    for (auto& it : items_)
      if (it.sel) pendingSelect_.push_back(it.name);
  LoadAsync(curPath_, keepSel);
}

void App::UpdateTitle() {
  std::wstring name = curPath_.empty() ? L"Ten komputer" : PathName(curPath_);
  if (name.empty()) name = curPath_;
  T().title = name;
  setWindowTitle(Q(name) + " - Carton Explorer");
}

void App::LoadAsync(const std::wstring& path, bool keepSel) {
  u64 gen = ++loadGen_;
  loading_ = true;
  loadStart_ = NowSec();
  loadError_.clear();
  bool hidden = showHidden_;
  int key = T().sortCol;
  bool asc = T().sortAsc;
  loaders_++;
  watcher_.Stop();
  std::thread([=] {
    auto o = std::make_shared<LoadOut>();
    o->gen = gen;
    o->path = path;
    o->keepSel = keepSel;
    auto& v = o->items;
    o->err = EnumDir(path, [&](const FsEntry& e) {
      if (!hidden && (e.attrs & (A_HIDDEN | A_SYSTEM))) return;
      Item it;
      it.name.assign(e.name, e.nameLen);
      it.path = PathJoin(path, it.name);
      it.attrs = e.attrs;
      it.size = e.size;
      it.mtime = e.mtime;
      it.isDir = (e.attrs & A_DIR) != 0;
      it.isLink = e.reparseTag == TAG_SYMLINK;
      if (!it.isDir) it.ext = PathExt(it.name);
      it.dispDate = FormatDateTime(e.mtime);
      v.push_back(std::move(it));
    });
    if (!o->err) {
      u32 dn = idx_.Resolve(path);
      if (dn != NONE) {
        idx_.EnsureListed(dn);
        std::vector<u32> pending;
        for (auto& it : v)
          if (it.isDir) {
            u32 c = idx_.FindChild(dn, it.name.data(), it.name.size());
            it.node = c;
            if (c != NONE) {
              SizeInfo si;
              idx_.GetInfo(c, si);
              it.sizeState = si.complete ? 2 : 1;
              it.folderSize = si.size;
              it.fileCount = si.files;
              it.dirCount = si.dirs;
              if (!si.complete && pending.size() < 400) pending.push_back(c);
            }
          }
        for (size_t i = pending.size(); i-- > 0;) idx_.Prioritize(pending[i]);
        o->dirNode = dn;
        if (idx_.WatchLimited()) idx_.MarkDirty(path);  // bez inotify dla tego katalogu: zsynchronizuj indeks przy wejściu
      }
    }
    SortVec(v, key, asc, false);
    Post([this, o] { OnLoaded(o.get()); });
    loaders_--;
  }).detach();
}

void App::OnLoaded(LoadOut* o) {
  if (o->gen != loadGen_.load()) return;
  loading_ = false;
  if (o->err) {
    if (o->err == ENOTDIR) {  // to plik
      std::wstring f = o->path;
      Tab& t = T();
      if (!t.back.empty()) { std::wstring p = t.back.back(); t.back.pop_back(); t.path = p; curPath_ = p; Reload(false); }
      OpenPath(f);
      return;
    }
    loadError_ = L"Nie można otworzyć tego folderu: " + ErrnoText(o->err);
    items_.clear();
    update();
    return;
  }
  items_ = std::move(o->items);
  dirNode_ = o->dirNode;
  curPath_ = o->path;
  anchor_ = focus_ = -1;
  if (o->keepSel || !pendingSelect_.empty()) {
    for (auto& it : items_)
      for (auto& n : pendingSelect_)
        if (it.name == n) { it.sel = true; break; }
    pendingSelect_.clear();
    if (renameIdx_ < 0)
      for (size_t i = 0; i < items_.size(); i++)
        if (items_[i].sel) { focus_ = anchor_ = (int)i; break; }
    float mx = ScrollMax();
    scroll_ = scrollTarget_ = std::min(scrollTarget_, mx);
  } else {
    float s = restoreScroll_ >= 0 ? restoreScroll_ : 0;
    scroll_ = scrollTarget_ = std::min(s, ScrollMax());
  }
  restoreScroll_ = -1;
  auto changed = [this] {
    Post([this] { dirTimer_.start(180); });
  };
  watcher_.Start(curPath_, false, [changed](const std::wstring&, u32) { changed(); }, changed);
  sizesPending_ = true;
  if (!renameAfterLoad_.empty()) {
    std::wstring n = renameAfterLoad_;
    renameAfterLoad_.clear();
    for (size_t i = 0; i < items_.size(); i++)
      if (items_[i].name == n) { SelectOnly((int)i); RenameBegin((int)i); break; }
  }
  update();
}

// ------------------------------------------------------------------ sortowanie
void App::SortVec(std::vector<Item>& v, int key, bool asc, bool searchMode) {
  auto cmpKey = [&](const Item& a, const Item& b) -> int {
    switch (key) {
      case 1: return a.mtime == b.mtime ? 0 : (a.mtime < b.mtime ? -1 : 1);
      case 2: return CompareNoCase(a.ext.data(), a.ext.size(), b.ext.data(), b.ext.size());
      case 3: {
        u64 x = a.isDir ? a.folderSize : a.size, y = b.isDir ? b.folderSize : b.size;
        return x == y ? 0 : (x < y ? -1 : 1);
      }
      case 4: {
        std::wstring la = PathParent(a.path), lb = PathParent(b.path);
        return NaturalCompare(la.data(), la.size(), lb.data(), lb.size());
      }
      case -1: return a.score == b.score ? 0 : (a.score < b.score ? -1 : 1);
      default: return NaturalCompare(a.name.data(), a.name.size(), b.name.data(), b.name.size());
    }
  };
  std::stable_sort(v.begin(), v.end(), [&](const Item& a, const Item& b) {
    if (!searchMode && a.isDir != b.isDir) return a.isDir;
    int c = cmpKey(a, b);
    if (c == 0 && key != 0) c = NaturalCompare(a.name.data(), a.name.size(), b.name.data(), b.name.size());
    return asc ? c < 0 : c > 0;
  });
}

void App::SortItems() {
  std::wstring an = anchor_ >= 0 && anchor_ < (int)items_.size() ? items_[anchor_].path : L"";
  std::wstring fn = focus_ >= 0 && focus_ < (int)items_.size() ? items_[focus_].path : L"";
  SortVec(items_, T().sortCol, T().sortAsc, searchActive_);
  anchor_ = focus_ = -1;
  for (size_t i = 0; i < items_.size(); i++) {
    if (!an.empty() && items_[i].path == an) anchor_ = (int)i;
    if (!fn.empty() && items_[i].path == fn) focus_ = (int)i;
  }
}

void App::RefreshFolderSizes(bool allowResort) {
  bool pending = false, changed = false;
  for (auto& it : items_) {
    if (!it.isDir || it.node == NONE) continue;
    SizeInfo si;
    idx_.GetInfo(it.node, si);
    u8 st = si.complete ? 2 : 1;
    if (si.size != it.folderSize || st != it.sizeState || si.files != it.fileCount) changed = true;
    it.folderSize = si.size;
    it.fileCount = si.files;
    it.dirCount = si.dirs;
    it.sizeState = st;
    if (!si.complete) pending = true;
  }
  sizesPending_ = pending;
  if (changed && allowResort && T().sortCol == 3 && NowSec() - lastResort_ > 0.8) {
    SortItems();
    lastResort_ = NowSec();
  }
}

// ------------------------------------------------------------------ indeks i wyszukiwanie
void App::StartIndex() {
  std::vector<std::wstring> roots;
  for (auto& m : ListMounts(false))
    if (m.kind == MountEntry::Fixed) roots.push_back(m.path);
  if (roots.empty()) roots.push_back(L"/");
  idx_.SetNotify([this] { Post([this] {
    RefreshFolderSizes(true);
    if (searchActive_ && !searchComplete_ && NowSec() - lastIndexUi_ > 1.0) {
      lastIndexUi_ = NowSec();
      RunSearch();
    }
    update();
  }); });
  idx_.Start(roots);
}

void App::OnSearchTextChanged() {
  std::wstring text = hSearch_->text().toStdWString();
  text = Utf8ToW(hSearch_->text().toUtf8().toStdString());
  T().query = text;
  size_t b = text.find_first_not_of(L" \t");
  if (b == std::wstring::npos) {
    if (searchActive_) ExitSearch(false);
    return;
  }
  if (!searchActive_) {
    searchActive_ = true;
    for (auto& it : items_) it.sel = false;
    anchor_ = focus_ = -1;
    if (T().sortCol == 2) T().sortCol = -1;
    else if (T().sortCol == 0 || T().sortCol == 1 || T().sortCol == 3) T().sortCol = -1;
    T().sortAsc = true;
    Layout();
  }
  RunSearch();
  update();
}

void App::RunSearch() {
  u32 scope = NONE;
  if (!T().searchAll && !curPath_.empty()) {
    scope = dirNode_ != NONE ? dirNode_ : idx_.ResolveNoList(curPath_);
    if (scope != NONE) idx_.Prioritize(scope);
  }
  u64 gen = ++sgen_;
  {
    std::lock_guard<std::mutex> l(smx_);
    sText_ = T().query;
    sScope_ = scope;
    sReqGen_ = gen;
    sHas_ = true;
  }
  scv_.notify_one();
}

void App::SearchThread() {
  for (;;) {
    std::wstring text;
    u32 scope;
    u64 gen;
    {
      std::unique_lock<std::mutex> lk(smx_);
      scv_.wait(lk, [&] { return sStop_ || sHas_; });
      if (sStop_) return;
      text = sText_;
      scope = sScope_;
      gen = sReqGen_;
      sHas_ = false;
    }
    auto out = std::make_shared<SearchOut>();
    out->gen = gen;
    Query q = Query::Parse(text);
    out->error = q.error;
    if (!q.Empty()) {
      SearchResult r = idx_.Search(q, scope, 200000, &sgen_, gen);
      if (sgen_.load() != gen) continue;
      out->ms = r.ms;
      out->truncated = r.truncated;
      out->complete = r.indexComplete;
      out->items.reserve(r.ids.size());
      for (u32 id : r.ids) {
        const Node& n = idx_.N(id);
        Item it;
        it.name.assign(idx_.Name(n), n.nameLen);
        it.path = idx_.Path(id);
        it.attrs = n.attrs;
        it.isDir = n.IsDir();
        it.isLink = (n.attrs & A_REPARSE) != 0;
        it.mtime = n.mtime;
        it.node = id;
        if (it.isDir) {
          it.folderSize = n.size.load();
          it.fileCount = n.files.load();
          it.dirCount = n.dirs.load();
          it.sizeState = (n.state.load() & ST_SIZED) ? 2 : 1;
        } else {
          it.size = n.size.load();
          it.ext = PathExt(it.name);
        }
        it.dispDate = FormatDateTime(n.mtime);
        it.score = q.Score(it.name.data(), it.name.size());
        out->items.push_back(std::move(it));
      }
      if (sgen_.load() != gen) continue;
      SortVec(out->items, -1, true, true);
    } else
      out->complete = idx_.GetStatus().bgComplete;
    Post([this, out] {
      if (out->gen != sgen_.load() || !searchActive_) return;
      items_ = std::move(out->items);
      searchMs_ = out->ms;
      searchTrunc_ = out->truncated;
      searchComplete_ = out->complete;
      searchErr_ = out->error;
      anchor_ = focus_ = -1;
      SetScroll(0, false);
      sizesPending_ = true;
      update();
    });
  }
}

void App::ExitSearch(bool clearText) {
  ++sgen_;
  bool was = searchActive_;
  searchActive_ = false;
  T().query.clear();
  if (hSearch_ && !hSearch_->text().isEmpty()) {
    hSearch_->blockSignals(true);
    hSearch_->clear();
    hSearch_->blockSignals(false);
  }
  if (T().sortCol == -1 || T().sortCol == 4) T().sortCol = 0;
  if (was) {
    items_.clear();
    anchor_ = focus_ = -1;
    Layout();
    if (!curPath_.empty()) Reload(false);
  }
  update();
}

void App::FocusSearch() {
  hSearch_->setFocus();
  hSearch_->selectAll();
}

void App::TestSearch(const QString& q) {
  hSearch_->setText(q);
}

void App::TestWaitIdle(int ms) {
  double t0 = NowSec();
  while ((NowSec() - t0) * 1000 < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    usleep(2000);
  }
}

// ------------------------------------------------------------------ dyski / pasek boczny
std::wstring App::DriveLabel(const DriveInfo& d) const {
  std::wstring l = d.label;
  if (l.empty()) {
    if (d.root == L"/") l = L"System plików";
    else
      l = d.kind == MountEntry::Removable ? L"Dysk wymienny" : d.kind == MountEntry::Optical ? L"Napęd CD/DVD" : d.kind == MountEntry::Remote ? L"Dysk sieciowy" : PathName(d.root);
  }
  return l + L" (" + d.root + L")";
}

Ic App::DriveIcon(const DriveInfo& d) const {
  return d.kind == MountEntry::Removable ? Ic::Usb : d.kind == MountEntry::Remote ? Ic::Network : d.kind == MountEntry::Optical ? Ic::Disc : Ic::Drive;
}

void App::RefreshDrivesAsync() {
  loaders_++;
  std::thread([this] {
    auto v = std::make_shared<std::vector<DriveInfo>>();
    for (auto& m : ListMounts(true)) {
      DriveInfo d;
      d.root = m.path;
      d.label = m.label;
      d.fs = m.fs;
      d.source = m.source;
      d.kind = m.kind;
      d.total = m.total;
      d.freeB = m.freeB;
      v->push_back(std::move(d));
    }
    Post([this, v] {
      drives_ = std::move(*v);
      BuildSidebar();
      Layout();
      update();
    });
    loaders_--;
  }).detach();
}

void App::BuildSidebar() {
  side_.clear();
  auto hdr = [&](const wchar_t* t) {
    SideEntry e;
    e.type = SideEntry::Header;
    e.label = t;
    side_.push_back(e);
  };
  hdr(L"Szybki dostęp");
  struct K { QStandardPaths::StandardLocation loc; const wchar_t* name; Ic g; };
  K ks[] = {{QStandardPaths::HomeLocation, L"Katalog domowy", Ic::Home},      {QStandardPaths::DesktopLocation, L"Pulpit", Ic::Desktop},
            {QStandardPaths::DownloadLocation, L"Pobrane", Ic::Download},     {QStandardPaths::DocumentsLocation, L"Dokumenty", Ic::Docs},
            {QStandardPaths::PicturesLocation, L"Obrazy", Ic::Pictures},      {QStandardPaths::MusicLocation, L"Muzyka", Ic::Music},
            {QStandardPaths::MoviesLocation, L"Wideo", Ic::Video}};
  std::set<std::wstring> seen;
  for (auto& k : ks) {
    QString p = QStandardPaths::writableLocation(k.loc);
    if (p.isEmpty()) continue;
    std::wstring wp = FromQPath(p);
    FileStat fs;
    if (!StatPath(wp, fs) || !(fs.attrs & A_DIR)) continue;
    if (k.loc != QStandardPaths::HomeLocation && wp == FromQPath(QDir::homePath())) continue;  // XDG wskazuje na $HOME
    if (!seen.insert(wp).second) continue;
    SideEntry e;
    e.type = SideEntry::Folder;
    e.label = k.name;
    e.path = wp;
    e.glyph = k.g;
    side_.push_back(e);
  }
  {
    std::wstring tr = TrashFilesDir();
    FileStat fs;
    if (StatPath(tr, fs)) {
      SideEntry e;
      e.type = SideEntry::Folder;
      e.label = L"Kosz";
      e.path = tr;
      e.glyph = Ic::Trash;
      side_.push_back(e);
    }
  }
  for (auto& p : pinned_) {
    SideEntry e;
    e.type = SideEntry::Folder;
    e.label = PathName(p);
    if (e.label.empty()) e.label = p;
    e.path = p;
    e.glyph = Ic::Pin;
    e.pinned = true;
    side_.push_back(e);
  }
  SideEntry pc;
  pc.type = SideEntry::ThisPC;
  pc.label = L"Ten komputer";
  pc.glyph = Ic::PC;
  side_.push_back(pc);
  for (auto& d : drives_) {
    SideEntry e;
    e.type = SideEntry::Drive;
    e.label = DriveLabel(d);
    e.path = d.root;
    e.glyph = DriveIcon(d);
    e.total = d.total;
    e.freeB = d.freeB;
    side_.push_back(e);
  }
}

void App::PositionEdits() {
  if (hSearch_) {
    int h = 22;
    hSearch_->setGeometry((int)(rSearch_.x + 38), (int)(rSearch_.y + rSearch_.h / 2) - h / 2, (int)(rSearch_.w - 38 - 34), h);
  }
  if (hAddr_ && addrEditing_) {
    int h = 22;
    hAddr_->setGeometry((int)(rAddr_.x + 14), (int)(rAddr_.y + rAddr_.h / 2) - h / 2, (int)(rAddr_.w - 28), h);
  }
}

void App::ShowAddrEdit(bool show) {
  if (show == addrEditing_) return;
  addrEditing_ = show;
  if (show) {
    hAddr_->setText(Q(curPath_));
    PositionEdits();
    hAddr_->show();
    hAddr_->raise();
    hAddr_->setFocus();
    hAddr_->selectAll();
  } else {
    hAddr_->hide();
    if (hAddr_->hasFocus()) setFocus();
  }
  update();
}

// ------------------------------------------------------------------ edycja: filtr zdarzeń
bool App::eventFilter(QObject* o, QEvent* ev) {
  if (o == hSearch_ || o == hAddr_ || o == hRename_) {
    if (ev->type() == QEvent::KeyPress) {
      auto* k = static_cast<QKeyEvent*>(ev);
      int key = k->key();
      if (o == hSearch_) {
        if (key == Qt::Key_Escape) { ExitSearch(true); setFocus(); return true; }
        if (key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter) {
          setFocus();
          if (!items_.empty()) { SelectOnly(0); EnsureVisible(0); }
          update();
          return true;
        }
      } else if (o == hAddr_) {
        if (key == Qt::Key_Escape) { ShowAddrEdit(false); return true; }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
          QString t = hAddr_->text().trimmed();
          if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.mid(1, t.size() - 2);
          if (t.startsWith("~")) t = QDir::homePath() + t.mid(1);
          std::wstring wt = FromQPath(t);
          std::wstring f = FoldStr(wt);
          ShowAddrEdit(false);
          if (wt.empty() || f == L"ten komputer" || f == L"this pc") Navigate(L"", true);
          else Navigate(wt, true);
          return true;
        }
      } else if (o == hRename_) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { RenameCommit(true); return true; }
        if (key == Qt::Key_Escape) { RenameCommit(false); return true; }
      }
    } else if (ev->type() == QEvent::FocusOut) {
      if (o == hRename_ && renameIdx_ >= 0) QTimer::singleShot(0, this, [this] { if (renameIdx_ >= 0 && !hRename_->hasFocus()) RenameCommit(true); });
      else if (o == hAddr_ && addrEditing_) {
        QTimer::singleShot(0, this, [this] {
          QWidget* fw = QApplication::focusWidget();
          bool popup = QApplication::activePopupWidget() != nullptr;
          if (addrEditing_ && !hAddr_->hasFocus() && !popup && fw != hAddr_) ShowAddrEdit(false);
        });
      }
      update();
    } else if (ev->type() == QEvent::FocusIn)
      update();
  }
  return QWidget::eventFilter(o, ev);
}

}  // namespace nv

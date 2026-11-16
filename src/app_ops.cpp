#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <QPixmap>
#include <algorithm>
#include <cmath>

#include "app.h"

namespace nv {

enum {
  CMD_NEWFOLDER = 1, CMD_NEWFILE, CMD_PASTE, CMD_REFRESH, CMD_TERMINAL, CMD_PROPS, CMD_CUT, CMD_COPY, CMD_RENAME, CMD_DELETE, CMD_SORT,
  CMD_HIDDEN, CMD_COPYPATH, CMD_SORT_BASE = 21, CMD_ASC = 40, CMD_DESC = 41
};

static QString Q(const std::wstring& s) { return QString::fromStdWString(s); }

// ------------------------------------------------------------------ zaznaczenie
std::vector<std::wstring> App::SelectedPaths() const {
  std::vector<std::wstring> r;
  for (auto& it : items_)
    if (it.sel) r.push_back(it.path);
  return r;
}
int App::SelCount() const {
  int n = 0;
  for (auto& it : items_) n += it.sel;
  return n;
}
u64 App::SelSize() const {
  u64 s = 0;
  for (auto& it : items_)
    if (it.sel && !it.isDir) s += it.size;
  return s;
}
void App::ClearSel() {
  for (auto& it : items_) it.sel = false;
}
void App::SelectOnly(int i) {
  ClearSel();
  if (i >= 0 && i < (int)items_.size()) items_[i].sel = true;
  anchor_ = focus_ = i;
}
void App::SelectRange(int a, int b, bool additive) {
  if (!additive) ClearSel();
  if (a > b) std::swap(a, b);
  a = std::max(a, 0);
  b = std::min(b, (int)items_.size() - 1);
  for (int i = a; i <= b; i++) items_[i].sel = true;
}

float App::ScrollMax() const { return std::max(0.f, (float)items_.size() * rowH_ + 8.f - rList_.h); }

void App::SetScroll(float s, bool animate) {
  s = std::clamp(s, 0.f, ScrollMax());
  scrollTarget_ = s;
  if (!animate) scroll_ = s;
  update();
}

void App::EnsureVisible(int i) {
  if (i < 0) return;
  float top = 4 + i * rowH_, viewH = rList_.h;
  if (top < scrollTarget_) SetScroll(top - 4, true);
  else if (top + rowH_ > scrollTarget_ + viewH - 4) SetScroll(top + rowH_ - viewH + 8, true);
}

bool App::CanPaste() const { return !curPath_.empty() && !searchActive_ && clipHasFiles_; }

// ------------------------------------------------------------------ otwieranie
void App::OpenPath(const std::wstring& p) {
  if (!OpenPathDefault(p)) Alert(L"Nie można otworzyć", p + L"\n\nBrak skojarzonej aplikacji lub dostępu.");
}

void App::OpenItem(int i) {
  if (i < 0 || i >= (int)items_.size()) return;
  Item it = items_[i];
  if (it.isDir) Navigate(it.path, true);
  else OpenPath(it.path);
}

// ------------------------------------------------------------------ schowek
void App::CopySel(bool cut) {
  auto paths = SelectedPaths();
  if (paths.empty()) return;
  ownClip_ = true;
  if (ClipboardSetFiles(paths, cut)) {
    cutSet_.clear();
    if (cut)
      for (auto& p : paths) cutSet_.insert(p);
    clipHasFiles_ = true;
  }
  update();
}

void App::Paste() {
  if (!CanPaste()) return;
  std::vector<std::wstring> paths;
  bool cut = false;
  if (!ClipboardGetFiles(paths, cut)) return;
  StartJob(cut ? JobOp::Move : JobOp::Copy, paths, curPath_);
  if (cut) {
    ownClip_ = true;
    ClipboardClear();
    cutSet_.clear();
    clipHasFiles_ = false;
  }
}

void App::StartJob(JobOp op, std::vector<std::wstring> src, const std::wstring& dest) {
  if (src.empty()) return;
  jobs_.Start(op, std::move(src), dest, [this](const ConflictInfo& ci, bool& all) { return JobConflict(ci, all); });
  update();
}

void App::DoDrop(const std::vector<std::wstring>& paths, const std::wstring& dest, Qt::DropAction act) {
  if (dest == TrashFilesDir()) {
    TrashAsync(paths);
    return;
  }
  JobOp op = act == Qt::MoveAction ? JobOp::Move : JobOp::Copy;
  if (op == JobOp::Move) {
    bool allSame = true;
    for (auto& p : paths)
      if (PathNormalize(PathParent(p)) != PathNormalize(dest)) allSame = false;
    if (allSame) return;
  }
  StartJob(op, paths, dest);
}

// ------------------------------------------------------------------ usuwanie
void App::DeleteSel(bool permanent) {
  auto paths = SelectedPaths();
  if (paths.empty()) return;
  std::wstring trash = TrashFilesDir();
  for (auto& p : paths)
    if (PathIsUnder(p, trash)) permanent = true;
  if (!permanent) {
    TrashAsync(paths);
    return;
  }
  Modal m;
  m.title = L"Usunąć trwale?";
  m.lines = {paths.size() == 1 ? L"„" + PathName(paths[0]) + L"”" : std::to_wstring(paths.size()) + L" elementów",
             L"Tej operacji nie można cofnąć — elementy nie trafią do Kosza."};
  m.btns = {L"Usuń trwale", L"Anuluj"};
  m.def = 1;
  m.cancelBtn = 1;
  m.danger = true;
  m.cb = [this, paths](int b, bool) {
    if (b == 0) StartJob(JobOp::Delete, paths, L"");
  };
  ShowModal(m);
}

void App::TrashAsync(std::vector<std::wstring> paths) {
  loaders_++;
  std::thread([this, paths] {
    std::vector<std::wstring> failed;
    for (auto& p : paths)
      if (!MoveToTrash(p)) failed.push_back(p);
    Post([this, failed] {
      if (!failed.empty()) {
        Modal m;
        m.title = L"Nie można przenieść do Kosza";
        m.lines = {failed.size() == 1 ? L"„" + PathName(failed[0]) + L"”" : std::to_wstring(failed.size()) + L" elementów",
                   L"Kosz nie jest dostępny dla tego miejsca (np. dysk sieciowy lub brak uprawnień).",
                   L"Czy usunąć je trwale?"};
        m.btns = {L"Usuń trwale", L"Anuluj"};
        m.def = 1;
        m.cancelBtn = 1;
        m.danger = true;
        m.cb = [this, failed](int b, bool) {
          if (b == 0) StartJob(JobOp::Delete, failed, L"");
        };
        ShowModal(m);
      }
      Reload(true);
    });
    loaders_--;
  }).detach();
}

// ------------------------------------------------------------------ zmiana nazwy / tworzenie
static int RenameNoReplace(const std::string& a, const std::string& b) {
  long r = syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 1 /*RENAME_NOREPLACE*/);
  if (r == 0) return 0;
  if (errno == ENOSYS || errno == EINVAL) {
    struct stat st;
    if (lstat(b.c_str(), &st) == 0) return EEXIST;
    return rename(a.c_str(), b.c_str()) == 0 ? 0 : errno;
  }
  return errno;
}

void App::RenameBegin(int i) {
  if (i < 0 || i >= (int)items_.size()) return;
  if (renameIdx_ >= 0) RenameCommit(true);
  EnsureVisible(i);
  scroll_ = scrollTarget_;
  renameIdx_ = i;
  const Item& it = items_[i];
  float y = rList_.y + 4 + i * rowH_ - scroll_;
  float x = rList_.x + 10 + 32;
  float w = std::max(120.f, colX_[1] - x - 10);
  hRename_->setText(Q(it.name));
  hRename_->setGeometry((int)x, (int)(y + 6), (int)w, (int)(rowH_ - 10));
  hRename_->show();
  hRename_->raise();
  hRename_->setFocus();
  size_t dot = it.isDir ? std::wstring::npos : it.name.rfind(L'.');
  if (dot == std::wstring::npos || dot == 0) hRename_->selectAll();
  else hRename_->setSelection(0, (int)QString::fromStdWString(it.name.substr(0, dot)).size());
  update();
}

void App::RenameCommit(bool apply) {
  if (renameIdx_ < 0) return;
  int i = renameIdx_;
  renameIdx_ = -1;
  std::wstring nn = Utf8ToW(hRename_->text().toUtf8().toStdString());
  bool hadFocus = hRename_->hasFocus();
  hRename_->hide();
  if (hadFocus) setFocus();
  update();
  if (!apply || i >= (int)items_.size()) return;
  Item& it = items_[i];
  if (nn.empty() || nn == it.name) return;
  if (nn.find(L'/') != std::wstring::npos || nn == L"." || nn == L".." || nn.find(L'\0') != std::wstring::npos) {
    Alert(L"Niepoprawna nazwa", L"Nazwa nie może zawierać znaku / ani być równa „.” lub „..”");
    return;
  }
  std::wstring np = PathJoin(PathParent(it.path), nn);
  int err = RenameNoReplace(WToUtf8(it.path), WToUtf8(np));
  if (err) {
    Alert(L"Nie udało się zmienić nazwy", err == EEXIST ? L"Element o tej nazwie już istnieje" : ErrnoText((u32)err));
    return;
  }
  if (searchActive_) {
    it.name = nn;
    it.path = np;
    it.ext = it.isDir ? L"" : PathExt(nn);
  } else {
    pendingSelect_ = {nn};
    LoadAsync(curPath_, true);
  }
}

static std::wstring FirstFree(const std::wstring& dir, const std::wstring& base, bool isDir) {
  FileStat fs;
  if (!StatPath(PathJoin(dir, base), fs)) return base;
  return UniqueName(dir, base, false, isDir);
}

void App::NewFolder() {
  if (curPath_.empty() || searchActive_) return;
  std::wstring name = FirstFree(curPath_, L"Nowy folder", true);
  if (mkdir(WToUtf8(PathJoin(curPath_, name)).c_str(), 0777) != 0) {
    Alert(L"Nie można utworzyć folderu", ErrnoText((u32)errno));
    return;
  }
  pendingSelect_ = {name};
  renameAfterLoad_ = name;
  LoadAsync(curPath_, true);
}

void App::NewFile() {
  if (curPath_.empty() || searchActive_) return;
  std::wstring name = FirstFree(curPath_, L"Nowy dokument tekstowy.txt", false);
  int fd = open(WToUtf8(PathJoin(curPath_, name)).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (fd < 0) {
    Alert(L"Nie można utworzyć pliku", ErrnoText((u32)errno));
    return;
  }
  ::close(fd);
  pendingSelect_ = {name};
  renameAfterLoad_ = name;
  LoadAsync(curPath_, true);
}

void App::CopyPathToClipboard() {
  auto paths = SelectedPaths();
  if (paths.empty() && !curPath_.empty()) paths.push_back(curPath_);
  std::wstring s;
  for (size_t i = 0; i < paths.size(); i++) {
    if (i) s += L"\n";
    s += paths[i];
  }
  ownClip_ = true;
  ClipboardSetText(s);
}

void App::OpenTerminal() {
  if (curPath_.empty()) return;
  if (!OpenTerminalIn(curPath_)) Alert(L"Nie można otworzyć terminala", L"Nie znaleziono emulatora terminala.\nUstaw zmienną środowiskową TERMINAL.");
}

void App::PinToggle(const std::wstring& path) {
  auto it = std::find(pinned_.begin(), pinned_.end(), path);
  if (it != pinned_.end()) pinned_.erase(it);
  else pinned_.push_back(path);
  BuildSidebar();
  Layout();
  SaveSettings();
  update();
}

void App::ShowProperties(const std::wstring& path) {
  FileStat st;
  if (!StatPath(path, st)) {
    Alert(L"Właściwości", L"Nie można odczytać: " + path);
    return;
  }
  bool dir = (st.attrs & A_DIR) != 0;
  Modal m;
  m.title = L"Właściwości";
  std::wstring nm = PathName(path);
  m.lines.push_back(L"Nazwa:   " + nm);
  m.lines.push_back(L"Typ:   " + (dir ? std::wstring(L"Folder") : g_.TypeName(nm, PathExt(nm)).toStdWString()) + ((st.attrs & A_REPARSE) ? L" (dowiązanie symboliczne)" : L""));
  m.lines.push_back(L"Lokalizacja:   " + (PathParent(path).empty() ? path : PathParent(path)));
  if (dir) {
    u32 n = idx_.ResolveNoList(path);
    if (n != NONE) {
      SizeInfo si;
      idx_.GetInfo(n, si);
      m.lines.push_back(L"Rozmiar:   " + FormatSize(si.size) + (si.complete ? L"" : L" (liczenie trwa…)"));
      m.lines.push_back(L"Zawiera:   " + FormatCount(si.files) + L" plików, " + FormatCount(si.dirs) + L" folderów");
    }
  } else
    m.lines.push_back(L"Rozmiar:   " + FormatSize(st.size) + L" (" + FormatCount(st.size) + L" B)");
  m.lines.push_back(L"Zmodyfikowano:   " + FormatDateTime(st.mtime));
  m.lines.push_back(L"Uprawnienia:   " + PermString(st.mode).toStdWString() + L"   " + OwnerName(st.uid).toStdWString() + L":" + GroupName(st.gid).toStdWString());
  m.btns = {L"Zamknij"};
  m.cancelBtn = 0;
  ShowModal(m);
}

// ------------------------------------------------------------------ modale
void App::ShowModal(Modal m) {
  modals_.push_back(std::move(m));
  PumpModals();
}

void App::Alert(const std::wstring& title, const std::wstring& text) {
  Modal m;
  m.title = title;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t k = text.find(L'\n', pos);
    m.lines.push_back(text.substr(pos, k == std::wstring::npos ? k : k - pos));
    if (k == std::wstring::npos) break;
    pos = k + 1;
  }
  m.btns = {L"OK"};
  m.def = 0;
  m.cancelBtn = 0;
  ShowModal(m);
}

void App::PumpModals() {
  if (modalActive_) return;
  ConflictReq* req = nullptr;
  {
    std::lock_guard<std::mutex> l(cmx_);
    if (!creqs_.empty()) {
      req = creqs_.front();
      creqs_.pop_front();
    }
  }
  if (req) {
    Modal m;
    const ConflictInfo& ci = req->ci;
    m.title = L"Plik już istnieje";
    m.lines = {L"Miejsce docelowe zawiera już plik o tej samej nazwie:", PathName(ci.dst),
               L"Źródło:    " + FormatSize(ci.srcSize) + L"   ·   " + FormatDateTime(ci.srcTime),
               L"Istniejący:   " + FormatSize(ci.dstSize) + L"   ·   " + FormatDateTime(ci.dstTime)};
    m.btns = {L"Zamień", L"Pomiń", L"Zachowaj oba", L"Anuluj"};
    m.def = 0;
    m.cancelBtn = 3;
    m.hasCheck = true;
    m.checkText = L"Zastosuj do wszystkich konfliktów";
    m.cb = [req](int b, bool chk) {
      std::lock_guard<std::mutex> l(req->m);
      req->res = b == 0 ? Resolution::Replace : b == 1 ? Resolution::Skip : b == 2 ? Resolution::KeepBoth : Resolution::Cancel;
      req->all = chk;
      req->done = true;
      req->cv.notify_all();
    };
    modals_.push_front(m);
  }
  if (!modals_.empty()) {
    modalActive_ = true;
    LayoutModal();
    update();
  }
}

void App::CloseModal(int btn) {
  if (!modalActive_ || modals_.empty()) return;
  Modal m = std::move(modals_.front());
  modals_.pop_front();
  modalActive_ = false;
  if (m.cb) m.cb(btn, m.checked);
  update();
  PumpModals();
}

void App::ShowJobErrors(CopyJob& j) {
  auto errs = j.Errors();
  Modal m;
  m.title = L"Operacja zakończona z błędami (" + std::to_wstring(errs.size()) + L")";
  for (size_t i = 0; i < errs.size() && i < 8; i++) {
    m.lines.push_back(PathName(errs[i].path));
    m.lines.push_back(L"   " + errs[i].msg);
  }
  if (errs.size() > 8) m.lines.push_back(L"… i " + std::to_wstring(errs.size() - 8) + L" więcej");
  m.btns = {L"OK"};
  m.cancelBtn = 0;
  ShowModal(m);
}

Resolution App::JobConflict(const ConflictInfo& ci, bool& all) {
  ConflictReq req;
  req.ci = ci;
  {
    std::lock_guard<std::mutex> l(cmx_);
    creqs_.push_back(&req);
  }
  Post([this] { PumpModals(); });
  {
    std::unique_lock<std::mutex> lk(req.m);
    while (!req.done) {
      req.cv.wait_for(lk, std::chrono::milliseconds(100));
      if (closing_ && !req.done) {
        lk.unlock();
        {
          std::lock_guard<std::mutex> l(cmx_);
          creqs_.erase(std::remove(creqs_.begin(), creqs_.end(), &req), creqs_.end());
        }
        lk.lock();
        req.res = Resolution::Cancel;
        req.done = true;
      }
    }
  }
  all = req.all;
  return req.res;
}

void App::OnJobEvent(u32 id) {
  for (auto& j : jobs_.Jobs()) {
    if (j->id != id) continue;
    if (j->Finished()) {
      double now = NowSec();
      if (j->State() == JobState::Done) dismissAt_[id] = now + 2.8;
      else if (j->State() == JobState::Cancelled) dismissAt_[id] = now + 1.6;
      if (j->op != JobOp::Copy) cutSet_.clear();
      if (!searchActive_ && !curPath_.empty()) Reload(true);
    }
  }
  update();
}

// ------------------------------------------------------------------ cel upuszczania
std::wstring App::DropTargetFor(QPoint c, int* row, int* side, int* crumb) {
  float x = (float)c.x(), y = (float)c.y();
  *row = *side = *crumb = -1;
  if (modalActive_) return L"";
  if (rSide_.Has(x, y)) {
    for (size_t i = 0; i < side_.size(); i++)
      if (side_[i].rect.Has(x, y) && (side_[i].type == SideEntry::Folder || side_[i].type == SideEntry::Drive)) {
        *side = (int)i;
        return side_[i].path;
      }
    return L"";
  }
  for (size_t i = 0; i < crumbs_.size(); i++)
    if (crumbs_[i].r.Has(x, y) && !crumbs_[i].path.empty()) {
      *crumb = (int)i;
      return crumbs_[i].path;
    }
  if (thisPC_) {
    for (size_t i = 0; i < tileRects_.size() && i < drives_.size(); i++)
      if (tileRects_[i].Has(x, y)) {
        *row = (int)i;
        return drives_[i].root;
      }
    return L"";
  }
  if (rMain_.Has(x, y)) {
    if (searchActive_) return L"";
    if (rList_.Has(x, y)) {
      int r = (int)std::floor((y - rList_.y - 4 + scroll_) / rowH_);
      if (r >= 0 && r < (int)items_.size() && items_[r].isDir) {
        *row = r;
        return items_[r].path;
      }
    }
    return curPath_;
  }
  return L"";
}

Qt::DropAction App::DragOverImpl(QPoint c, Qt::KeyboardModifiers mods, Qt::DropActions allowed) {
  int row, side, crumb;
  std::wstring dest = DropTargetFor(c, &row, &side, &crumb);
  if (row != dropRow_ || side != dropSide_ || crumb != dropCrumb_) update();
  dropRow_ = row;
  dropSide_ = side;
  dropCrumb_ = crumb;
  float y = (float)c.y();
  dragAutoScroll_ = 0;
  if (rList_.Has((float)c.x(), y) || (y > rList_.y - 12 && y < rList_.b() + 12)) {
    if (y < rList_.y + 26) dragAutoScroll_ = -1;
    else if (y > rList_.b() - 26) dragAutoScroll_ = 1;
  }
  if (dest.empty()) return Qt::IgnoreAction;
  if (dragSrcSerial_ == 0 && !dragPaths_.empty()) dragSrcSerial_ = VolumeSerial(PathParent(dragPaths_[0]));
  if (dest != lastDest_) {
    lastDest_ = dest;
    lastDestSerial_ = VolumeSerial(dest);
  }
  bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
  Qt::DropAction eff;
  if (ctrl) eff = Qt::CopyAction;
  else if (shift) eff = Qt::MoveAction;
  else eff = (dragSrcSerial_ && dragSrcSerial_ == lastDestSerial_) ? Qt::MoveAction : Qt::CopyAction;
  if (dest == TrashFilesDir()) eff = Qt::MoveAction;
  if (!(allowed & eff)) eff = (allowed & Qt::CopyAction) ? Qt::CopyAction : (allowed & Qt::MoveAction) ? Qt::MoveAction : Qt::IgnoreAction;
  for (auto& p : dragPaths_)
    if (PathIsUnder(dest, p)) return Qt::IgnoreAction;  // do samego siebie / podfolderu
  if (eff == Qt::MoveAction) {
    bool allSame = !dragPaths_.empty();
    for (auto& p : dragPaths_)
      if (PathNormalize(PathParent(p)) != PathNormalize(dest)) allSame = false;
    if (allSame) return Qt::IgnoreAction;
  }
  return eff;
}

void App::BeginDrag() {
  auto paths = SelectedPaths();
  if (paths.empty()) return;
  dragCand_ = false;
  const Item* first = nullptr;
  for (auto& it : items_)
    if (it.sel) { first = &it; break; }
  std::wstring label = paths.size() == 1 && first ? first->name : std::to_wstring(paths.size()) + L" elementów";
  float dpr = (float)devicePixelRatioF();
  float tw = std::min(g_.TextW(label, Gfx::UI), 220.f);
  int w = (int)(14 + 24 + 8 + tw + 18 + (paths.size() > 1 ? 30 : 0)), h = 40;
  QPixmap pm((int)(w * dpr), (int)(h * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  {
    QPainter p(&pm);
    g_.Begin(&p, dpr);
    Rf box{1, 1, (float)w - 2, (float)h - 2};
    g_.FillR(box, 10, A(th::elev2, 0.94f));
    g_.StrokeR(box, 10, th::selEdge, 1.5f);
    Rf ir{12, 8, 24, 24};
    if (first && first->isDir) g_.DrawFolderIcon(ir);
    else if (first) g_.DrawFileIcon(first->name, first->ext, ir, 1.f);
    g_.Text(label, {46, 0, tw + 4, (float)h}, Gfx::UI, th::text);
    if (paths.size() > 1) {
      Rf badge{46 + tw + 10, 9, 24, 22};
      g_.FillR(badge, 11, th::accent);
      g_.Text(std::to_wstring(paths.size()), badge, Gfx::SmallB, C(0xffffff), Al::Center);
    }
    g_.End();
  }
  dragPaths_ = paths;
  dragSrcSerial_ = VolumeSerial(PathParent(paths[0]));
  lastDest_.clear();
  dragFromSelf_ = true;
  down_ = Hit();
  StartFileDrag(this, paths, pm, QPoint(24, 20));
  dragFromSelf_ = false;
  dragPaths_.clear();
  dragSrcSerial_ = 0;
  dropRow_ = dropSide_ = dropCrumb_ = -1;
  dragAutoScroll_ = 0;
  dropActive_ = false;
  update();
}

// ------------------------------------------------------------------ menu
void App::ShowRowMenu(QPoint screen) {
  auto paths = SelectedPaths();
  if (paths.empty()) return;
  const Item* single = nullptr;
  if (paths.size() == 1)
    for (auto& it : items_)
      if (it.sel) single = &it;
  std::vector<MenuItem> m;
  m.push_back({1, "Otwórz"});
  m.back().shortcut = "Enter";
  std::vector<AppEntry> apps;
  if (single && !single->isDir) {
    apps = AppsForFile(single->path);
    if (!apps.empty()) {
      MenuItem sub;
      sub.text = "Otwórz za pomocą";
      for (size_t i = 0; i < apps.size(); i++) {
        MenuItem a;
        a.id = 100 + (int)i;
        a.text = apps[i].name;
        sub.sub.push_back(a);
      }
      m.push_back(sub);
    }
  }
  if (single && single->isDir) {
    m.push_back({2, "Otwórz w nowej karcie"});
    bool pinned = std::find(pinned_.begin(), pinned_.end(), single->path) != pinned_.end();
    m.push_back({3, pinned ? "Odepnij z Szybkiego dostępu" : "Przypnij do Szybkiego dostępu"});
    m.push_back({12, "Otwórz terminal tutaj"});
  }
  if (searchActive_ && single) m.push_back({11, "Otwórz lokalizację"});
  MenuItem sep;
  sep.separator = true;
  m.push_back(sep);
  m.push_back({4, "Wytnij"});
  m.back().shortcut = "Ctrl+X";
  m.push_back({5, "Kopiuj"});
  m.back().shortcut = "Ctrl+C";
  if (single && single->isDir) {
    MenuItem pst{6, "Wklej do folderu"};
    pst.enabled = clipHasFiles_;
    m.push_back(pst);
  }
  m.push_back({7, "Kopiuj ścieżkę"});
  m.back().shortcut = "Ctrl+Shift+C";
  m.push_back(sep);
  MenuItem rn{8, "Zmień nazwę"};
  rn.enabled = single != nullptr;
  rn.shortcut = "F2";
  m.push_back(rn);
  m.push_back({9, "Przenieś do Kosza"});
  m.back().shortcut = "Del";
  m.push_back({10, "Usuń trwale"});
  m.back().shortcut = "Shift+Del";
  m.push_back(sep);
  m.push_back({13, "Właściwości"});
  m.back().shortcut = "Alt+Enter";

  int r = ExecMenu(this, screen, m);
  if (r >= 100 && r < 100 + (int)apps.size() && single) {
    LaunchWith(apps[r - 100], {single->path});
    return;
  }
  switch (r) {
    case 1: OpenItem(focus_); break;
    case 2: if (single) NewTab(single->path); break;
    case 3: if (single) PinToggle(single->path); break;
    case 4: CopySel(true); break;
    case 5: CopySel(false); break;
    case 6:
      if (single && single->isDir) {
        std::vector<std::wstring> ps;
        bool cut;
        if (ClipboardGetFiles(ps, cut)) StartJob(cut ? JobOp::Move : JobOp::Copy, ps, single->path);
      }
      break;
    case 7: CopyPathToClipboard(); break;
    case 8: RenameBegin(focus_); break;
    case 9: DeleteSel(false); break;
    case 10: DeleteSel(true); break;
    case 11:
      if (single) {
        std::wstring loc = PathParent(single->path), nm = single->name;
        Navigate(loc, true);
        pendingSelect_ = {nm};
      }
      break;
    case 12: if (single) OpenTerminalIn(single->path); break;
    case 13: if (single) ShowProperties(single->path); break;
    default: break;
  }
}

static std::vector<MenuItem> SortItemsMenu(int cur, bool asc, bool search) {
  std::vector<MenuItem> v;
  const char* names[] = {"Nazwa", "Data modyfikacji", "Typ", "Rozmiar"};
  for (int i = 0; i < 4; i++) {
    MenuItem it{CMD_SORT_BASE + i, names[i]};
    it.checked = cur == i;
    v.push_back(it);
  }
  if (search) {
    MenuItem it{CMD_SORT_BASE + 4, "Lokalizacja"};
    it.checked = cur == 4;
    v.push_back(it);
  }
  MenuItem sep;
  sep.separator = true;
  v.push_back(sep);
  MenuItem a{CMD_ASC, "Rosnąco"};
  a.checked = asc;
  MenuItem d{CMD_DESC, "Malejąco"};
  d.checked = !asc;
  v.push_back(a);
  v.push_back(d);
  return v;
}

void App::ShowSortMenu(QPoint screen) {
  int cmd = ExecMenu(this, screen, SortItemsMenu(T().sortCol, T().sortAsc, searchActive_));
  if (cmd) OnCommand(cmd);
}

void App::ShowBackgroundMenu(QPoint screen) {
  bool inFolder = !curPath_.empty() && !searchActive_;
  std::vector<MenuItem> m;
  MenuItem nf{CMD_NEWFOLDER, "Nowy folder"};
  nf.enabled = inFolder;
  nf.shortcut = "Ctrl+Shift+N";
  MenuItem nfi{CMD_NEWFILE, "Nowy dokument tekstowy"};
  nfi.enabled = inFolder;
  MenuItem sep;
  sep.separator = true;
  m.push_back(nf);
  m.push_back(nfi);
  m.push_back(sep);
  MenuItem pst{CMD_PASTE, "Wklej"};
  pst.enabled = CanPaste();
  pst.shortcut = "Ctrl+V";
  m.push_back(pst);
  m.push_back(sep);
  MenuItem sortm;
  sortm.text = "Sortuj według";
  sortm.sub = SortItemsMenu(T().sortCol, T().sortAsc, false);
  m.push_back(sortm);
  MenuItem hid{CMD_HIDDEN, "Pokaż ukryte pliki"};
  hid.checked = showHidden_;
  hid.shortcut = "Ctrl+H";
  m.push_back(hid);
  MenuItem rf{CMD_REFRESH, "Odśwież"};
  rf.shortcut = "F5";
  m.push_back(rf);
  m.push_back(sep);
  MenuItem tm{CMD_TERMINAL, "Otwórz terminal tutaj"};
  tm.enabled = inFolder;
  m.push_back(tm);
  MenuItem pr{CMD_PROPS, "Właściwości"};
  pr.enabled = !curPath_.empty();
  m.push_back(pr);
  int cmd = ExecMenu(this, screen, m);
  if (cmd) OnCommand(cmd);
}

void App::ShowDriveMenu(int sideIdx, QPoint screen) {
  if (sideIdx < 0 || sideIdx >= (int)side_.size()) return;
  const SideEntry e = side_[sideIdx];
  if (e.path.empty()) return;
  std::vector<MenuItem> m;
  m.push_back({1, "Otwórz"});
  m.push_back({2, "Otwórz w nowej karcie"});
  if (e.type == SideEntry::Folder && e.glyph != Ic::Trash) m.push_back({3, e.pinned ? "Odepnij z Szybkiego dostępu" : "Przypnij do Szybkiego dostępu"});
  m.push_back({4, "Kopiuj ścieżkę"});
  int r = ExecMenu(this, screen, m);
  if (r == 1) Navigate(e.path, true);
  else if (r == 2) NewTab(e.path);
  else if (r == 3) PinToggle(e.path);
  else if (r == 4) ClipboardSetText(e.path);
}

void App::OnCommand(int id) {
  switch (id) {
    case CMD_NEWFOLDER: NewFolder(); break;
    case CMD_NEWFILE: NewFile(); break;
    case CMD_PASTE: Paste(); break;
    case CMD_REFRESH:
      if (thisPC_) RefreshDrivesAsync();
      else if (searchActive_) RunSearch();
      else Reload(true);
      break;
    case CMD_TERMINAL: OpenTerminal(); break;
    case CMD_PROPS: {
      auto sel = SelectedPaths();
      if (!sel.empty()) ShowProperties(sel[0]);
      else if (!curPath_.empty()) ShowProperties(curPath_);
      break;
    }
    case CMD_CUT: CopySel(true); break;
    case CMD_COPY: CopySel(false); break;
    case CMD_RENAME: if (focus_ >= 0) RenameBegin(focus_); break;
    case CMD_DELETE: DeleteSel(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier); break;
    case CMD_COPYPATH: CopyPathToClipboard(); break;
    case CMD_SORT: {
      QPoint p = ToGlobal(rCmd_.x, rCmd_.b());
      for (auto& c : cmds_)
        if (c.id == CMD_SORT) p = ToGlobal(c.rect.x, c.rect.b());
      ShowSortMenu(p);
      break;
    }
    case CMD_HIDDEN:
      showHidden_ = !showHidden_;
      SaveSettings();
      if (!searchActive_ && !thisPC_) Reload(true);
      break;
    case CMD_ASC:
    case CMD_DESC:
      T().sortAsc = id == CMD_ASC;
      SortItems();
      break;
    default:
      if (id >= CMD_SORT_BASE && id < CMD_SORT_BASE + 5) {
        int k = id - CMD_SORT_BASE;
        T().sortCol = k;
        T().sortAsc = true;
        SortItems();
      }
  }
  update();
}

// ------------------------------------------------------------------ testy / zrzuty
void App::TestShowModal(const std::wstring& which) {
  if (which == L"conflict") {
    Modal m;
    m.title = L"Plik już istnieje";
    m.lines = {L"Miejsce docelowe zawiera już plik o tej samej nazwie:", L"raport.pdf", L"Źródło:    1,2 MB   ·   12.11.2026 10:15", L"Istniejący:   980 KB   ·   03.11.2026 08:40"};
    m.btns = {L"Zamień", L"Pomiń", L"Zachowaj oba", L"Anuluj"};
    m.cancelBtn = 3;
    m.hasCheck = true;
    m.checkText = L"Zastosuj do wszystkich konfliktów";
    ShowModal(m);
  }
}

QPixmap App::Screenshot() { return grab(); }

}  // namespace nv

// Carton Explorer (Linux) - okno główne i logika aplikacji (Qt6, własne rysowanie)
#pragma once
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QTimer>
#include <QWidget>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "copyengine.h"
#include "gfx.h"
#include "index.h"
#include "shell.h"

namespace nv {

struct Item {
  std::wstring name, path, ext, dispDate;
  u32 attrs = 0;
  u64 size = 0;
  i64 mtime = 0;
  bool isDir = false, sel = false, bumped = false, isLink = false;
  u32 node = NONE;       // węzeł w indeksie (dla folderów)
  u8 sizeState = 0;      // 0 nieznany, 1 liczony (suma częściowa), 2 gotowy
  u64 folderSize = 0, fileCount = 0, dirCount = 0;
  int score = 0;
};

struct Tab {
  std::wstring path;  // L"" = Ten komputer
  std::vector<std::wstring> back, fwd;
  int sortCol = 0;
  bool sortAsc = true;
  float scroll = 0;
  std::wstring title;
  bool searching = false;
  bool searchAll = false;
  std::wstring query;
};

struct SideEntry {
  enum Type { Header, Folder, Drive, ThisPC } type = Folder;
  std::wstring label, path;
  Ic glyph = Ic::Folder;
  bool pinned = false;
  u64 total = 0, freeB = 0;
  Rf rect;
};

struct DriveInfo {
  std::wstring root, label, fs, source;
  MountEntry::Kind kind = MountEntry::Fixed;
  u64 total = 0, freeB = 0;
};

struct Modal {
  std::wstring title;
  std::vector<std::wstring> lines;
  std::vector<std::wstring> btns;
  int def = 0, cancelBtn = -1;
  bool hasCheck = false, checked = false, danger = false;
  std::wstring checkText;
  std::function<void(int, bool)> cb;
  Rf box, checkRect;
  std::vector<Rf> btnRects;
};

struct Hit {
  enum Kind {
    None, TabBtn, TabClose, TabNew, WinMin, WinMax, WinClose, Back, Fwd, Up, Refresh, Addr, Crumb, SearchBox, SearchClear,
    Cmd, Side, Splitter, HeaderCol, HeaderSep, Row, Empty, ScopeFolder, ScopeAll, JobPause, JobCancel, JobClose, JobInfo,
    ModalBtn, ModalCheck, DriveTile, ScrollBar, TitleDrag
  } kind = None;
  int idx = -1;
};

struct CmdBtn {
  int id;
  Ic glyph;
  std::wstring label;
  Rf rect;
  bool enabled = true;
  bool sepBefore = false;
};

struct LoadOut {
  u64 gen = 0;
  std::wstring path;
  std::vector<Item> items;
  u32 dirNode = NONE;
  u32 err = 0;
  bool keepSel = false;
};

struct SearchOut {
  u64 gen = 0;
  std::vector<Item> items;
  double ms = 0;
  bool truncated = false, complete = false;
  std::wstring error;
};

struct ConflictReq {
  ConflictInfo ci;
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  Resolution res = Resolution::Skip;
  bool all = false;
};

double NowSec();

class App : public QWidget {
 public:
  App();
  ~App() override;
  bool Create(const std::wstring& startPath);
  void Shutdown();
  QPixmap Screenshot();  // do testów

  // ---- dostęp dla testów automatycznych (zrzuty ekranu)
  void TestNavigate(const std::wstring& p) { Navigate(p, true); }
  void TestSearch(const QString& q);
  void TestWaitIdle(int ms);
  void TestStartCopy(const std::vector<std::wstring>& src, const std::wstring& dest, bool move) { StartJob(move ? JobOp::Move : JobOp::Copy, src, dest); }
  void TestSelectAll() { for (auto& it : items_) it.sel = true; update(); }
  void TestShowModal(const std::wstring& which);
  JobManager& Jobs() { return jobs_; }
  FileIndex& Index() { return idx_; }
  size_t ItemCount() const { return items_.size(); }
  void TestSelectFirstN(int n) { for (int i = 0; i < n && i < (int)items_.size(); i++) items_[i].sel = true; update(); }

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void leaveEvent(QEvent*) override;
  void closeEvent(QCloseEvent*) override;
  void changeEvent(QEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragMoveEvent(QDragMoveEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;
  bool eventFilter(QObject*, QEvent*) override;
  bool focusNextPrevChild(bool) override { return false; }
  void focusInEvent(QFocusEvent*) override { update(); }
  void focusOutEvent(QFocusEvent*) override { update(); }

 private:
  void Post(std::function<void()> f);
  void InitUI();
  QPoint ToGlobal(float x, float y) const { return mapToGlobal(QPoint((int)x, (int)y)); }
  float W_ = 0, H_ = 0;  // rozmiar w px logicznych
  bool maximized_ = false;
  bool closing_ = false;
  Gfx g_;
  QTimer timer_, dirTimer_;

  // ---------- ustawienia
  void LoadSettings();
  void SaveSettings();
  std::wstring SettingsPath();
  std::vector<std::wstring> pinned_;
  bool showHidden_ = false;
  float sideW_ = 236;
  std::wstring startPath_, cmdPath_;
  unsigned tick_ = 0;
  int deferSingle_ = -1;

  // ---------- zakładki / nawigacja
  std::vector<Tab> tabs_;
  int cur_ = 0;
  Tab& T() { return tabs_[cur_]; }
  const Tab& T() const { return tabs_[cur_]; }
  void NewTab(const std::wstring& path);
  void CloseTab(int i);
  void SwitchTab(int i);
  void Navigate(const std::wstring& path, bool pushHistory = true);
  void GoBack();
  void GoFwd();
  void GoUp();
  void Reload(bool keepSel);
  void LoadAsync(const std::wstring& path, bool keepSel);
  void OnLoaded(LoadOut* o);
  void UpdateTitle();
  std::vector<Item> items_;
  bool thisPC_ = false;
  std::wstring curPath_;
  u32 dirNode_ = NONE;
  std::atomic<u64> loadGen_{0};
  std::atomic<int> loaders_{0};
  bool loading_ = false;
  double loadStart_ = 0;
  std::wstring loadError_;
  std::vector<std::wstring> pendingSelect_;
  DirWatcher watcher_;

  // ---------- sortowanie / rozmiary
  void SortItems();
  static void SortVec(std::vector<Item>& v, int col, bool asc, bool searchMode);
  void RefreshFolderSizes(bool allowResort);
  bool sizesPending_ = false;
  double lastResort_ = 0;

  // ---------- indeks / wyszukiwanie
  FileIndex idx_;
  void StartIndex();
  void OnSearchTextChanged();
  void RunSearch();
  void SearchThread();
  void ExitSearch(bool clear);
  void FocusSearch();
  std::mutex smx_;
  std::condition_variable scv_;
  bool sHas_ = false;
  std::wstring sText_;
  u32 sScope_ = NONE;
  u64 sReqGen_ = 0;
  std::atomic<u64> sgen_{0};
  std::thread sth_;
  bool sStop_ = false;
  double searchMs_ = 0;
  bool searchTrunc_ = false, searchComplete_ = false;
  std::wstring searchErr_;
  bool searchActive_ = false;  // wyniki wyszukiwania wyświetlane na liście
  double lastIndexUi_ = 0;

  // ---------- dyski / pasek boczny
  std::vector<DriveInfo> drives_;
  std::vector<SideEntry> side_;
  void BuildSidebar();
  void RefreshDrivesAsync();
  std::wstring DriveLabel(const DriveInfo& d) const;
  Ic DriveIcon(const DriveInfo& d) const;
  int mountFd_ = -1;

  // ---------- operacje
  void OpenItem(int i);
  void OpenPath(const std::wstring& p);
  void CopySel(bool cut);
  void Paste();
  void DeleteSel(bool permanent);
  void TrashAsync(std::vector<std::wstring> paths);
  void RenameBegin(int i);
  void RenameCommit(bool apply);
  void NewFolder();
  void NewFile();
  void StartJob(JobOp op, std::vector<std::wstring> src, const std::wstring& dest);
  void DoDrop(const std::vector<std::wstring>& paths, const std::wstring& dest, Qt::DropAction act);
  void CopyPathToClipboard();
  void OpenTerminal();
  void PinToggle(const std::wstring& path);
  void ShowProperties(const std::wstring& path);
  std::vector<std::wstring> SelectedPaths() const;
  int SelCount() const;
  u64 SelSize() const;
  void ClearSel();
  void SelectOnly(int i);
  void SelectRange(int a, int b, bool additive);
  void EnsureVisible(int i);
  bool CanPaste() const;
  bool clipHasFiles_ = false;
  std::set<std::wstring> cutSet_;
  bool ownClip_ = false;
  std::wstring renameAfterLoad_;
  float restoreScroll_ = -1;
  std::wstring lastDest_;
  u64 lastDestSerial_ = 0, dragSrcSerial_ = 0;
  std::vector<Rf> tileRects_;
  void LayoutTiles();

  // ---------- zadania kopiowania
  JobManager jobs_;
  void OnJobEvent(u32 id);
  Resolution JobConflict(const ConflictInfo&, bool& all);
  void PumpModals();
  std::mutex cmx_;
  std::deque<ConflictReq*> creqs_;
  std::map<u32, double> dismissAt_;
  bool launcherOn_ = false;

  // ---------- modale
  std::deque<Modal> modals_;
  bool modalActive_ = false;
  void ShowModal(Modal m);
  void CloseModal(int btn);
  void Alert(const std::wstring& title, const std::wstring& text);
  void LayoutModal();
  void ShowJobErrors(CopyJob& j);

  // ---------- układ
  Rf rTitle_, rToolbar_, rCmd_, rSide_, rMain_, rHeader_, rList_, rStatus_, rSearchBar_;
  Rf rMin_, rMax_, rClose_, rBack_, rFwd_, rUp_, rRefresh_, rAddr_, rSearch_, rSearchClear_, rScopeFolder_, rScopeAll_;
  Rf rSplit_;
  std::vector<Rf> tabRects_, tabClose_;
  Rf rNewTab_;
  std::vector<CmdBtn> cmds_;
  struct Crumb { Rf r; std::wstring path; };
  std::vector<Crumb> crumbs_;
  std::vector<std::wstring> crumbLabels_;
  float colX_[5] = {0, 0, 0, 0, 0};  // lewe krawędzie kolumn + prawy brzeg
  float colW_[4] = {0, 150, 140, 110};
  float scolW_[4] = {0, 260, 150, 110};
  Rf rThumb_;
  float rowH_ = 32;
  void Layout();
  void LayoutCrumbs();
  void LayoutCols();
  bool InSearchView() const { return searchActive_; }

  // ---------- rysowanie
  void Paint();
  void PaintTitle();
  void PaintToolbar();
  void PaintCmdBar();
  void PaintSidebar();
  void PaintMain();
  void PaintThisPC();
  void PaintList();
  void PaintStatus();
  void PaintOverlays();
  void PaintJobCards();
  void PaintModal();
  void DrawIconBtn(const Rf& r, Ic glyph, bool hot, bool down, bool enabled = true);
  std::wstring TypeName(const Item& it);
  std::wstring SizeText(const Item& it);
  std::unordered_map<std::wstring, std::wstring> typeCache_;
  std::vector<u32> toBump_;
  std::vector<std::pair<Rf, std::pair<Hit::Kind, int>>> jobHits_;

  // ---------- wejście
  Hit HitTest(float x, float y);
  void OnMouseMove(float x, float y, Qt::MouseButtons b);
  void OnLButtonDown(float x, float y, Qt::KeyboardModifiers m);
  void OnLButtonUp(float x, float y);
  void OnLButtonDbl(float x, float y);
  void OnRButtonUp(float x, float y);
  void OnWheel(float x, float y, float dy, bool pixels, bool ctrl);
  bool OnKey(QKeyEvent* e);
  void OnChar(const QString& t);
  void OnTimer();
  void OnCommand(int id);
  void UpdateCursor();
  void BeginDrag();
  void ShowBackgroundMenu(QPoint screen);
  void ShowRowMenu(QPoint screen);
  void ShowSortMenu(QPoint screen);
  void ShowDriveMenu(int sideIdx, QPoint screen);
  void Rubber(float y);
  float ScrollMax() const;
  void SetScroll(float s, bool animate);
  Hit hover_, down_;
  float mx_ = 0, my_ = 0, downX_ = 0, downY_ = 0;
  bool dragCand_ = false, banding_ = false, resizingCol_ = false, resizingSide_ = false, draggingScroll_ = false;
  int resizeCol_ = -1;
  float bandStartY_ = 0, scrollTarget_ = 0, scroll_ = 0, scrollDragOff_ = 0;
  int anchor_ = -1, focus_ = -1;
  Qt::Edges edgeHover_;
  std::wstring typeAhead_;
  double typeAheadAt_ = 0;
  // drag&drop (cel)
  int dropRow_ = -1, dropSide_ = -1, dropCrumb_ = -1;
  bool dropActive_ = false;
  std::vector<std::wstring> dragPaths_;
  Qt::DropAction DragOverImpl(QPoint client, Qt::KeyboardModifiers mods, Qt::DropActions allowed);
  std::wstring DropTargetFor(QPoint client, int* row, int* side, int* crumb);
  float dragAutoScroll_ = 0;
  bool dragFromSelf_ = false;

  // ---------- edycja
  QLineEdit *hSearch_ = nullptr, *hAddr_ = nullptr, *hRename_ = nullptr;
  int renameIdx_ = -1;
  bool addrEditing_ = false;
  void ShowAddrEdit(bool show);
  void PositionEdits();
};

}  // namespace nv

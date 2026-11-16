#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>

#include "app.h"

namespace nv {

enum {
  CMD_NEWFOLDER = 1, CMD_NEWFILE, CMD_PASTE, CMD_REFRESH, CMD_TERMINAL, CMD_PROPS, CMD_CUT, CMD_COPY, CMD_RENAME, CMD_DELETE, CMD_SORT,
  CMD_HIDDEN, CMD_COPYPATH
};

static std::shared_ptr<CopyJob> FindJob(JobManager& m, int id) {
  for (auto& j : m.Jobs())
    if ((int)j->id == id) return j;
  return nullptr;
}

static const float EDGE = 6.f;

// ------------------------------------------------------------------ hit-test
Hit App::HitTest(float x, float y) {
  Hit h;
  auto set = [&](Hit::Kind k, int i = -1) { h.kind = k; h.idx = i; return h; };
  if (modalActive_ && !modals_.empty()) {
    Modal& m = modals_.front();
    for (size_t i = 0; i < m.btnRects.size(); i++)
      if (m.btnRects[i].Has(x, y)) return set(Hit::ModalBtn, (int)i);
    if (m.hasCheck && m.checkRect.Has(x, y)) return set(Hit::ModalCheck);
    return h;
  }
  for (auto& jh : jobHits_)
    if (jh.first.Has(x, y)) return set(jh.second.first, jh.second.second);
  if (y < rTitle_.h) {
    if (rClose_.Has(x, y)) return set(Hit::WinClose);
    if (rMax_.Has(x, y)) return set(Hit::WinMax);
    if (rMin_.Has(x, y)) return set(Hit::WinMin);
    for (size_t i = 0; i < tabRects_.size(); i++) {
      if (tabs_.size() > 1 && tabClose_[i].Has(x, y)) return set(Hit::TabClose, (int)i);
      if (tabRects_[i].Has(x, y)) return set(Hit::TabBtn, (int)i);
    }
    if (rNewTab_.Has(x, y)) return set(Hit::TabNew);
    return set(Hit::TitleDrag);
  }
  if (rBack_.Has(x, y)) return set(Hit::Back);
  if (rFwd_.Has(x, y)) return set(Hit::Fwd);
  if (rUp_.Has(x, y)) return set(Hit::Up);
  if (rRefresh_.Has(x, y)) return set(Hit::Refresh);
  if (!T().query.empty() && rSearchClear_.Has(x, y)) return set(Hit::SearchClear);
  if (rSearch_.Has(x, y)) return set(Hit::SearchBox);
  if (rAddr_.Has(x, y)) {
    if (!addrEditing_)
      for (size_t i = 0; i < crumbs_.size(); i++)
        if (crumbs_[i].r.Has(x, y)) return set(Hit::Crumb, (int)i);
    return set(Hit::Addr);
  }
  for (size_t i = 0; i < cmds_.size(); i++)
    if (cmds_[i].rect.Has(x, y)) return set(Hit::Cmd, (int)i);
  Rf split = rSplit_;
  split.x -= 2;
  split.w += 4;
  if (split.Has(x, y)) return set(Hit::Splitter);
  if (rSide_.Has(x, y)) {
    for (size_t i = 0; i < side_.size(); i++)
      if (side_[i].type != SideEntry::Header && side_[i].rect.Has(x, y)) return set(Hit::Side, (int)i);
    return h;
  }
  if (rMain_.Has(x, y)) {
    if (searchActive_ && rSearchBar_.Has(x, y)) {
      if (rScopeFolder_.Has(x, y)) return set(Hit::ScopeFolder);
      if (rScopeAll_.Has(x, y)) return set(Hit::ScopeAll);
      return h;
    }
    if (thisPC_) {
      for (size_t i = 0; i < tileRects_.size(); i++)
        if (tileRects_[i].Has(x, y)) return set(Hit::DriveTile, (int)i);
      return h;
    }
    if (rHeader_.Has(x, y)) {
      for (int c = 1; c < 4; c++)
        if (std::fabs(x - (colX_[c] - 8)) < 5) return set(Hit::HeaderSep, c);
      for (int c = 3; c >= 0; c--)
        if (x >= colX_[c] - 8) return set(Hit::HeaderCol, c);
      return set(Hit::HeaderCol, 0);
    }
    if (rList_.Has(x, y)) {
      if (rThumb_.w > 0 && x >= rThumb_.x - 4 && y >= rThumb_.y && y < rThumb_.b()) return set(Hit::ScrollBar);
      int row = (int)std::floor((y - rList_.y - 4 + scroll_) / rowH_);
      if (row >= 0 && row < (int)items_.size()) return set(Hit::Row, row);
      return set(Hit::Empty);
    }
  }
  return h;
}

void App::UpdateCursor() {
  Qt::CursorShape c = Qt::ArrowCursor;
  if (edgeHover_) {
    bool l = edgeHover_ & Qt::LeftEdge, r = edgeHover_ & Qt::RightEdge, t = edgeHover_ & Qt::TopEdge, b = edgeHover_ & Qt::BottomEdge;
    if ((l && t) || (r && b)) c = Qt::SizeFDiagCursor;
    else if ((r && t) || (l && b)) c = Qt::SizeBDiagCursor;
    else if (l || r) c = Qt::SizeHorCursor;
    else c = Qt::SizeVerCursor;
  } else if (hover_.kind == Hit::Splitter || hover_.kind == Hit::HeaderSep || resizingCol_ || resizingSide_)
    c = Qt::SplitHCursor;
  else {
    switch (hover_.kind) {
      case Hit::Back: case Hit::Fwd: case Hit::Up: case Hit::Refresh: case Hit::Cmd: case Hit::Side: case Hit::Crumb: case Hit::TabBtn:
      case Hit::TabNew: case Hit::TabClose: case Hit::ModalBtn: case Hit::DriveTile: case Hit::HeaderCol: case Hit::ScopeAll: case Hit::ScopeFolder:
      case Hit::JobPause: case Hit::JobCancel: case Hit::JobClose: case Hit::JobInfo: case Hit::WinMin: case Hit::WinMax: case Hit::WinClose:
      case Hit::SearchClear: case Hit::ModalCheck:
        c = Qt::PointingHandCursor;
        break;
      case Hit::Addr: case Hit::SearchBox: c = Qt::IBeamCursor; break;
      default: break;
    }
  }
  setCursor(c);
}

// ------------------------------------------------------------------ mysz
void App::Rubber(float y) {
  float y1 = std::min(bandStartY_, y), y2 = std::max(bandStartY_, y);
  int a = (int)std::floor((y1 - rList_.y - 4 + scroll_) / rowH_);
  int b = (int)std::floor((y2 - rList_.y - 4 + scroll_) / rowH_);
  bool additive = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
  if (!additive) ClearSel();
  SelectRange(a, b, true);
  if (b >= 0 && b < (int)items_.size()) focus_ = b;
}

void App::mouseMoveEvent(QMouseEvent* e) {
  float x = (float)e->position().x(), y = (float)e->position().y();
  if (!(e->buttons() & Qt::LeftButton) && !maximized_ && !modalActive_) {
    Qt::Edges ed;
    if (x < EDGE) ed |= Qt::LeftEdge;
    if (x >= W_ - EDGE) ed |= Qt::RightEdge;
    if (y < 4) ed |= Qt::TopEdge;
    if (y >= H_ - EDGE) ed |= Qt::BottomEdge;
    if (ed != edgeHover_) { edgeHover_ = ed; UpdateCursor(); }
  } else if (edgeHover_) {
    edgeHover_ = {};
  }
  OnMouseMove(x, y, e->buttons());
}

void App::OnMouseMove(float x, float y, Qt::MouseButtons b) {
  mx_ = x;
  my_ = y;
  if (resizingSide_) {
    sideW_ = std::clamp(x - 10, 170.f, 420.f);
    Layout();
    PositionEdits();
    update();
    return;
  }
  if (resizingCol_) {
    float* cw = searchActive_ ? scolW_ : colW_;
    int c = resizeCol_;
    float right = (c == 3) ? colX_[4] : colX_[c + 1];
    cw[c] = std::clamp(right - (x + 8), 60.f, 480.f);
    Layout();
    update();
    return;
  }
  if (draggingScroll_) {
    float mx = ScrollMax();
    float track = rList_.h - rThumb_.h;
    if (track > 1) SetScroll(((y - scrollDragOff_ - rList_.y) / track) * mx, false);
    return;
  }
  if (banding_) {
    Rubber(y);
    if (y < rList_.y + 10) SetScroll(scrollTarget_ - 24, false);
    else if (y > rList_.b() - 10) SetScroll(scrollTarget_ + 24, false);
    update();
    return;
  }
  if (dragCand_ && (b & Qt::LeftButton)) {
    if (std::fabs(x - downX_) > 5 || std::fabs(y - downY_) > 5) {
      deferSingle_ = -1;
      BeginDrag();
      return;
    }
  }
  Hit h = HitTest(x, y);
  if (h.kind != hover_.kind || h.idx != hover_.idx) {
    hover_ = h;
    UpdateCursor();
    update();
  }
}

void App::mousePressEvent(QMouseEvent* e) {
  float x = (float)e->position().x(), y = (float)e->position().y();
  setFocus();
  if (e->button() == Qt::BackButton) { GoBack(); return; }
  if (e->button() == Qt::ForwardButton) { GoFwd(); return; }
  if (e->button() == Qt::LeftButton) {
    if (!maximized_ && !modalActive_) {
      Qt::Edges ed;
      if (x < EDGE) ed |= Qt::LeftEdge;
      if (x >= W_ - EDGE) ed |= Qt::RightEdge;
      if (y < 4) ed |= Qt::TopEdge;
      if (y >= H_ - EDGE) ed |= Qt::BottomEdge;
      if (ed && windowHandle()) { windowHandle()->startSystemResize(ed); return; }
    }
    OnLButtonDown(x, y, e->modifiers());
  }
}

void App::mouseReleaseEvent(QMouseEvent* e) {
  float x = (float)e->position().x(), y = (float)e->position().y();
  if (e->button() == Qt::LeftButton) OnLButtonUp(x, y);
  else if (e->button() == Qt::RightButton) OnRButtonUp(x, y);
}

void App::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) OnLButtonDbl((float)e->position().x(), (float)e->position().y());
}

void App::OnLButtonDown(float x, float y, Qt::KeyboardModifiers mods) {
  Hit h = HitTest(x, y);
  down_ = h;
  downX_ = x;
  downY_ = y;
  if (modalActive_) {
    if (h.kind == Hit::ModalBtn) CloseModal(h.idx);
    else if (h.kind == Hit::ModalCheck && !modals_.empty()) {
      modals_.front().checked = !modals_.front().checked;
      update();
    }
    return;
  }
  if (renameIdx_ >= 0 && h.kind != Hit::None) RenameCommit(true);
  if (addrEditing_ && h.kind != Hit::Addr) ShowAddrEdit(false);
  bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
  switch (h.kind) {
    case Hit::TitleDrag: {
      static double lastTitleClick = 0;
      double now = NowSec();
      down_ = Hit();
      if (now - lastTitleClick < 0.4) {
        lastTitleClick = 0;
        if (isMaximized()) showNormal();
        else showMaximized();
        return;
      }
      lastTitleClick = now;
      if (windowHandle() && !maximized_) windowHandle()->startSystemMove();
      return;
    }
    case Hit::TabBtn: SwitchTab(h.idx); return;
    case Hit::TabClose: CloseTab(h.idx); down_ = Hit(); return;
    case Hit::TabNew: NewTab(curPath_); down_ = Hit(); return;
    case Hit::Splitter: resizingSide_ = true; return;
    case Hit::HeaderSep: resizingCol_ = true; resizeCol_ = h.idx; return;
    case Hit::ScrollBar: draggingScroll_ = true; scrollDragOff_ = y - rThumb_.y; return;
    case Hit::Row: {
      Item& it = items_[h.idx];
      deferSingle_ = -1;
      if (shift) {
        SelectRange(anchor_ >= 0 ? anchor_ : 0, h.idx, ctrl);
        focus_ = h.idx;
      } else if (ctrl) {
        it.sel = !it.sel;
        anchor_ = focus_ = h.idx;
      } else {
        if (!it.sel) SelectOnly(h.idx);
        else deferSingle_ = h.idx;
        anchor_ = focus_ = h.idx;
        dragCand_ = true;
      }
      update();
      return;
    }
    case Hit::Empty:
      if (!ctrl) ClearSel();
      banding_ = true;
      bandStartY_ = y;
      update();
      return;
    default: break;
  }
}

void App::OnLButtonUp(float x, float y) {
  if (modalActive_) return;
  Hit h = HitTest(x, y);
  bool same = h.kind == down_.kind && h.idx == down_.idx;
  if (resizingSide_ || resizingCol_) {
    resizingSide_ = resizingCol_ = false;
    SaveSettings();
  }
  banding_ = false;
  draggingScroll_ = false;
  if (dragCand_) {
    dragCand_ = false;
    if (deferSingle_ >= 0 && same) SelectOnly(deferSingle_);
    deferSingle_ = -1;
  }
  Hit d = down_;
  down_ = Hit();
  if (!same) {
    update();
    return;
  }
  switch (d.kind) {
    case Hit::WinClose: close(); return;
    case Hit::WinMax: if (isMaximized()) showNormal(); else showMaximized(); break;
    case Hit::WinMin: showMinimized(); break;
    case Hit::Back: GoBack(); break;
    case Hit::Fwd: GoFwd(); break;
    case Hit::Up: GoUp(); break;
    case Hit::Refresh: OnCommand(CMD_REFRESH); break;
    case Hit::Addr: ShowAddrEdit(true); break;
    case Hit::Crumb: if (d.idx < (int)crumbs_.size()) Navigate(crumbs_[d.idx].path, true); break;
    case Hit::SearchBox: FocusSearch(); break;
    case Hit::SearchClear: ExitSearch(true); break;
    case Hit::Cmd:
      if (d.idx < (int)cmds_.size() && cmds_[d.idx].enabled) OnCommand(cmds_[d.idx].id);
      break;
    case Hit::Side: {
      const SideEntry e = side_[d.idx];
      if (e.type == SideEntry::ThisPC) Navigate(L"", true);
      else if (!e.path.empty()) Navigate(e.path, true);
      break;
    }
    case Hit::DriveTile: if (d.idx < (int)drives_.size()) Navigate(drives_[d.idx].root, true); break;
    case Hit::HeaderCol: {
      static const int keysN[4] = {0, 1, 2, 3}, keysS[4] = {0, 4, 1, 3};
      int key = searchActive_ ? keysS[d.idx] : keysN[d.idx];
      if (T().sortCol == key) T().sortAsc = !T().sortAsc;
      else {
        T().sortCol = key;
        T().sortAsc = true;
      }
      SortItems();
      break;
    }
    case Hit::ScopeFolder: T().searchAll = false; RunSearch(); break;
    case Hit::ScopeAll: T().searchAll = true; RunSearch(); break;
    case Hit::JobPause: {
      auto j = FindJob(jobs_, d.idx);
      if (j) j->pause = !j->pause.load();
      break;
    }
    case Hit::JobCancel: {
      auto j = FindJob(jobs_, d.idx);
      if (j) { j->cancel = true; j->pause = false; }
      break;
    }
    case Hit::JobClose: jobs_.Dismiss((u32)d.idx); dismissAt_.erase((u32)d.idx); break;
    case Hit::JobInfo: {
      auto j = FindJob(jobs_, d.idx);
      if (j) ShowJobErrors(*j);
      break;
    }
    default: break;
  }
  update();
}

void App::OnLButtonDbl(float x, float y) {
  if (modalActive_) return;
  Hit h = HitTest(x, y);
  if (h.kind == Hit::Row) OpenItem(h.idx);
  else if (h.kind == Hit::DriveTile && h.idx < (int)drives_.size()) Navigate(drives_[h.idx].root, true);
  else if (h.kind == Hit::Splitter) {
    sideW_ = 236;
    Layout();
    update();
  } else if (h.kind == Hit::HeaderSep) {
    float* cw = searchActive_ ? scolW_ : colW_;
    static const float defN[4] = {0, 150, 140, 110}, defS[4] = {0, 260, 150, 110};
    cw[h.idx] = (searchActive_ ? defS : defN)[h.idx];
    Layout();
    update();
  } else if (h.kind == Hit::Empty || h.kind == Hit::None || h.kind == Hit::TitleDrag) {
    // nic (przełączanie maksymalizacji obsługuje pierwsze kliknięcie w pasek tytułu)
  } else
    OnLButtonDown(x, y, {});
}

void App::OnRButtonUp(float x, float y) {
  if (modalActive_) return;
  Hit h = HitTest(x, y);
  QPoint pt = ToGlobal(x, y);
  if (h.kind == Hit::Row) {
    if (!items_[h.idx].sel) SelectOnly(h.idx);
    focus_ = h.idx;
    update();
    ShowRowMenu(pt);
  } else if (h.kind == Hit::Empty || (rList_.Has(x, y) && !thisPC_)) {
    ClearSel();
    update();
    ShowBackgroundMenu(pt);
  } else if (h.kind == Hit::Side) {
    ShowDriveMenu(h.idx, pt);
  } else if (h.kind == Hit::DriveTile && h.idx < (int)drives_.size()) {
    std::wstring root = drives_[h.idx].root;
    std::vector<MenuItem> m;
    m.push_back({1, "Otwórz"});
    m.push_back({2, "Otwórz w nowej karcie"});
    m.push_back({3, "Kopiuj ścieżkę"});
    int r = ExecMenu(this, pt, m);
    if (r == 1) Navigate(root, true);
    else if (r == 2) NewTab(root);
    else if (r == 3) ClipboardSetText(root);
  } else if (thisPC_ && rMain_.Has(x, y)) {
    ShowBackgroundMenu(pt);
  }
}

void App::wheelEvent(QWheelEvent* e) {
  float x = (float)e->position().x(), y = (float)e->position().y();
  bool px = !e->pixelDelta().isNull();
  float dy = px ? (float)e->pixelDelta().y() : (float)e->angleDelta().y();
  OnWheel(x, y, dy, px, e->modifiers() & Qt::ControlModifier);
}

void App::OnWheel(float x, float y, float delta, bool pixels, bool ctrl) {
  if (modalActive_) return;
  if (renameIdx_ >= 0) RenameCommit(true);
  if (ctrl && rMain_.Has(x, y)) {
    rowH_ = std::clamp(rowH_ + (delta > 0 ? 2.f : -2.f), 26.f, 46.f);
    Layout();
    update();
    return;
  }
  if (rMain_.Has(x, y) && !thisPC_) {
    if (pixels) SetScroll(scrollTarget_ - delta, false);
    else SetScroll(scrollTarget_ - delta / 120.f * rowH_ * 3.f, true);
  }
}

// ------------------------------------------------------------------ klawiatura
void App::keyPressEvent(QKeyEvent* e) {
  if (OnKey(e)) { e->accept(); return; }
  if (!(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !e->text().isEmpty()) OnChar(e->text());
  QWidget::keyPressEvent(e);
}

bool App::OnKey(QKeyEvent* e) {
  int key = e->key();
  Qt::KeyboardModifiers mods = e->modifiers();
  if (modalActive_ && !modals_.empty()) {
    Modal& m = modals_.front();
    if (key == Qt::Key_Return || key == Qt::Key_Enter) { CloseModal(m.def); return true; }
    if (key == Qt::Key_Escape) { CloseModal(m.cancelBtn >= 0 ? m.cancelBtn : (int)m.btns.size() - 1); return true; }
    if (key == Qt::Key_Space && m.hasCheck) { m.checked = !m.checked; update(); return true; }
    return true;
  }
  bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
  int n = (int)items_.size();
  auto moveFocus = [&](int idx) {
    if (n == 0) return;
    idx = std::clamp(idx, 0, n - 1);
    if (shift) SelectRange(anchor_ >= 0 ? anchor_ : idx, idx, false);
    else if (ctrl) {
    } else {
      SelectOnly(idx);
    }
    if (!shift && ctrl) anchor_ = anchor_ >= 0 ? anchor_ : idx;
    if (shift && anchor_ < 0) anchor_ = idx;
    focus_ = idx;
    EnsureVisible(idx);
    update();
  };
  int page = std::max(1, (int)(rList_.h / rowH_) - 1);
  switch (key) {
    case Qt::Key_A: if (ctrl) { for (auto& it : items_) it.sel = true; update(); return true; } break;
    case Qt::Key_C: if (ctrl) { if (shift) CopyPathToClipboard(); else CopySel(false); return true; } break;
    case Qt::Key_X: if (ctrl) { CopySel(true); return true; } break;
    case Qt::Key_V: if (ctrl) { Paste(); return true; } break;
    case Qt::Key_N: if (ctrl && shift) { NewFolder(); return true; } break;
    case Qt::Key_H: if (ctrl) { OnCommand(CMD_HIDDEN); return true; } break;
    case Qt::Key_T: if (ctrl) { NewTab(curPath_); return true; } break;
    case Qt::Key_W: if (ctrl) { CloseTab(cur_); return true; } break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
      if (ctrl && tabs_.size() > 1) {
        bool back = shift || key == Qt::Key_Backtab;
        SwitchTab((cur_ + (back ? -1 : 1) + (int)tabs_.size()) % (int)tabs_.size());
        return true;
      }
      break;
    case Qt::Key_L: if (ctrl) { ShowAddrEdit(true); return true; } break;
    case Qt::Key_D: if (alt) { ShowAddrEdit(true); return true; } break;
    case Qt::Key_F:
    case Qt::Key_E: if (ctrl) { FocusSearch(); return true; } break;
    case Qt::Key_F3: FocusSearch(); return true;
    case Qt::Key_F2: if (focus_ >= 0 && SelCount() >= 1) { RenameBegin(focus_); return true; } break;
    case Qt::Key_F5: OnCommand(CMD_REFRESH); return true;
    case Qt::Key_Delete: DeleteSel(shift); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (alt) { OnCommand(CMD_PROPS); return true; }
      if (focus_ >= 0 && focus_ < n) { OpenItem(focus_); return true; }
      break;
    case Qt::Key_Backspace: GoUp(); return true;
    case Qt::Key_Left: if (alt) { GoBack(); return true; } break;
    case Qt::Key_Right: if (alt) { GoFwd(); return true; } break;
    case Qt::Key_Up: if (alt) { GoUp(); return true; } moveFocus(focus_ < 0 ? n - 1 : focus_ - 1); return true;
    case Qt::Key_Down: moveFocus(focus_ < 0 ? 0 : focus_ + 1); return true;
    case Qt::Key_Home: moveFocus(0); return true;
    case Qt::Key_End: moveFocus(n - 1); return true;
    case Qt::Key_PageUp: moveFocus(focus_ - page); return true;
    case Qt::Key_PageDown: moveFocus(focus_ + page); return true;
    case Qt::Key_Escape:
      if (searchActive_) ExitSearch(true);
      else { ClearSel(); update(); }
      return true;
    case Qt::Key_Menu: {
      if (focus_ >= 0) {
        QPoint p = ToGlobal(rList_.x + 200, rList_.y + 4 + focus_ * rowH_ - scroll_ + rowH_);
        if (SelCount() == 0) SelectOnly(focus_);
        ShowRowMenu(p);
      }
      return true;
    }
    default: break;
  }
  return false;
}

void App::OnChar(const QString& t) {
  if (modalActive_ || thisPC_ || t.isEmpty() || t[0].unicode() < 32) return;
  double now = NowSec();
  if (now - typeAheadAt_ > 0.9) typeAhead_.clear();
  typeAheadAt_ = now;
  typeAhead_ += t.toStdWString();
  int n = (int)items_.size();
  if (n == 0) return;
  std::wstring pre = FoldStr(typeAhead_);
  bool single = pre.size() > 1 && std::all_of(pre.begin(), pre.end(), [&](wchar_t x) { return x == pre[0]; });
  int start = (focus_ >= 0 && (typeAhead_.size() == 1 || single)) ? focus_ + 1 : 0;
  std::wstring key = single ? pre.substr(0, 1) : pre;
  for (int k = 0; k < n; k++) {
    int i = (start + k) % n;
    std::wstring nm = FoldStr(items_[i].name);
    if (nm.compare(0, key.size(), key) == 0) {
      SelectOnly(i);
      EnsureVisible(i);
      update();
      return;
    }
  }
}

// ------------------------------------------------------------------ timer
void App::OnTimer() {
  if (closing_) return;
  tick_++;
  bool inv = false;
  float d = scrollTarget_ - scroll_;
  if (std::fabs(d) > 0.3f) {
    scroll_ += d * 0.28f;
    inv = true;
  } else if (d != 0) {
    scroll_ = scrollTarget_;
    inv = true;
  }
  double now = NowSec();
  auto js = jobs_.Jobs();
  const CopyJob* active = nullptr;
  for (auto& j : js) {
    if (!j->Finished()) {
      j->Tick();
      inv = true;
      if (!active) active = j.get();
    }
    auto it = dismissAt_.find(j->id);
    if (it != dismissAt_.end() && now > it->second) {
      u32 jid = j->id;
      dismissAt_.erase(it);
      jobs_.Dismiss(jid);
      inv = true;
    }
  }
  if (active) {
    SetLauncherProgress(true, active->totalBytes.load() > 0 ? active->Fraction() : 0.0);
    launcherOn_ = true;
  } else if (launcherOn_) {
    SetLauncherProgress(false, 0);
    launcherOn_ = false;
  }
  bool indexing = !idx_.GetStatus().bgComplete;
  if ((loading_ || indexing) && tick_ % 8 == 0) inv = true;
  if (sizesPending_ && tick_ % 8 == 4) {
    RefreshFolderSizes(true);
    inv = true;
  }
  if (dropActive_ && dragAutoScroll_ != 0) {
    SetScroll(scrollTarget_ + dragAutoScroll_ * rowH_ * 0.5f, false);
    inv = true;
  }
  if (thisPC_ && tick_ % 450 == 0) RefreshDrivesAsync();
  if (inv) update();
}

// ------------------------------------------------------------------ drag & drop (cel)
void App::dragEnterEvent(QDragEnterEvent* e) {
  if (!e->mimeData()->hasUrls()) return;
  dragPaths_ = PathsFromMime(e->mimeData());
  dragSrcSerial_ = dragPaths_.empty() ? 0 : VolumeSerial(PathParent(dragPaths_[0]));
  lastDest_.clear();
  dropActive_ = true;
  e->acceptProposedAction();
}

void App::dragMoveEvent(QDragMoveEvent* e) {
  Qt::DropAction a = DragOverImpl(e->position().toPoint(), e->modifiers(), e->possibleActions());
  if (a == Qt::IgnoreAction) {
    e->ignore();
    return;
  }
  e->setDropAction(a);
  e->accept();
}

void App::dragLeaveEvent(QDragLeaveEvent*) {
  dropRow_ = dropSide_ = dropCrumb_ = -1;
  dropActive_ = false;
  dragAutoScroll_ = 0;
  dragPaths_.clear();
  update();
}

void App::dropEvent(QDropEvent* e) {
  int row, side, crumb;
  std::wstring dest = DropTargetFor(e->position().toPoint(), &row, &side, &crumb);
  auto paths = PathsFromMime(e->mimeData());
  Qt::DropAction act = DragOverImpl(e->position().toPoint(), e->modifiers(), e->possibleActions());
  dropRow_ = dropSide_ = dropCrumb_ = -1;
  dropActive_ = false;
  dragAutoScroll_ = 0;
  dragPaths_.clear();
  update();
  if (!dest.empty() && !paths.empty() && act != Qt::IgnoreAction) {
    e->setDropAction(act == Qt::MoveAction && !dragFromSelf_ ? Qt::CopyAction : act);  // obce źródło nie usuwa plików po "przeniesieniu" - robimy to sami
    e->accept();
    DoDrop(paths, dest, act);
  }
}

}  // namespace nv

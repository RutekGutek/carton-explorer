#include "app.h"

#include <algorithm>
#include <cmath>



namespace nv {

enum {
  CMD_NEWFOLDER = 1, CMD_NEWFILE, CMD_PASTE, CMD_REFRESH, CMD_TERMINAL, CMD_PROPS, CMD_CUT, CMD_COPY, CMD_RENAME, CMD_DELETE, CMD_SORT,
  CMD_HIDDEN, CMD_COPYPATH
};

// ------------------------------------------------------------------ układ
void App::Layout() {
  if (W_ < 10) return;
  const float titleH = 44, toolH = 52, cmdH = 40, statH = 28, pad = 10, gap = 6;
  rTitle_ = {0, 0, W_, titleH};
  const float bw = 46, bh = 36;
  rClose_ = {W_ - bw, 0, bw, bh};
  rMax_ = {W_ - 2 * bw, 0, bw, bh};
  rMin_ = {W_ - 3 * bw, 0, bw, bh};

  size_t n = std::max<size_t>(1, tabs_.size());
  float avail = W_ - 12 - 3 * bw - 44 - 12;
  float tw = std::clamp(avail / (float)n, 96.f, 230.f);
  tabRects_.assign(n, Rf());
  tabClose_.assign(n, Rf());
  float x = 12;
  for (size_t i = 0; i < n; i++) {
    tabRects_[i] = {x, 8, tw - 4, 32};
    tabClose_[i] = {x + tw - 4 - 26, 8 + 6, 20, 20};
    x += tw;
  }
  rNewTab_ = {x + 2, 10, 28, 28};

  float ty = titleH + 4;
  rToolbar_ = {0, titleH, W_, toolH};
  rBack_ = {12, ty + 2, 34, 34};
  rFwd_ = {48, ty + 2, 34, 34};
  rUp_ = {84, ty + 2, 34, 34};
  rRefresh_ = {120, ty + 2, 34, 34};
  float searchW = std::clamp(W_ * 0.27f, 240.f, 400.f);
  rSearch_ = {W_ - 12 - searchW, ty + 2, searchW, 34};
  rSearchClear_ = {rSearch_.r() - 32, rSearch_.y + 3, 28, 28};
  rAddr_ = {162, ty + 2, rSearch_.x - 12 - 162, 34};

  // pasek poleceń
  rCmd_ = {pad, titleH + toolH, W_ - 2 * pad, cmdH};
  cmds_.clear();
  bool labels = W_ > 1150;
  auto add = [&](int id, Ic g, const wchar_t* label, bool sep = false) {
    CmdBtn c;
    c.id = id;
    c.glyph = g;
    c.label = label;
    c.sepBefore = sep;
    cmds_.push_back(c);
  };
  add(CMD_NEWFOLDER, Ic::NewFolder, L"Nowy folder");
  add(CMD_CUT, Ic::Cut, L"Wytnij", true);
  add(CMD_COPY, Ic::Copy, L"Kopiuj");
  add(CMD_PASTE, Ic::Paste, L"Wklej");
  add(CMD_RENAME, Ic::Rename, L"Zmień nazwę");
  add(CMD_DELETE, Ic::Delete, L"Usuń");
  add(CMD_SORT, Ic::Sort, L"Sortuj", true);
  add(CMD_HIDDEN, Ic::Eye, L"Ukryte");
  add(CMD_PROPS, Ic::Info, L"Właściwości");
  float cx = rCmd_.x + 4;
  for (auto& c : cmds_) {
    float lw = (labels && c.id != CMD_PROPS) || c.id == CMD_NEWFOLDER ? g_.TextW(c.label, Gfx::Small) + 6 : 0;
    if (!labels && c.id != CMD_NEWFOLDER) c.label.clear();
    if (c.sepBefore) cx += 10;
    float w = 12 + 16 + lw + 12;
    c.rect = {cx, rCmd_.y + 4, w, 32};
    cx += w + 2;
  }

  float y0 = rCmd_.b() + 4;
  rStatus_ = {pad, H_ - statH - 6, W_ - 2 * pad, statH};
  float contentH = std::max(50.f, rStatus_.y - 4 - y0);
  rSide_ = {pad, y0, sideW_, contentH};
  rSplit_ = {rSide_.r(), y0, gap, contentH};
  rMain_ = {rSide_.r() + gap, y0, std::max(100.f, W_ - pad - (rSide_.r() + gap)), contentH};

  float top = rMain_.y;
  if (searchActive_) {
    rSearchBar_ = {rMain_.x, top, rMain_.w, 42};
    top += 42;
  } else
    rSearchBar_ = {};
  rHeader_ = thisPC_ ? Rf() : Rf{rMain_.x, top, rMain_.w, 34};
  float lt = thisPC_ ? top : rHeader_.b();
  rList_ = {rMain_.x, lt, rMain_.w, rMain_.b() - lt - 4};
  rScopeFolder_ = {rSearchBar_.x + 78, rSearchBar_.y + 7, 96, 28};
  rScopeAll_ = {rScopeFolder_.r() + 4, rSearchBar_.y + 7, 118, 28};

  // pasek boczny
  float y = rSide_.y + 10;
  for (auto& e : side_) {
    float h = e.type == SideEntry::Header ? 30 : e.type == SideEntry::Drive ? 52 : 34;
    e.rect = {rSide_.x + 8, y, rSide_.w - 16, h};
    y += h + (e.type == SideEntry::Header ? 0 : 2);
    if (e.type == SideEntry::Folder && (&e == &side_.back() || (&e + 1)->type == SideEntry::ThisPC)) y += 8;
  }
  LayoutCols();
  LayoutCrumbs();
  LayoutTiles();
  float mx = ScrollMax();
  if (scrollTarget_ > mx) scrollTarget_ = mx;
  if (scroll_ > mx) scroll_ = mx;
}

void App::LayoutCols() {
  float* cw = searchActive_ ? scolW_ : colW_;
  float x0 = rMain_.x + 14, R = rMain_.r() - 16;
  colX_[4] = R;
  colX_[3] = R - cw[3];
  colX_[2] = colX_[3] - cw[2];
  colX_[1] = colX_[2] - cw[1];
  colX_[0] = x0;
  if (colX_[1] - colX_[0] < 140) {
    float over = 140 - (colX_[1] - colX_[0]);
    colX_[1] += over;
  }
}

void App::LayoutCrumbs() {
  crumbs_.clear();
  std::vector<std::pair<std::wstring, std::wstring>> parts;  // label, path
  parts.push_back({L"Ten komputer", L""});
  if (!curPath_.empty()) {
    size_t rl = RootLen(curPath_);
    std::wstring acc = curPath_.substr(0, rl);
    parts.push_back({acc, acc + L"\\"});
    for (auto& c : PathSplit(curPath_)) {
      acc = PathJoin(acc, c);
      if (acc.size() == rl) acc += L"\\";
      parts.push_back({c, acc});
    }
  }
  std::vector<float> w;
  for (auto& p : parts) w.push_back(g_.TextW(p.first, Gfx::UI) + 16);
  float availW = rAddr_.w - 24, total = 0;
  for (float x : w) total += x + 14;
  size_t start = 0;
  bool ell = false;
  while (total > availW && start + 1 < parts.size()) {
    total -= w[start] + 14;
    start++;
    ell = true;
  }
  float x = rAddr_.x + 10;
  float y = rAddr_.y + 4, h = rAddr_.h - 8;
  if (ell) {
    Rf r{x, y, 28, h};
    crumbs_.push_back({r, parts[start - 1].second});
    x += 28 + 14;
  }
  for (size_t i = start; i < parts.size(); i++) {
    Rf r{x, y, w[i], h};
    crumbs_.push_back({r, parts[i].second});
    x += w[i] + 14;
  }
  crumbLabels_.clear();
  if (ell) crumbLabels_.push_back(L"…");
  for (size_t i = start; i < parts.size(); i++) crumbLabels_.push_back(parts[i].first);
}

void App::LayoutTiles() {
  tileRects_.clear();
  if (!thisPC_) return;
  float x0 = rMain_.x + 24, y0 = rMain_.y + 64, tw = 308, th_ = 96, gap = 16;
  int cols = std::max(1, (int)((rMain_.w - 48 + gap) / (tw + gap)));
  for (size_t i = 0; i < drives_.size(); i++) {
    int cx = (int)i % cols, cy = (int)i / cols;
    tileRects_.push_back({x0 + cx * (tw + gap), y0 + cy * (th_ + gap), tw, th_});
  }
}

// ------------------------------------------------------------------ pomocnicze
std::wstring App::TypeName(const Item& it) {
  if (it.isDir) return it.isLink ? L"Skrót do folderu" : L"Folder plików";
  auto f = typeCache_.find(it.ext);
  if (f != typeCache_.end()) return f->second;
  std::wstring r = g_.TypeName(it.name, it.ext).toStdWString();
  typeCache_[it.ext] = r;
  return r;
}

std::wstring App::SizeText(const Item& it) {
  if (!it.isDir) return FormatSize(it.size);
  if (it.node == NONE) return L"—";
  if (it.sizeState == 2) return FormatSize(it.folderSize);
  if (it.folderSize == 0) return L"liczenie…";
  return L"≥ " + FormatSize(it.folderSize);
}

void App::DrawIconBtn(const Rf& r, Ic glyph, bool hot, bool down, bool enabled) {
  if (enabled && (hot || down)) g_.FillR(r, 9, down ? th::elev2 : th::elev);
  g_.Icon(glyph, r, 16, enabled ? th::text : th::text3);
}

// ------------------------------------------------------------------ główne rysowanie
void App::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.fillRect(rect(), Qt::transparent);
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  float rad = maximized_ ? 0.f : 12.f;
  p.setPen(Qt::NoPen);
  p.setBrush(th::bg);
  p.drawRoundedRect(QRectF(0, 0, W_, H_), rad, rad);
  g_.Begin(&p, (float)devicePixelRatioF());
  Paint();
  g_.End();
  if (!maximized_) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(th::line, 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, W_ - 1, H_ - 1), rad, rad);
  }
}

void App::Paint() {
  toBump_.clear();
  jobHits_.clear();
  PaintTitle();
  PaintToolbar();
  PaintCmdBar();
  PaintSidebar();
  PaintMain();
  PaintStatus();
  PaintOverlays();
  for (u32 n : toBump_) idx_.Prioritize(n);
  if (!toBump_.empty()) sizesPending_ = true;
}

void App::PaintTitle() {
  // zakładki
  for (size_t i = 0; i < tabs_.size() && i < tabRects_.size(); i++) {
    const Rf& r = tabRects_[i];
    bool active = (int)i == cur_;
    bool hot = hover_.kind == Hit::TabBtn && hover_.idx == (int)i;
    if (active) {
      g_.FillR(r, 9, th::panel2);
      g_.StrokeR(r, 9, th::line);
      g_.FillR({r.x + 14, r.b() - 3, r.w - 28, 3}, 1.5f, th::accent);
    } else if (hot)
      g_.FillR(r, 9, th::hover);
    const Tab& t = tabs_[i];
    if (t.path.empty()) g_.Icon(Ic::PC, {r.x + 8, r.y, 22, r.h}, 15, active ? th::accent2 : th::text2);
    else g_.DrawFolderIcon({r.x + 11, r.y + (r.h - 17) / 2, 17, 17});
    float tx = r.x + 34, tw2 = r.w - 34 - (tabs_.size() > 1 ? 30 : 8);
    g_.Text(t.title.empty() ? L"Nowa karta" : t.title, {tx, r.y, tw2, r.h}, active ? Gfx::UIB : Gfx::UI, active ? th::text : th::text2);
    if (tabs_.size() > 1 && (active || hot)) {
      bool ch = hover_.kind == Hit::TabClose && hover_.idx == (int)i;
      if (ch) g_.FillR(tabClose_[i], 6, th::elev2);
      g_.Icon(Ic::Close, tabClose_[i], 10, th::text2);
    }
  }
  bool nh = hover_.kind == Hit::TabNew;
  if (nh) g_.FillR(rNewTab_, 8, th::hover);
  g_.Icon(Ic::Plus, rNewTab_, 12, th::text2);

  // przyciski okna
  struct B { const Rf* r; int id; Ic g; };
  B bs[3] = {{&rMin_, 1, Ic::Minimize}, {&rMax_, 2, maximized_ ? Ic::Restore : Ic::Maximize}, {&rClose_, 3, Ic::Close}};
  for (auto& b : bs) {
    Hit::Kind hk = b.id == 1 ? Hit::WinMin : b.id == 2 ? Hit::WinMax : Hit::WinClose;
    bool hot = hover_.kind == hk, dn = down_.kind == hk && hot;
    if (hot || dn) {
      Col c = b.id == 3 ? C(0xe81123, dn ? 0.8f : 1.f) : (dn ? th::elev2 : th::elev);
      g_.FillR(b.r->Inset(3, 4), 8, c);
    }
    g_.Icon(b.g, *b.r, b.id == 2 ? 11 : 10, th::text);
  }
}

void App::PaintToolbar() {
  const Tab& t = T();
  struct N { const Rf* r; Ic g; Hit::Kind k; bool en; };
  N ns[4] = {{&rBack_, Ic::Back, Hit::Back, !t.back.empty()},
             {&rFwd_, Ic::Fwd, Hit::Fwd, !t.fwd.empty()},
             {&rUp_, Ic::Up, Hit::Up, !curPath_.empty()},
             {&rRefresh_, Ic::Refresh, Hit::Refresh, true}};
  for (auto& n : ns) DrawIconBtn(*n.r, n.g, hover_.kind == n.k, down_.kind == n.k && hover_.kind == n.k, n.en);

  // pasek adresu
  g_.FillR(rAddr_, 10, th::elev);
  if (addrEditing_) g_.StrokeR(rAddr_, 10, th::accent, 1.5f);
  else {
    for (size_t i = 0; i < crumbs_.size(); i++) {
      const Rf& r = crumbs_[i].r;
      bool hot = (hover_.kind == Hit::Crumb && hover_.idx == (int)i) || (int)i == dropCrumb_;
      if (hot) g_.FillR(r, 7, (int)i == dropCrumb_ ? A(th::accent, 0.35f) : th::elev2);
      bool last = i + 1 == crumbs_.size();
      g_.Text(crumbLabels_[i], r, last ? Gfx::UIB : Gfx::UI, last ? th::text : th::text2, Al::Center);
      if (!last) g_.DrawChevron(r.r() + 7, r.y + r.h / 2, 3.2f, th::text3, 0);
    }
  }

  // wyszukiwarka
  bool focus = hSearch_->hasFocus();
  g_.FillR(rSearch_, 10, th::elev);
  if (focus || searchActive_) g_.StrokeR(rSearch_, 10, th::accent, 1.5f);
  g_.Icon(Ic::Search, {rSearch_.x + 6, rSearch_.y, 30, rSearch_.h}, 14, focus ? th::accent : th::text2);
  if (!T().query.empty()) {
    bool ch = hover_.kind == Hit::SearchClear;
    if (ch) g_.FillR(rSearchClear_, 8, th::elev2);
    g_.Icon(Ic::Close, rSearchClear_, 10, th::text2);
  }
}

void App::PaintCmdBar() {
  bool hasSel = SelCount() > 0;
  for (size_t i = 0; i < cmds_.size(); i++) {
    CmdBtn& c = cmds_[i];
    bool en = true;
    switch (c.id) {
      case CMD_NEWFOLDER: en = !curPath_.empty() && !searchActive_; break;
      case CMD_CUT: case CMD_COPY: case CMD_DELETE: en = hasSel; break;
      case CMD_RENAME: en = SelCount() == 1; break;
      case CMD_PASTE: en = CanPaste(); break;
      case CMD_PROPS: en = !curPath_.empty() || hasSel; break;
      default: break;
    }
    c.enabled = en;
    if (c.sepBefore) g_.Line(c.rect.x - 6, c.rect.y + 6, c.rect.x - 6, c.rect.b() - 6, th::line);
    bool hot = hover_.kind == Hit::Cmd && hover_.idx == (int)i && en;
    bool down = down_.kind == Hit::Cmd && down_.idx == (int)i && hot;
    if (hot) g_.FillR(c.rect, 9, down ? th::elev2 : th::hover);
    Col ic = !en ? th::text3 : (c.id == CMD_HIDDEN && showHidden_) ? th::accent : (c.id == CMD_DELETE && hot) ? th::danger : th::text;
    g_.Icon(c.glyph, {c.rect.x + 8, c.rect.y, 24, c.rect.h}, 15, ic);
    if (!c.label.empty()) g_.Text(c.label, {c.rect.x + 36, c.rect.y, c.rect.w - 40, c.rect.h}, Gfx::Small, en ? th::text : th::text3);
  }
}

void App::PaintSidebar() {
  g_.FillR(rSide_, 14, th::panel);
  g_.Clip(rSide_.Inset(1));
  for (size_t i = 0; i < side_.size(); i++) {
    SideEntry& e = side_[i];
    if (e.rect.b() > rSide_.b()) break;
    if (e.type == SideEntry::Header) {
      g_.Text(e.label, {e.rect.x + 10, e.rect.y, e.rect.w, e.rect.h}, Gfx::SmallB, th::text3);
      continue;
    }
    bool cur = e.type == SideEntry::ThisPC ? (thisPC_ && !searchActive_) : (!e.path.empty() && PathNormalize(e.path) == curPath_);
    bool hot = hover_.kind == Hit::Side && hover_.idx == (int)i;
    bool drop = (int)i == dropSide_;
    if (cur) {
      g_.FillR(e.rect, 9, th::sel);
      g_.FillR({e.rect.x, e.rect.y + 8, 3, e.rect.h - 16}, 1.5f, th::accent);
    } else if (hot)
      g_.FillR(e.rect, 9, th::hover);
    if (drop) {
      g_.FillR(e.rect, 9, A(th::accent, 0.22f));
      g_.StrokeR(e.rect, 9, th::accent, 1.5f);
    }
    Col gc = e.type == SideEntry::Drive ? th::accent2 : e.pinned ? th::warn : e.type == SideEntry::ThisPC ? th::accent2 : th::folderFront;
    float ty = e.rect.y;
    float th2 = e.type == SideEntry::Drive ? 30.f : e.rect.h;
    g_.Icon(e.glyph, {e.rect.x + 8, ty, 28, th2}, 16, gc);
    g_.Text(e.label, {e.rect.x + 42, ty, e.rect.w - 50, th2}, cur ? Gfx::UIB : Gfx::UI, cur ? th::text : th::text2);
    if (e.type == SideEntry::Drive && e.total > 0) {
      float used = 1.f - (float)((double)e.freeB / (double)e.total);
      Rf bar{e.rect.x + 42, e.rect.y + 33, e.rect.w - 54, 5};
      g_.FillR(bar, 2.5f, th::elev);
      Rf fill = bar;
      fill.w = std::max(5.f, bar.w * used);
      if (used > 0.9f) g_.FillR(fill, 2.5f, th::danger);
      else g_.FillGrad(fill, 2.5f, th::accent, th::accent2);
    }
  }
  g_.Unclip();
}

void App::PaintMain() {
  g_.FillR(rMain_, 14, th::panel);
  if (thisPC_) {
    PaintThisPC();
    return;
  }
  if (searchActive_) {
    // pasek wyszukiwania
    g_.Text(L"Szukaj w:", {rSearchBar_.x + 14, rSearchBar_.y, 64, rSearchBar_.h}, Gfx::Small, th::text3);
    bool all = T().searchAll || curPath_.empty();
    auto chip = [&](const Rf& r, const wchar_t* label, bool on, bool hot) {
      g_.FillR(r, 14, on ? A(th::accent, 0.30f) : hot ? th::hover : th::elev);
      if (on) g_.StrokeR(r, 14, th::accent, 1.f);
      g_.Text(label, r, on ? Gfx::SmallB : Gfx::Small, on ? th::text : th::text2, Al::Center);
    };
    chip(rScopeFolder_, L"Ten folder", !all, hover_.kind == Hit::ScopeFolder);
    chip(rScopeAll_, L"Cały komputer", all, hover_.kind == Hit::ScopeAll);
    std::wstring info;
    if (!searchErr_.empty()) info = searchErr_;
    else {
      info = FormatCount(items_.size()) + L" wyników";
      wchar_t ms[32];
      swprintf(ms, 32, L" · %.1f ms", searchMs_);
      info += ms;
      if (searchTrunc_) info += L" · pokazano pierwsze 200 000";
      if (!searchComplete_) info += L" · trwa indeksowanie, wyniki mogą być niepełne";
    }
    g_.Text(info, {rScopeAll_.r() + 14, rSearchBar_.y, rSearchBar_.r() - rScopeAll_.r() - 24, rSearchBar_.h}, Gfx::Small,
            searchErr_.empty() ? th::text2 : th::danger);
    g_.Line(rSearchBar_.x + 10, rSearchBar_.b() - 1, rSearchBar_.r() - 10, rSearchBar_.b() - 1, th::line);
  }

  // nagłówek kolumn
  const wchar_t* normal[4] = {L"Nazwa", L"Data modyfikacji", L"Typ", L"Rozmiar"};
  const wchar_t* srch[4] = {L"Nazwa", L"Lokalizacja", L"Data modyfikacji", L"Rozmiar"};
  int keysN[4] = {0, 1, 2, 3}, keysS[4] = {0, 4, 1, 3};
  for (int c = 0; c < 4; c++) {
    float x = colX_[c], w = (c == 3 ? colX_[4] : colX_[c + 1]) - x;
    bool hot = hover_.kind == Hit::HeaderCol && hover_.idx == c;
    Rf r{x - 6, rHeader_.y + 3, w + 2, rHeader_.h - 6};
    if (hot) g_.FillR(r, 7, th::hover);
    int key = searchActive_ ? keysS[c] : keysN[c];
    bool sorted = T().sortCol == key;
    const wchar_t* title = searchActive_ ? srch[c] : normal[c];
    g_.Text(title, {x, rHeader_.y, w - 18, rHeader_.h}, sorted ? Gfx::SmallB : Gfx::Small, sorted ? th::text : th::text2, c == 3 ? Al::Right : Al::Left);
    if (sorted) {
      float ax = c == 3 ? x + 2 : x + g_.TextW(title, sorted ? Gfx::SmallB : Gfx::Small) + 10;
      g_.DrawChevron(ax, rHeader_.y + rHeader_.h / 2, 2.6f, th::accent, T().sortAsc ? 3 : 1, 1.6f);
    }
    if (c > 0) g_.Line(x - 8, rHeader_.y + 8, x - 8, rHeader_.b() - 8, th::line);
  }
  g_.Line(rHeader_.x + 10, rHeader_.b() - 1, rHeader_.r() - 10, rHeader_.b() - 1, th::line);
  PaintList();
}

void App::PaintList() {
  g_.Clip(rList_);
  int n = (int)items_.size();
  float rowW = rList_.w - 12 - (ScrollMax() > 0 ? 8 : 0);
  if (n == 0) {
    std::wstring msg;
    if (!loadError_.empty()) msg = loadError_;
    else if (loading_ && NowSec() - loadStart_ > 0.2) {
      int dots = (int)(NowSec() * 3) % 4;
      msg = L"Ładowanie" + std::wstring(dots, L'.');
    } else if (searchActive_) msg = L"Brak wyników dla „" + T().query + L"”";
    else if (!loading_) msg = L"Ten folder jest pusty";
    g_.Text(msg, {rList_.x, rList_.y + 20, rList_.w, 60}, Gfx::UI, loadError_.empty() ? th::text3 : th::danger, Al::Center);
  }
  int first = std::max(0, (int)std::floor((scroll_ - 4) / rowH_));
  int last = std::min(n - 1, (int)std::floor((scroll_ + rList_.h) / rowH_));
  for (int i = first; i <= last; i++) {
    Item& it = items_[i];
    Rf r{rList_.x + 6, rList_.y + 4 + i * rowH_ - scroll_, rowW, rowH_ - 2};
    bool hot = hover_.kind == Hit::Row && hover_.idx == i;
    bool cut = !cutSet_.empty() && cutSet_.count(it.path);
    if (it.sel) {
      g_.FillR(r, 8, hot ? th::elev2 : th::sel);
      g_.StrokeR(r, 8, A(th::selEdge, 0.55f), 1.f);
    } else if (hot)
      g_.FillR(r, 8, th::hover);
    if (i == dropRow_) {
      g_.FillR(r, 8, A(th::accent, 0.25f));
      g_.StrokeR(r, 8, th::accent, 1.5f);
    }
    if (i == focus_ && !it.sel && hasFocus()) g_.StrokeR(r, 8, A(th::text3, 0.7f), 1.f);
    float alpha = cut ? 0.45f : 1.f;
    Rf ir{colX_[0] - 2, r.y + (r.h - 20) / 2, 20, 20};
    if (it.isDir) g_.DrawFolderIcon(ir);
    else g_.DrawFileIcon(it.name, it.ext, ir, alpha);
    if (it.isLink) {
      g_.Circle(ir.x + 3.5f, ir.b() - 3.5f, 4.6f, th::accent2);
      g_.Icon(Ic::Fwd, {ir.x - 1, ir.b() - 8, 9, 9}, 6.5f, C(0xffffff));
    }
    float* cw = searchActive_ ? scolW_ : colW_;
    (void)cw;
    Col tc = cut ? th::text3 : th::text;
    g_.Text(it.name, {colX_[0] + 30, r.y, colX_[1] - colX_[0] - 38, r.h}, Gfx::UI, tc);
    if (searchActive_) {
      g_.Text(PathParent(it.path), {colX_[1], r.y, colX_[2] - colX_[1] - 10, r.h}, Gfx::Small, th::text3);
      g_.Text(it.dispDate, {colX_[2], r.y, colX_[3] - colX_[2] - 8, r.h}, Gfx::Small, th::text2);
    } else {
      g_.Text(it.dispDate, {colX_[1], r.y, colX_[2] - colX_[1] - 8, r.h}, Gfx::Small, th::text2);
      g_.Text(TypeName(it), {colX_[2], r.y, colX_[3] - colX_[2] - 8, r.h}, Gfx::Small, th::text2);
    }
    bool partial = it.isDir && it.sizeState != 2;
    g_.Text(SizeText(it), {colX_[3], r.y, colX_[4] - colX_[3], r.h}, Gfx::Small, it.node == NONE && it.isDir ? th::text3 : partial ? th::accent2 : th::text2, Al::Right);
    // folder do przeliczenia: poproś indeks o priorytet
    if (it.isDir && it.node != NONE && it.sizeState != 2 && !it.bumped) {
      it.bumped = true;
      toBump_.push_back(it.node);
    }
  }
  if (banding_) {
    float y1 = std::min(bandStartY_, my_), y2 = std::max(bandStartY_, my_);
    Rf b{rList_.x + 6, std::max(rList_.y, y1), rowW, std::min(rList_.b(), y2) - std::max(rList_.y, y1)};
    g_.FillR(b, 4, A(th::accent, 0.16f));
    g_.StrokeR(b, 4, A(th::accent, 0.55f), 1.f);
  }
  // pasek przewijania
  float mx = ScrollMax();
  rThumb_ = Rf();
  if (mx > 0) {
    float content = n * rowH_ + 8, vis = rList_.h;
    float th2 = std::max(36.f, vis * vis / content);
    float ty = rList_.y + (vis - th2) * (scroll_ / mx);
    rThumb_ = {rList_.r() - 12, ty, 8, th2};
    bool hot = hover_.kind == Hit::ScrollBar || draggingScroll_;
    g_.FillR(rThumb_, 4, A(th::text3, hot ? 0.9f : 0.45f));
  }
  g_.Unclip();
}

void App::PaintThisPC() {
  g_.Text(L"Dyski i urządzenia", {rMain_.x + 24, rMain_.y + 14, 400, 40}, Gfx::Title, th::text);
  for (size_t i = 0; i < tileRects_.size() && i < drives_.size(); i++) {
    const Rf& r = tileRects_[i];
    const DriveInfo& d = drives_[i];
    bool hot = hover_.kind == Hit::DriveTile && hover_.idx == (int)i;
    bool drop = (int)i == dropRow_;
    g_.FillR(r, 14, hot ? th::elev2 : th::elev);
    if (drop) g_.StrokeR(r, 14, th::accent, 2.f);
    else if (hot) g_.StrokeR(r, 14, A(th::accent, 0.5f), 1.f);
    g_.Icon(DriveIcon(d), {r.x + 14, r.y + 12, 48, 48}, 34, th::accent2);
    g_.Text(DriveLabel(d), {r.x + 70, r.y + 12, r.w - 84, 26}, Gfx::UIB, th::text);
    if (d.total > 0) {
      float used = 1.f - (float)((double)d.freeB / (double)d.total);
      Rf bar{r.x + 70, r.y + 46, r.w - 86, 8};
      g_.FillR(bar, 4, th::bg);
      Rf fill = bar;
      fill.w = std::max(8.f, bar.w * used);
      if (used > 0.9f) g_.FillR(fill, 4, th::danger);
      else g_.FillGrad(fill, 4, th::accent, th::accent2);
      g_.Text(FormatSize(d.freeB) + L" wolne z " + FormatSize(d.total), {r.x + 70, r.y + 58, r.w - 84, 26}, Gfx::Small, th::text2);
    } else
      g_.Text(d.fs.empty() ? L"Brak dysku" : d.fs, {r.x + 70, r.y + 46, r.w - 84, 26}, Gfx::Small, th::text3);
  }
  if (drives_.empty()) g_.Text(L"Wczytywanie…", {rMain_.x + 24, rMain_.y + 70, 300, 30}, Gfx::UI, th::text3);
}

void App::PaintStatus() {
  std::wstring left;
  if (thisPC_) left = std::to_wstring(drives_.size()) + L" dysków";
  else if (searchActive_) left = FormatCount(items_.size()) + L" wyników";
  else left = FormatCount(items_.size()) + L" elementów";
  int sc = SelCount();
  if (sc > 0) {
    left += L"   ·   zaznaczono " + std::to_wstring(sc);
    u64 sz = SelSize();
    if (sz) left += L" (" + FormatSize(sz) + L")";
  }
  g_.Text(left, {rStatus_.x + 6, rStatus_.y, rStatus_.w * 0.6f, rStatus_.h}, Gfx::Small, th::text2);

  IndexStatus st = idx_.GetStatus();
  std::wstring right;
  Col dot = th::ok;
  if (st.bgComplete) right = L"Indeks gotowy · " + FormatCount(st.nodes) + L" pozycji";
  else {
    int dots = (int)(NowSec() * 2) % 4;
    right = L"Indeksowanie" + std::wstring(dots, L'.') + L" " + FormatCount(st.nodes) + L" pozycji";
    dot = th::warn;
  }
  float tw = g_.TextW(right, Gfx::Small);
  g_.Text(right, {rStatus_.r() - tw - 22, rStatus_.y, tw + 16, rStatus_.h}, Gfx::Small, th::text2, Al::Right);
  g_.Circle(rStatus_.r() - tw - 30, rStatus_.y + rStatus_.h / 2, 3.5f, dot);
}

// ------------------------------------------------------------------ nakładki: zadania + modale
void App::PaintOverlays() {
  PaintJobCards();
  if (modalActive_) PaintModal();
}

void App::PaintJobCards() {
  auto jobs = jobs_.Jobs();
  float y = rStatus_.y - 8;
  const float cw = 392;
  for (size_t k = jobs.size(); k-- > 0;) {
    auto& j = jobs[k];
    bool fin = j->Finished();
    JobState st = j->State();
    auto errs = fin ? j->Errors() : std::vector<JobError>();
    float ch = fin ? (st == JobState::DoneErrors ? 92.f : 60.f) : 112.f;
    Rf card{W_ - 16 - cw, y - ch, cw, ch};
    y -= ch + 10;
    g_.Shadow(card, 14, 18, 1.f);
    g_.FillR(card, 14, th::panel2);
    g_.StrokeR(card, 14, th::line);

    std::wstring verb = j->op == JobOp::Copy ? L"Kopiowanie" : j->op == JobOp::Move ? L"Przenoszenie" : L"Usuwanie";
    u32 total = j->totalFiles.load();
    std::wstring dest = j->op == JobOp::Delete ? L"" : PathName(j->destDir).empty() ? j->destDir : PathName(j->destDir);
    std::wstring title = verb + L" " + FormatCount(total) + (total == 1 ? L" elementu" : L" elementów");
    if (!dest.empty()) title += L" → " + dest;

    Rf close{card.r() - 34, card.y + 8, 26, 26};
    if (fin) {
      Col ic = st == JobState::Done ? th::ok : st == JobState::Cancelled ? th::text2 : th::warn;
      Ic gl = st == JobState::Done ? Ic::Check : st == JobState::Cancelled ? Ic::Close : Ic::Warn;
      g_.Icon(gl, {card.x + 14, card.y + 10, 24, 24}, 16, ic);
      std::wstring t = st == JobState::Done ? verb + L" zakończone" : st == JobState::Cancelled ? verb + L" przerwane" : verb + L" zakończone z błędami (" + std::to_wstring(errs.size()) + L")";
      g_.Text(t, {card.x + 46, card.y + 8, card.w - 90, 28}, Gfx::UIB, th::text);
      std::wstring sub = FormatSize(j->doneBytes.load()) + L" · " + FormatCount(j->doneFiles.load()) + L" plików";
      if (j->skipped.load()) sub += L" · pominięto " + std::to_wstring(j->skipped.load());
      g_.Text(sub, {card.x + 46, card.y + 32, card.w - 60, 22}, Gfx::Small, th::text3);
      bool ch = hover_.kind == Hit::JobClose && hover_.idx == (int)j->id;
      if (ch) g_.FillR(close, 7, th::elev2);
      g_.Icon(Ic::Close, close, 10, th::text2);
      jobHits_.push_back({close, {Hit::JobClose, (int)j->id}});
      if (st == JobState::DoneErrors) {
        Rf b{card.x + 14, card.y + 58, 120, 26};
        bool hot = hover_.kind == Hit::JobInfo && hover_.idx == (int)j->id;
        g_.FillR(b, 8, hot ? th::elev2 : th::elev);
        g_.Text(L"Pokaż szczegóły", b, Gfx::Small, th::text, Al::Center);
        jobHits_.push_back({b, {Hit::JobInfo, (int)j->id}});
      }
      continue;
    }
    g_.Text(title, {card.x + 16, card.y + 8, card.w - 100, 26}, Gfx::UIB, th::text);
    // przyciski pauza / anuluj
    Rf pause{card.r() - 66, card.y + 8, 26, 26}, cancel{card.r() - 36, card.y + 8, 26, 26};
    bool hp = hover_.kind == Hit::JobPause && hover_.idx == (int)j->id, hc = hover_.kind == Hit::JobCancel && hover_.idx == (int)j->id;
    if (hp) g_.FillR(pause, 7, th::elev2);
    if (hc) g_.FillR(cancel, 7, th::elev2);
    g_.Icon(j->pause.load() ? Ic::Play : Ic::Pause, pause, 11, th::text);
    g_.Icon(Ic::Close, cancel, 12, hc ? th::danger : th::text);
    jobHits_.push_back({pause, {Hit::JobPause, (int)j->id}});
    jobHits_.push_back({cancel, {Hit::JobCancel, (int)j->id}});
    // pasek
    Rf bar{card.x + 16, card.y + 44, card.w - 32, 8};
    g_.FillR(bar, 4, th::bg);
    bool scanning = st == JobState::Scanning;
    if (scanning) {
      float t = (float)std::fmod(NowSec() * 0.9, 1.0);
      float sw = bar.w * 0.28f;
      Rf seg{bar.x + (bar.w + sw) * t - sw, bar.y, sw, bar.h};
      Rf clipped{std::max(seg.x, bar.x), seg.y, std::min(seg.r(), bar.r()) - std::max(seg.x, bar.x), seg.h};
      if (clipped.w > 0) g_.FillGrad(clipped, 4, th::accent, th::accent2);
    } else {
      Rf fill = bar;
      fill.w = std::max(8.f, bar.w * (float)j->Fraction());
      g_.FillGrad(fill, 4, th::accent, th::accent2);
    }
    std::wstring info;
    if (scanning) info = L"Przygotowywanie…";
    else {
      info = FormatSize(j->doneBytes.load()) + L" z " + FormatSize(j->totalBytes.load());
      if (j->pause.load()) info += L"  ·  wstrzymano";
      else {
        if (j->speed > 0) info += L"  ·  " + FormatSpeed(j->speed);
        double eta = j->Eta();
        if (eta >= 0) info += L"  ·  pozostało " + FormatDuration(eta);
      }
    }
    g_.Text(info, {card.x + 16, card.y + 58, card.w - 32, 22}, Gfx::Small, th::text2);
    g_.Text(j->Current(), {card.x + 16, card.y + 80, card.w - 32, 22}, Gfx::Small, th::text3);
  }
}

void App::LayoutModal() {
  if (modals_.empty()) return;
  Modal& m = modals_.front();
  float w = std::min(520.f, W_ - 40);
  float h = 24 + 34 + (float)m.lines.size() * 22 + 14 + (m.hasCheck ? 38 : 0) + 56;
  m.box = {(W_ - w) / 2, (H_ - h) / 2 - 20, w, h};
  float x = m.box.r() - 20, y = m.box.b() - 20 - 36;
  m.btnRects.assign(m.btns.size(), Rf());
  for (size_t i = m.btns.size(); i-- > 0;) {
    float bw = std::max(96.f, g_.TextW(m.btns[i], Gfx::UIB) + 36);
    x -= bw;
    m.btnRects[i] = {x, y, bw, 36};
    x -= 10;
  }
  m.checkRect = {m.box.x + 24, m.box.y + 24 + 34 + (float)m.lines.size() * 22 + 14, m.box.w - 48, 26};
}

void App::PaintModal() {
  g_.FillR({0, 0, W_, H_}, maximized_ ? 0.f : 12.f, C(0x05050f, 0.62f));
  Modal& m = modals_.front();
  g_.Shadow(m.box, 16, 30, 1.f);
  g_.FillR(m.box, 16, th::panel2);
  g_.StrokeR(m.box, 16, th::line);
  g_.Text(m.title, {m.box.x + 24, m.box.y + 20, m.box.w - 48, 34}, Gfx::Title, th::text);
  for (size_t i = 0; i < m.lines.size(); i++)
    g_.Text(m.lines[i], {m.box.x + 24, m.box.y + 24 + 34 + (float)i * 22, m.box.w - 48, 22}, Gfx::UI, i == 0 || m.lines[i][0] == L' ' ? th::text2 : th::text);
  if (m.hasCheck) {
    Rf cb{m.checkRect.x, m.checkRect.y + 4, 18, 18};
    bool hot = hover_.kind == Hit::ModalCheck;
    g_.FillR(cb, 5, m.checked ? th::accent : hot ? th::elev2 : th::elev);
    if (!m.checked) g_.StrokeR(cb, 5, th::text3, 1.f);
    if (m.checked) g_.DrawCheck(cb.x + 9, cb.y + 9, 4.5f, C(0xffffff));
    g_.Text(m.checkText, {cb.r() + 10, m.checkRect.y, m.checkRect.w - 30, m.checkRect.h}, Gfx::UI, th::text2);
  }
  for (size_t i = 0; i < m.btns.size(); i++) {
    const Rf& r = m.btnRects[i];
    bool hot = hover_.kind == Hit::ModalBtn && hover_.idx == (int)i;
    bool def = (int)i == m.def;
    if (def) {
      if (m.danger) g_.FillR(r, 10, hot ? C(0xff8296) : th::danger);
      else g_.FillGrad(r, 10, hot ? C(0x8d7fff) : th::accent, hot ? C(0x62b0ff) : th::accent2);
      g_.Text(m.btns[i], r, Gfx::UIB, C(0xffffff), Al::Center);
    } else {
      g_.FillR(r, 10, hot ? th::elev2 : th::elev);
      g_.Text(m.btns[i], r, Gfx::UI, th::text, Al::Center);
    }
  }
}

}  // namespace nv

#include "gfx.h"

#include <QFontDatabase>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainterPath>
#include <QLinearGradient>
#include <cmath>

namespace nv {

Gfx::Gfx() {
  QStringList pref = {"Inter", "Segoe UI Variable Text", "Segoe UI", "Noto Sans", "Cantarell", "Ubuntu", "Roboto", "Open Sans", "DejaVu Sans"};
  QStringList have = QFontDatabase::families();
  family_ = QFont().family();
  for (auto& f : pref)
    if (have.contains(f)) { family_ = f; break; }
  struct Def { int px; QFont::Weight w; bool mono; };
  Def defs[7] = {{14, QFont::Normal, false}, {14, QFont::DemiBold, false}, {12, QFont::Normal, false}, {12, QFont::DemiBold, false},
                 {15, QFont::DemiBold, false}, {22, QFont::DemiBold, false}, {12, QFont::Normal, true}};
  for (int i = 0; i < 7; i++) {
    QFont f(defs[i].mono ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : family_);
    f.setPixelSize(defs[i].px);
    f.setWeight(defs[i].w);
    f.setHintingPreference(QFont::PreferNoHinting);
    f.setStyleStrategy(QFont::PreferAntialias);
    fonts_[i] = f;
    fm_[i] = new QFontMetricsF(f);
  }
}

void Gfx::Begin(QPainter* p, float dpr) {
  p_ = p;
  dpr_ = dpr;
  clipDepth_ = 0;
  p_->setRenderHint(QPainter::Antialiasing, true);
  p_->setRenderHint(QPainter::TextAntialiasing, true);
  p_->setRenderHint(QPainter::SmoothPixmapTransform, true);
}
void Gfx::End() {
  while (clipDepth_ > 0) Unclip();
  p_ = nullptr;
}

void Gfx::Fill(const Rf& r, Col c) { p_->fillRect(r.Q(), c); }

void Gfx::FillR(const Rf& r, float rad, Col c) {
  p_->setPen(Qt::NoPen);
  p_->setBrush(c);
  p_->drawRoundedRect(r.Q(), rad, rad);
}

void Gfx::StrokeR(const Rf& r, float rad, Col c, float w) {
  p_->setBrush(Qt::NoBrush);
  p_->setPen(QPen(c, w));
  Rf q = r.Inset(w / 2);
  p_->drawRoundedRect(q.Q(), rad, rad);
}

void Gfx::FillGrad(const Rf& r, float rad, Col a, Col b, bool horizontal) {
  QLinearGradient g(r.x, r.y, horizontal ? r.r() : r.x, horizontal ? r.y : r.b());
  g.setColorAt(0, a);
  g.setColorAt(1, b);
  p_->setPen(Qt::NoPen);
  p_->setBrush(g);
  p_->drawRoundedRect(r.Q(), rad, rad);
}

void Gfx::Shadow(const Rf& r, float rad, float spread, float alpha) {
  const int N = 10;
  for (int i = N; i >= 1; i--) {
    float t = (float)i / N;
    float grow = spread * t;
    Rf q{r.x - grow, r.y - grow + spread * 0.35f, r.w + 2 * grow, r.h + 2 * grow};
    FillR(q, rad + grow, C(0x000000, alpha * (1.f - t) * 0.22f));
  }
}

void Gfx::Line(float x1, float y1, float x2, float y2, Col c, float w) {
  p_->setPen(QPen(c, w));
  p_->drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

void Gfx::Circle(float cx, float cy, float rad, Col c) {
  p_->setPen(Qt::NoPen);
  p_->setBrush(c);
  p_->drawEllipse(QPointF(cx, cy), rad, rad);
}

void Gfx::Clip(const Rf& r) {
  p_->save();
  p_->setClipRect(r.Q(), Qt::IntersectClip);
  clipDepth_++;
}
void Gfx::Unclip() {
  if (clipDepth_ <= 0) return;
  p_->restore();
  clipDepth_--;
}

void Gfx::Text(const std::wstring& s, const Rf& r, Font f, Col c, Al al) {
  if (s.empty() || r.w <= 0) return;
  QString q = QString::fromStdWString(s);
  q.replace(QChar('\n'), QChar(' '));
  QString el = fm_[f]->elidedText(q, Qt::ElideRight, r.w);
  p_->setFont(fonts_[f]);
  p_->setPen(c);
  int fl = Qt::AlignVCenter | (al == Al::Left ? Qt::AlignLeft : al == Al::Center ? Qt::AlignHCenter : Qt::AlignRight);
  p_->drawText(r.Q(), fl | Qt::TextSingleLine, el);
}

float Gfx::TextW(const std::wstring& s, Font f) { return (float)fm_[f]->horizontalAdvance(QString::fromStdWString(s)); }

// ------------------------------------------------------------------ prymitywy
void Gfx::DrawChevron(float cx, float cy, float s, Col c, int dir, float w) {
  float dx[3], dy[3];
  switch (dir) {
    case 0: dx[0] = -s / 2; dy[0] = -s; dx[1] = s / 2; dy[1] = 0; dx[2] = -s / 2; dy[2] = s; break;
    case 1: dx[0] = -s; dy[0] = -s / 2; dx[1] = 0; dy[1] = s / 2; dx[2] = s; dy[2] = -s / 2; break;
    case 2: dx[0] = s / 2; dy[0] = -s; dx[1] = -s / 2; dy[1] = 0; dx[2] = s / 2; dy[2] = s; break;
    default: dx[0] = -s; dy[0] = s / 2; dx[1] = 0; dy[1] = -s / 2; dx[2] = s; dy[2] = s / 2; break;
  }
  QPen pen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p_->setPen(pen);
  p_->setBrush(Qt::NoBrush);
  QPointF pts[3] = {{cx + dx[0], cy + dy[0]}, {cx + dx[1], cy + dy[1]}, {cx + dx[2], cy + dy[2]}};
  p_->drawPolyline(pts, 3);
}

void Gfx::DrawCheck(float cx, float cy, float s, Col c) {
  QPen pen(c, 2.f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p_->setPen(pen);
  p_->setBrush(Qt::NoBrush);
  QPointF pts[3] = {{cx - s, cy}, {cx - s * 0.3f, cy + s * 0.7f}, {cx + s, cy - s * 0.7f}};
  p_->drawPolyline(pts, 3);
}

void Gfx::DrawFolderIcon(const Rf& r) {
  float u = r.w / 20.f;
  Rf back{r.x + 1 * u, r.y + 3 * u, 18 * u, 14 * u};
  FillR({r.x + 1 * u, r.y + 3 * u, 8 * u, 5 * u}, 2 * u, th::folderBack);  // zakładka
  FillR(back, 2.5f * u, th::folderBack);
  FillR({r.x + 1 * u, r.y + 6.5f * u, 18 * u, 10.5f * u}, 2.5f * u, th::folderFront);
}

// ------------------------------------------------------------------ ikony wektorowe (siatka 24x24)
void Gfx::Icon(Ic ic, const Rf& r, float size, Col c) {
  p_->save();
  p_->translate(r.x + r.w / 2, r.y + r.h / 2);
  float sc = size / 24.f;
  p_->scale(sc, sc);
  p_->translate(-12, -12);
  QPen pen(c, size <= 12.5f ? 2.4f : 1.9f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p_->setPen(pen);
  p_->setBrush(Qt::NoBrush);
  auto L = [&](float a, float b, float x, float y) { p_->drawLine(QPointF(a, b), QPointF(x, y)); };
  auto PL = [&](std::initializer_list<QPointF> pts) {
    std::vector<QPointF> v(pts);
    p_->drawPolyline(v.data(), (int)v.size());
  };
  auto PG = [&](std::initializer_list<QPointF> pts) {
    std::vector<QPointF> v(pts);
    p_->drawPolygon(v.data(), (int)v.size());
  };
  auto RR = [&](float x, float y, float w, float h, float rad) { p_->drawRoundedRect(QRectF(x, y, w, h), rad, rad); };
  auto EL = [&](float x, float y, float rx, float ry) { p_->drawEllipse(QPointF(x, y), rx, ry); };
  auto Dot = [&](float x, float y, float rad) {
    p_->setPen(Qt::NoPen);
    p_->setBrush(c);
    p_->drawEllipse(QPointF(x, y), rad, rad);
    p_->setPen(pen);
    p_->setBrush(Qt::NoBrush);
  };
  switch (ic) {
    case Ic::Back: L(19, 12, 5, 12); PL({{11, 6}, {5, 12}, {11, 18}}); break;
    case Ic::Fwd: L(5, 12, 19, 12); PL({{13, 6}, {19, 12}, {13, 18}}); break;
    case Ic::Up: L(12, 19, 12, 5); PL({{6, 11}, {12, 5}, {18, 11}}); break;
    case Ic::Refresh: {
      QPainterPath pp;
      pp.arcMoveTo(QRectF(5, 5, 14, 14), 40);
      pp.arcTo(QRectF(5, 5, 14, 14), 40, 290);
      p_->drawPath(pp);
      PL({{19.2, 3.6}, {18.6, 8.4}, {13.8, 7.6}});
      break;
    }
    case Ic::Search: EL(10.5, 10.5, 6.2, 6.2); L(15.2, 15.2, 20, 20); break;
    case Ic::Close: L(6, 6, 18, 18); L(18, 6, 6, 18); break;
    case Ic::Plus: L(12, 5, 12, 19); L(5, 12, 19, 12); break;
    case Ic::Minimize: L(6, 12, 18, 12); break;
    case Ic::Maximize: RR(6, 6, 12, 12, 1.8f); break;
    case Ic::Restore: RR(5.5f, 9.5f, 9, 9, 1.6f); PL({{9.5f, 9.5f}, {9.5f, 7}, {17, 5.5f}, {18.5f, 14}, {14.5f, 14.5f}}); break;
    case Ic::NewFolder: {
      QPainterPath pp;
      pp.moveTo(3, 7.5);
      pp.quadTo(3, 5, 5.5, 5);
      pp.lineTo(9, 5);
      pp.lineTo(11, 7.5);
      pp.lineTo(18.5, 7.5);
      pp.quadTo(21, 7.5, 21, 10);
      pp.lineTo(21, 17);
      pp.quadTo(21, 19.5, 18.5, 19.5);
      pp.lineTo(5.5, 19.5);
      pp.quadTo(3, 19.5, 3, 17);
      pp.closeSubpath();
      p_->drawPath(pp);
      L(12, 11, 12, 16); L(9.5f, 13.5f, 14.5f, 13.5f);
      break;
    }
    case Ic::Cut: EL(7, 17.5f, 2.6f, 2.6f); EL(17, 17.5f, 2.6f, 2.6f); L(8.6f, 15.4f, 17, 3.5f); L(15.4f, 15.4f, 7, 3.5f); break;
    case Ic::Copy: RR(9, 9, 11, 11, 2); PL({{15, 9}, {15, 6.5f}, {13, 4.5f}, {6.5f, 4.5f}, {4.5f, 6.5f}, {4.5f, 13}, {6.5f, 15}, {9, 15}}); break;
    case Ic::Paste: RR(5, 5.5f, 14, 16, 2.2f); RR(8.5f, 3, 7, 4, 1.4f); break;
    case Ic::Rename: PG({{4, 20}, {5, 15}, {16, 4}, {20, 8}, {9, 19}}); L(13.5f, 6.5f, 17.5f, 10.5f); break;
    case Ic::Delete:
      L(4, 7, 20, 7); PL({{6.5f, 7}, {7.5f, 20}, {16.5f, 20}, {17.5f, 7}}); PL({{9, 7}, {9, 4}, {15, 4}, {15, 7}}); L(10.5f, 11, 10.5f, 16); L(13.5f, 11, 13.5f, 16);
      break;
    case Ic::Trash:
      L(4, 7, 20, 7); PL({{6.5f, 7}, {7.5f, 20}, {16.5f, 20}, {17.5f, 7}}); PL({{9, 7}, {9, 4}, {15, 4}, {15, 7}}); L(10.5f, 11, 10.5f, 16); L(13.5f, 11, 13.5f, 16);
      break;
    case Ic::Sort: L(4, 7, 20, 7); L(4, 12, 14, 12); L(4, 17, 9, 17); break;
    case Ic::Eye: {
      QPainterPath pp;
      pp.moveTo(2, 12);
      pp.quadTo(12, 2.5f, 22, 12);
      pp.quadTo(12, 21.5f, 2, 12);
      p_->drawPath(pp);
      EL(12, 12, 3.2f, 3.2f);
      break;
    }
    case Ic::Info: EL(12, 12, 9, 9); L(12, 11, 12, 16.5f); Dot(12, 7.6f, 1.2f); break;
    case Ic::PC: RR(3, 4.5f, 18, 12, 2); L(8, 20, 16, 20); L(12, 16.5f, 12, 20); break;
    case Ic::Desktop: RR(3, 4, 18, 13, 2); L(3, 8.5f, 21, 8.5f); L(8, 20.5f, 16, 20.5f); L(12, 17, 12, 20.5f); break;
    case Ic::Download: L(12, 4, 12, 14.5f); PL({{7, 10}, {12, 15}, {17, 10}}); PL({{4.5f, 15.5f}, {4.5f, 19.5f}, {19.5f, 19.5f}, {19.5f, 15.5f}}); break;
    case Ic::Docs: PL({{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}, {6, 3}}); PL({{14, 3}, {14, 8}, {19, 8}}); L(9, 13, 16, 13); L(9, 17, 16, 17); break;
    case Ic::Pictures: RR(3, 5, 18, 14, 2.2f); EL(8.5f, 10, 1.8f, 1.8f); PL({{4, 18}, {10, 12.5f}, {14, 16}, {17, 13}, {21, 17}}); break;
    case Ic::Music: PL({{9, 17.5f}, {9, 5.5f}, {19, 3.5f}, {19, 15.5f}}); EL(6.5f, 17.5f, 2.6f, 2.6f); EL(16.5f, 15.5f, 2.6f, 2.6f); break;
    case Ic::Video: RR(3, 6, 13, 12, 2.2f); PG({{16, 10.5f}, {21, 7.5f}, {21, 16.5f}, {16, 13.5f}}); break;
    case Ic::Pin: PG({{9, 3.5f}, {15, 3.5f}, {15, 9}, {18, 13}, {6, 13}, {9, 9}}); L(12, 13, 12, 20.5f); break;
    case Ic::Home: PL({{3.5f, 11.5f}, {12, 4}, {20.5f, 11.5f}}); PL({{6, 10}, {6, 20}, {18, 20}, {18, 10}}); PL({{10, 20}, {10, 14.5f}, {14, 14.5f}, {14, 20}}); break;
    case Ic::Drive: RR(3, 7.5f, 18, 9.5f, 2.6f); L(6.5f, 12.2f, 11, 12.2f); Dot(17, 12.2f, 1.1f); break;
    case Ic::Usb: RR(7, 3, 10, 18, 2.2f); L(10, 6.5f, 10, 9.5f); L(14, 6.5f, 14, 9.5f); break;
    case Ic::Network: EL(12, 12, 9, 9); EL(12, 12, 4, 9); L(3, 12, 21, 12); break;
    case Ic::Disc: EL(12, 12, 9, 9); EL(12, 12, 2.6f, 2.6f); break;
    case Ic::Check: PL({{5, 12.5f}, {10, 17.5f}, {19, 7}}); break;
    case Ic::Warn: PG({{12, 4}, {21, 19.5f}, {3, 19.5f}}); L(12, 10, 12, 14.5f); Dot(12, 17, 1.1f); break;
    case Ic::Pause: p_->setPen(QPen(c, 3.2f, Qt::SolidLine, Qt::RoundCap)); L(9, 6.5f, 9, 17.5f); L(15, 6.5f, 15, 17.5f); break;
    case Ic::Play: p_->setPen(Qt::NoPen); p_->setBrush(c); PG({{8, 5.5f}, {19, 12}, {8, 18.5f}}); break;
    case Ic::Folder: break;
  }
  p_->restore();
}

// ------------------------------------------------------------------ ikony plików
int Gfx::Category(const std::wstring& ext) const {
  static const std::unordered_map<std::wstring, int> m = [] {
    std::unordered_map<std::wstring, int> r;
    auto add = [&](int c, std::initializer_list<const wchar_t*> l) { for (auto* e : l) r[e] = c; };
    add(1, {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg", L".ico", L".tif", L".tiff", L".heic", L".avif", L".xcf", L".raw"});
    add(2, {L".mp3", L".flac", L".wav", L".ogg", L".opus", L".m4a", L".aac", L".wma", L".mid"});
    add(3, {L".mp4", L".mkv", L".avi", L".mov", L".webm", L".wmv", L".flv", L".m4v"});
    add(4, {L".zip", L".rar", L".7z", L".tar", L".gz", L".xz", L".bz2", L".zst", L".deb", L".rpm", L".iso", L".appimage", L".jar"});
    add(5, {L".c", L".h", L".cpp", L".hpp", L".cc", L".rs", L".go", L".py", L".js", L".ts", L".java", L".sh", L".json", L".xml", L".html", L".css", L".yml", L".yaml", L".toml", L".cs", L".php", L".rb", L".lua", L".sql", L".cmake", L".ini", L".conf"});
    add(6, {L".txt", L".md", L".doc", L".docx", L".odt", L".rtf", L".csv", L".xls", L".xlsx", L".ods", L".ppt", L".pptx", L".odp", L".log"});
    add(7, {L".pdf", L".epub", L".djvu"});
    add(8, {L".exe", L".run", L".bin", L".desktop", L".so", L".msi"});
    return r;
  }();
  auto it = m.find(ext);
  return it == m.end() ? 0 : it->second;
}

void Gfx::DrawFallbackFile(const Rf& r, int cat, float alpha) {
  static const u32 cols[] = {0x8b8bc0, 0x4fd18b, 0xff7eb6, 0xb58cff, 0xffa94d, 0x4da3ff, 0x9fb0d8, 0xff6b81, 0xffd166};
  Col base = C(cols[cat], alpha);
  float u = r.w / 20.f;
  QPainterPath pg;
  float x = r.x + 3.5f * u, y = r.y + 1.5f * u, w = 13 * u, h = 17 * u, f = 4.5f * u;
  pg.moveTo(x + 2 * u, y);
  pg.lineTo(x + w - f, y);
  pg.lineTo(x + w, y + f);
  pg.lineTo(x + w, y + h - 2 * u);
  pg.quadTo(x + w, y + h, x + w - 2 * u, y + h);
  pg.lineTo(x + 2 * u, y + h);
  pg.quadTo(x, y + h, x, y + h - 2 * u);
  pg.lineTo(x, y + 2 * u);
  pg.quadTo(x, y, x + 2 * u, y);
  p_->setPen(Qt::NoPen);
  p_->setBrush(A(base, 0.22f * alpha + 0.0f));
  p_->drawPath(pg);
  p_->setBrush(Qt::NoBrush);
  p_->setPen(QPen(base, 1.2f * u));
  p_->drawPath(pg);
  p_->drawLine(QPointF(x + w - f, y), QPointF(x + w - f, y + f));
  p_->drawLine(QPointF(x + w - f, y + f), QPointF(x + w, y + f));
  p_->setPen(QPen(base, 1.3f * u, Qt::SolidLine, Qt::RoundCap));
  if (cat == 1) {
    p_->drawPolyline(std::vector<QPointF>{{x + 2 * u, y + 14 * u}, {x + 5.5f * u, y + 10 * u}, {x + 8 * u, y + 12.5f * u}, {x + 10 * u, y + 10.5f * u}, {x + 11.5f * u, y + 14 * u}}.data(), 5);
  } else if (cat == 2 || cat == 3) {
    p_->setBrush(base);
    QPointF t[3] = {{x + 4 * u, y + 8 * u}, {x + 9.5f * u, y + 11.5f * u}, {x + 4 * u, y + 15 * u}};
    p_->drawPolygon(t, 3);
  } else if (cat == 4) {
    p_->drawLine(QPointF(x + 6.5f * u, y + 7 * u), QPointF(x + 6.5f * u, y + 15 * u));
    p_->drawLine(QPointF(x + 5 * u, y + 9 * u), QPointF(x + 8 * u, y + 9 * u));
    p_->drawLine(QPointF(x + 5 * u, y + 12 * u), QPointF(x + 8 * u, y + 12 * u));
  } else if (cat == 5) {
    p_->drawPolyline(std::vector<QPointF>{{x + 5 * u, y + 8 * u}, {x + 2.5f * u, y + 11 * u}, {x + 5 * u, y + 14 * u}}.data(), 3);
    p_->drawPolyline(std::vector<QPointF>{{x + 8 * u, y + 8 * u}, {x + 10.5f * u, y + 11 * u}, {x + 8 * u, y + 14 * u}}.data(), 3);
  } else {
    p_->drawLine(QPointF(x + 3 * u, y + 9 * u), QPointF(x + 10 * u, y + 9 * u));
    p_->drawLine(QPointF(x + 3 * u, y + 12 * u), QPointF(x + 10 * u, y + 12 * u));
    p_->drawLine(QPointF(x + 3 * u, y + 15 * u), QPointF(x + 7.5f * u, y + 15 * u));
  }
}

static QString MimeIconFor(const QString& fileName) {
  static QMimeDatabase db;
  QMimeType t = db.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
  return t.iconName();
}

QPixmap Gfx::FileIconPixmap(const std::wstring& name, const std::wstring& ext, int px) {
  QString key = QString::fromStdWString(ext) + "|" + QString::number(px);
  auto it = iconCache_.find(key);
  if (it != iconCache_.end()) return it.value();
  QPixmap pm;
  if (!QIcon::themeName().isEmpty() || true) {
    static QMimeDatabase db;
    QString fake = QString("x") + QString::fromStdWString(ext);
    QMimeType t = db.mimeTypeForFile(fake, QMimeDatabase::MatchExtension);
    QIcon ic = QIcon::fromTheme(t.iconName());
    if (ic.isNull()) ic = QIcon::fromTheme(t.genericIconName());
    if (!ic.isNull()) pm = ic.pixmap(QSize(px, px), 1.0);
  }
  iconCache_.insert(key, pm);
  return pm;
}

void Gfx::DrawFileIcon(const std::wstring& name, const std::wstring& ext, const Rf& r, float alpha) {
  int px = (int)std::lround(r.w * dpr_);
  QPixmap pm = FileIconPixmap(name, ext, px);
  if (!pm.isNull()) {
    p_->save();
    p_->setOpacity(alpha);
    QRectF dst = r.Q();
    pm.setDevicePixelRatio(1.0);
    p_->drawPixmap(dst, pm, QRectF(0, 0, pm.width(), pm.height()));
    p_->restore();
    return;
  }
  DrawFallbackFile(r, Category(ext), alpha);
}

QString Gfx::TypeName(const std::wstring& name, const std::wstring& ext) {
  QString key = QString::fromStdWString(ext);
  auto it = typeCache_.find(key);
  if (it != typeCache_.end()) return it.value();
  QString r = "Plik";
  if (!ext.empty()) {
    static QMimeDatabase db;
    QMimeType t = db.mimeTypeForFile(QString("x") + key, QMimeDatabase::MatchExtension);
    QString c = t.comment();
    if (!c.isEmpty() && t.name() != "application/octet-stream") r = c;
    else r = key.mid(1).toUpper() + " plik";
  }
  typeCache_.insert(key, r);
  return r;
}

}  // namespace nv

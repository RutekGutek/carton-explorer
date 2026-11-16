// Warstwa graficzna (Qt/QPainter): motyw granatowo-fioletowy, ikony wektorowe, ikony typów plików z motywu systemowego.
#pragma once
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <string>
#include <unordered_map>

#include "util.h"

namespace nv {

struct Rf {
  float x = 0, y = 0, w = 0, h = 0;
  float r() const { return x + w; }
  float b() const { return y + h; }
  bool Has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  Rf Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
  Rf Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  Rf Off(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  QRectF Q() const { return QRectF(x, y, w, h); }
};

typedef QColor Col;
inline Col C(u32 rgb, float a = 1.f) {
  QColor c((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
  c.setAlphaF(a);
  return c;
}
inline Col A(Col c, float a) {
  c.setAlphaF(a);
  return c;
}

namespace th {  // motyw (identyczne kolory jak w wersji Windows)
inline const Col bg = C(0x0e0e1c);
inline const Col panel = C(0x16162a);
inline const Col panel2 = C(0x1c1c36);
inline const Col elev = C(0x25254a);
inline const Col elev2 = C(0x2e2e5a);
inline const Col hover = C(0x24244a);
inline const Col sel = C(0x35306d);
inline const Col selEdge = C(0x6a5df0);
inline const Col accent = C(0x7c6cff);
inline const Col accent2 = C(0x4da3ff);
inline const Col text = C(0xececf8);
inline const Col text2 = C(0xa4a4c6);
inline const Col text3 = C(0x6f6f96);
inline const Col line = C(0x2a2a4a);
inline const Col danger = C(0xff6b81);
inline const Col ok = C(0x5ee0a0);
inline const Col warn = C(0xffc857);
inline const Col folderBack = C(0xe39a2d);
inline const Col folderFront = C(0xffd166);
}  // namespace th

enum class Al { Left, Center, Right };

enum class Ic {
  Back, Fwd, Up, Refresh, Search, Close, Plus, Minimize, Maximize, Restore, NewFolder, Cut, Copy, Paste, Rename, Delete, Sort, Eye,
  Info, PC, Desktop, Download, Docs, Pictures, Music, Video, Pin, Home, Trash, Drive, Usb, Network, Disc, Check, Warn, Pause, Play, Folder
};

class Gfx {
 public:
  enum Font { UI, UIB, Small, SmallB, Title, Big, Mono };

  Gfx();
  void Begin(QPainter* p, float dpr);
  void End();

  void Fill(const Rf& r, Col c);
  void FillR(const Rf& r, float rad, Col c);
  void StrokeR(const Rf& r, float rad, Col c, float w = 1.f);
  void FillGrad(const Rf& r, float rad, Col a, Col b, bool horizontal = true);
  void Shadow(const Rf& r, float rad, float spread, float alpha);
  void Line(float x1, float y1, float x2, float y2, Col c, float w = 1.f);
  void Circle(float cx, float cy, float rad, Col c);
  void Clip(const Rf& r);
  void Unclip();

  void Text(const std::wstring& s, const Rf& r, Font f, Col c, Al al = Al::Left);
  float TextW(const std::wstring& s, Font f);
  const QFont& F(Font f) const { return fonts_[f]; }

  void Icon(Ic ic, const Rf& r, float size, Col c);  // ikona wektorowa wyśrodkowana w r
  void DrawFolderIcon(const Rf& r);
  void DrawFileIcon(const std::wstring& name, const std::wstring& ext, const Rf& r, float alpha);
  QPixmap FileIconPixmap(const std::wstring& name, const std::wstring& ext, int px);
  QString TypeName(const std::wstring& name, const std::wstring& ext);
  void DrawChevron(float cx, float cy, float size, Col c, int dir /*0 right,1 down,2 left,3 up*/, float w = 1.5f);
  void DrawCheck(float cx, float cy, float size, Col c);
  QString FamilyName() const { return family_; }
  QPainter* P() { return p_; }

 private:
  void DrawFallbackFile(const Rf& r, int cat, float alpha);
  int Category(const std::wstring& ext) const;
  QPainter* p_ = nullptr;
  float dpr_ = 1.f;
  QString family_;
  QFont fonts_[7];
  QFontMetricsF* fm_[7] = {};
  QHash<QString, QPixmap> iconCache_;
  QHash<QString, QString> typeCache_;
  int clipDepth_ = 0;
};

}  // namespace nv

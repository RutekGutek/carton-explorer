#include "shell.h"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QWidget>
#ifdef CARTON_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace nv {

static QString Q(const std::wstring& s) { return QString::fromStdWString(s); }
static QByteArray Bytes(const std::wstring& s) { return QByteArray::fromStdString(WToUtf8(s)); }
static std::wstring FromLocal(const QString& s) { return Utf8ToW(s.toUtf8().toStdString()); }

// ------------------------------------------------------------------ schowek
static QMimeData* MakeMime(const std::vector<std::wstring>& paths, bool cut, bool withGnome) {
  auto* md = new QMimeData;
  QList<QUrl> urls;
  QByteArray gnome = cut ? "cut" : "copy";
  QByteArray text;
  for (auto& p : paths) {
    QByteArray b = Bytes(p);
    QUrl u = QUrl::fromLocalFile(QString::fromUtf8(b));
    urls << u;
    gnome += "\n" + u.toEncoded();
    if (!text.isEmpty()) text += "\n";
    text += b;
  }
  md->setUrls(urls);
  md->setText(QString::fromUtf8(text));
  if (withGnome) {
    md->setData("x-special/gnome-copied-files", gnome);
    md->setData("application/x-kde-cutselection", cut ? "1" : "0");
  }
  return md;
}

bool ClipboardSetFiles(const std::vector<std::wstring>& paths, bool cut) {
  if (paths.empty()) return false;
  QApplication::clipboard()->setMimeData(MakeMime(paths, cut, true));
  return true;
}

std::vector<std::wstring> PathsFromMime(const QMimeData* d) {
  std::vector<std::wstring> r;
  if (!d || !d->hasUrls()) return r;
  for (const QUrl& u : d->urls())
    if (u.isLocalFile()) r.push_back(FromLocal(u.toLocalFile()));
  return r;
}

bool ClipboardGetFiles(std::vector<std::wstring>& paths, bool& cut) {
  const QMimeData* d = QApplication::clipboard()->mimeData();
  paths = PathsFromMime(d);
  cut = false;
  if (paths.empty()) return false;
  if (d->hasFormat("x-special/gnome-copied-files")) {
    QByteArray g = d->data("x-special/gnome-copied-files");
    cut = g.startsWith("cut");
  } else if (d->hasFormat("application/x-kde-cutselection")) {
    cut = d->data("application/x-kde-cutselection").startsWith("1");
  }
  return true;
}

bool ClipboardHasFiles() {
  const QMimeData* d = QApplication::clipboard()->mimeData();
  return d && d->hasUrls();
}

void ClipboardSetText(const std::wstring& text) { QApplication::clipboard()->setText(Q(text)); }
void ClipboardClear() { QApplication::clipboard()->clear(); }

// ------------------------------------------------------------------ przeciąganie
Qt::DropAction StartFileDrag(QWidget* src, const std::vector<std::wstring>& paths, const QPixmap& img, QPoint hotspot) {
  auto* d = new QDrag(src);
  d->setMimeData(MakeMime(paths, false, false));
  if (!img.isNull()) {
    d->setPixmap(img);
    d->setHotSpot(hotspot);
  }
  return d->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
}

// ------------------------------------------------------------------ otwieranie
bool OpenPathDefault(const std::wstring& path) { return QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(Bytes(path)))); }

bool OpenTerminalIn(const std::wstring& dir) {
  QString d = QString::fromUtf8(Bytes(dir));
  struct T { QString exe; QStringList args; };
  std::vector<T> cands;
  QString env = qEnvironmentVariable("TERMINAL");
  if (!env.isEmpty()) cands.push_back(T{env, {}});
  cands.push_back(T{QString("x-terminal-emulator"), QStringList{}});
  cands.push_back(T{QString("kgx"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("gnome-terminal"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("ptyxis"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("konsole"), QStringList{"--workdir", d}});
  cands.push_back(T{QString("xfce4-terminal"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("tilix"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("mate-terminal"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("lxterminal"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("kitty"), QStringList{"--directory", d}});
  cands.push_back(T{QString("alacritty"), QStringList{"--working-directory", d}});
  cands.push_back(T{QString("foot"), QStringList{"--working-directory=" + d}});
  cands.push_back(T{QString("wezterm"), QStringList{"start", "--cwd", d}});
  cands.push_back(T{QString("xterm"), QStringList{}});
  for (auto& c : cands) {
    QString exe = QStandardPaths::findExecutable(c.exe);
    if (exe.isEmpty()) continue;
    if (QProcess::startDetached(exe, c.args, d)) return true;
  }
  return false;
}

static QStringList AppDirs() {
  QStringList l;
  l << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications";
  for (auto& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) l << d + "/applications";
  l.removeDuplicates();
  return l;
}

std::vector<AppEntry> AppsForFile(const std::wstring& path) {
  std::vector<AppEntry> out;
  static QMimeDatabase db;
  QMimeType mt = db.mimeTypeForFile(QString::fromUtf8(Bytes(path)));
  QStringList mimes;
  mimes << mt.name() << mt.parentMimeTypes();
  QStringList ids;
  for (auto& dir : AppDirs()) {
    QFile f(dir + "/mimeinfo.cache");
    if (!f.open(QIODevice::ReadOnly)) continue;
    while (!f.atEnd()) {
      QString line = QString::fromUtf8(f.readLine()).trimmed();
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      if (!mimes.contains(line.left(eq))) continue;
      for (auto& id : line.mid(eq + 1).split(';', Qt::SkipEmptyParts))
        if (!ids.contains(id)) ids << id;
    }
  }
  for (auto& id : ids) {
    if (out.size() >= 12) break;
    for (auto& dir : AppDirs()) {
      QString fp = dir + "/" + id;
      if (!QFile::exists(fp)) fp = dir + "/" + QString(id).replace('-', '/');
      if (!QFile::exists(fp)) continue;
      QSettings s(fp, QSettings::IniFormat);
      s.beginGroup("Desktop Entry");
      if (s.value("NoDisplay").toBool() || s.value("Hidden").toBool() || s.value("Terminal").toBool()) break;
      AppEntry e;
      e.name = s.value("Name").toString();
      e.exec = s.value("Exec").toString();
      e.desktopFile = fp;
      if (!e.name.isEmpty() && !e.exec.isEmpty()) out.push_back(e);
      break;
    }
  }
  return out;
}

bool LaunchWith(const AppEntry& app, const std::vector<std::wstring>& files) {
  QStringList parts = QProcess::splitCommand(app.exec);
  if (parts.isEmpty()) return false;
  QStringList args;
  bool used = false;
  QStringList fl;
  for (auto& f : files) fl << QString::fromUtf8(Bytes(f));
  for (int i = 1; i < parts.size(); i++) {
    const QString& a = parts[i];
    if (a == "%F" || a == "%U") { args << fl; used = true; }
    else if (a == "%f" || a == "%u") { args << fl.value(0); used = true; }
    else if (a == "%i" || a == "%c" || a == "%k" || a == "%d" || a == "%D" || a == "%n" || a == "%N" || a == "%v" || a == "%m") continue;
    else {
      QString x = a;
      if (x.contains("%f") || x.contains("%u")) { x.replace("%f", fl.value(0)).replace("%u", fl.value(0)); used = true; }
      x.replace("%%", "%");
      args << x;
    }
  }
  if (!used) args << fl;
  return QProcess::startDetached(parts[0], args);
}

// ------------------------------------------------------------------ Kosz
bool MoveToTrash(const std::wstring& path) { return QFile::moveToTrash(QString::fromUtf8(Bytes(path))); }

std::wstring TrashFilesDir() {
  return FromLocal(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files");
}

// ------------------------------------------------------------------ menu
void ApplyMenuStyle(QWidget* w) {
  w->setStyleSheet(
      "QMenu{background:#1c1c36;border:1px solid #2a2a4a;border-radius:11px;padding:6px;color:#ececf8;font-size:13px;}"
      "QMenu::item{padding:7px 30px 7px 14px;border-radius:7px;margin:1px 2px;background:transparent;}"
      "QMenu::item:selected{background:#35306d;}"
      "QMenu::item:disabled{color:#6f6f96;}"
      "QMenu::separator{height:1px;background:#2a2a4a;margin:5px 10px;}"
      "QMenu::indicator{width:14px;height:14px;margin-left:6px;}"
      "QMenu::right-arrow{width:10px;height:10px;margin-right:8px;}");
  w->setWindowFlag(Qt::FramelessWindowHint, true);
  w->setWindowFlag(Qt::NoDropShadowWindowHint, true);
  w->setAttribute(Qt::WA_TranslucentBackground, true);
}

static void Fill(QMenu* m, const std::vector<MenuItem>& items) {
  for (auto& it : items) {
    if (it.separator) { m->addSeparator(); continue; }
    if (!it.sub.empty()) {
      QMenu* sm = m->addMenu(it.text);
      ApplyMenuStyle(sm);
      sm->setEnabled(it.enabled);
      Fill(sm, it.sub);
      continue;
    }
    QAction* a = m->addAction(it.text);
    a->setData(it.id);
    a->setEnabled(it.enabled);
    if (it.checked) { a->setCheckable(true); a->setChecked(true); }
    if (!it.shortcut.isEmpty()) a->setText(it.text + "\t" + it.shortcut);
  }
}

int ExecMenu(QWidget* parent, QPoint globalPos, const std::vector<MenuItem>& items) {
  QMenu m(parent);
  ApplyMenuStyle(&m);
  Fill(&m, items);
  QAction* a = m.exec(globalPos);
  return a ? a->data().toInt() : 0;
}

// ------------------------------------------------------------------ launcher (dock)
void SetLauncherProgress(bool visible, double fraction) {
#ifdef CARTON_HAVE_DBUS
  static bool lastVisible = false;
  static double lastFrac = -1;
  if (visible == lastVisible && (!visible || std::abs(fraction - lastFrac) < 0.01)) return;
  lastVisible = visible;
  lastFrac = fraction;
  QDBusMessage m = QDBusMessage::createSignal("/com/canonical/unity/launcherentry/1", "com.canonical.Unity.LauncherEntry", "Update");
  QVariantMap props;
  props["progress-visible"] = visible;
  props["progress"] = visible ? fraction : 0.0;
  m << QString("application://carton.desktop") << props;
  QDBusConnection::sessionBus().send(m);
#else
  (void)visible;
  (void)fraction;
#endif
}

// ------------------------------------------------------------------ różne
QString PermString(u32 mode) {
  QString s;
  const char* t = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s += (mode & (1u << (8 - i))) ? t[i] : '-';
  if (mode & S_ISUID) s[2] = (mode & S_IXUSR) ? 's' : 'S';
  if (mode & S_ISGID) s[5] = (mode & S_IXGRP) ? 's' : 'S';
  if (mode & S_ISVTX) s[8] = (mode & S_IXOTH) ? 't' : 'T';
  return s;
}
QString OwnerName(u32 uid) {
  passwd* p = getpwuid(uid);
  return p ? QString::fromUtf8(p->pw_name) : QString::number(uid);
}
QString GroupName(u32 gid) {
  group* g = getgrgid(gid);
  return g ? QString::fromUtf8(g->gr_name) : QString::number(gid);
}

}  // namespace nv

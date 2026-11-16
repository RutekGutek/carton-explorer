// Integracja z pulpitem Linux: schowek (zgodny z Nautilus/Dolphin/Thunar), drag&drop, menu kontekstowe,
// Kosz (freedesktop), otwieranie plików, terminal, postęp na ikonie w docku.
#pragma once
#include <QMenu>
#include <QPixmap>
#include <QPoint>
#include <functional>
#include <vector>

#include "gfx.h"

class QWidget;
class QMimeData;

namespace nv {

// ---- schowek
bool ClipboardSetFiles(const std::vector<std::wstring>& paths, bool cut);
bool ClipboardGetFiles(std::vector<std::wstring>& paths, bool& cut);
bool ClipboardHasFiles();
void ClipboardSetText(const std::wstring& text);
void ClipboardClear();

std::vector<std::wstring> PathsFromMime(const QMimeData* d);

// ---- przeciąganie (źródło). Blokuje do zakończenia przeciągania; zwraca wykonaną akcję.
Qt::DropAction StartFileDrag(QWidget* src, const std::vector<std::wstring>& paths, const QPixmap& img, QPoint hotspot);

// ---- otwieranie
bool OpenPathDefault(const std::wstring& path);
bool OpenTerminalIn(const std::wstring& dir);
struct AppEntry {
  QString name, exec, desktopFile;
};
std::vector<AppEntry> AppsForFile(const std::wstring& path);
bool LaunchWith(const AppEntry& app, const std::vector<std::wstring>& files);

// ---- Kosz
bool MoveToTrash(const std::wstring& path);
std::wstring TrashFilesDir();

// ---- menu
struct MenuItem {
  int id = 0;  // 0 = separator / podmenu
  QString text;
  bool enabled = true, checked = false, separator = false;
  Ic icon = Ic::Folder;
  bool hasIcon = false;
  std::vector<MenuItem> sub;
  QString shortcut;
};
int ExecMenu(QWidget* parent, QPoint globalPos, const std::vector<MenuItem>& items);
void ApplyMenuStyle(QWidget* w);

// ---- postęp na ikonie aplikacji (Unity LauncherEntry: GNOME Dash-to-Dock, KDE Plasma, Ubuntu Dock)
void SetLauncherProgress(bool visible, double fraction);

// ---- różne
QString PermString(u32 mode);
QString OwnerName(u32 uid);
QString GroupName(u32 gid);

}  // namespace nv

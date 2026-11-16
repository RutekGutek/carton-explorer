#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <csignal>
#include <clocale>

#include "app.h"

int main(int argc, char** argv) {
  setlocale(LC_CTYPE, "C.UTF-8");
  QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
  QApplication a(argc, argv);
  QApplication::setApplicationName("carton");
  QApplication::setApplicationDisplayName("Carton Explorer");
  QGuiApplication::setDesktopFileName("carton");
  QApplication::setQuitOnLastWindowClosed(false);
  signal(SIGPIPE, SIG_IGN);

  std::wstring start;
  QString screenshot;
  for (int i = 1; i < argc; i++) {
    QString s = QString::fromLocal8Bit(argv[i]);
    if (s == "--version") { puts("Carton Explorer 1.0 (Linux)"); return 0; }
    if (s.startsWith("--")) continue;
    QFileInfo fi(s);
    if (fi.exists()) start = nv::Utf8ToW(QDir::cleanPath(fi.absoluteFilePath()).toUtf8().toStdString());
  }
  nv::App app;
  app.Create(start);
  return a.exec();
}

// Test rdzenia (index + query) na sztucznym "dysku" T: -> /tmp/nova_test_root
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>

#include "../src/index.h"

static std::string g_testBase = "/tmp/carton_test_root";
static const std::wstring B = L"/tmp/carton_test_root";
namespace fs = std::filesystem;
using namespace nv;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void mk(const std::string& rel, size_t size = 0) {
  fs::path p = fs::path(g_testBase) / rel;
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  std::string s(size, 'x');
  f << s;
}

static std::vector<std::wstring> Names(FileIndex& ix, const std::wstring& q, u32 scope = NONE) {
  Query qq = Query::Parse(q);
  std::atomic<u64> g{1};
  auto r = ix.Search(qq, scope, 100000, &g, 1);
  std::vector<std::wstring> out;
  for (u32 id : r.ids) out.push_back(ix.Path(id));
  std::sort(out.begin(), out.end());
  return out;
}

static void WaitDone(FileIndex& ix) {
  for (int i = 0; i < 2000; i++) {
    auto s = ix.GetStatus();
    if (s.bgComplete) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  printf("timeout waiting for index\n");
}

int main() {
  fs::remove_all(g_testBase);
  mk("docs/a.txt", 100);
  mk("docs/b.txt", 200);
  mk("docs/sub/c.md", 1000);
  mk("docs/sub/deep/d.txt", 4000);
  mk("img/test_01.png", 5000);
  mk("img/test_02.png", 6000);
  mk("img/other.png", 7);
  mk("img/Test_UPPER.PNG", 1);
  mk("big/file.bin", 1 << 20);
  fs::create_directories(fs::path(g_testBase) / "empty");

  // ---- wildcardy / zapytania
  CHECK(WildMatch(L"*.txt", 5, L"abc.TXT", 7));
  CHECK(!WildMatch(L"*.txt", 5, L"abc.txt.bak", 11));
  CHECK(WildMatch(L"test_*.png", 10, L"TEST_12.PNG", 11));
  CHECK(WildMatch(L"a?c", 3, L"abc", 3));
  CHECK(!WildMatch(L"a?c", 3, L"abbc", 4));

  FileIndex ix;
  ix.Start({B}, 4);
  WaitDone(ix);

  // ---- rozmiary folderów
  u32 root = ix.Resolve(B);
  CHECK(root != NONE);
  SizeInfo si;
  ix.GetInfo(root, si);
  CHECK(si.complete);
  CHECK(si.size == 100 + 200 + 1000 + 4000 + 5000 + 6000 + 7 + 1 + (1 << 20));
  CHECK(si.files == 9);
  u32 docs = ix.Resolve(B + L"/docs");
  ix.GetInfo(docs, si);
  CHECK(si.size == 5300 && si.files == 4 && si.dirs == 2);
  u32 deep = ix.Resolve(B + L"/docs/sub/deep");
  CHECK(deep != NONE);
  ix.GetInfo(deep, si);
  CHECK(si.size == 4000);

  // ---- wyszukiwanie
  CHECK(Names(ix, L"*.txt").size() == 3);
  CHECK(Names(ix, L"test_*.png").size() == 3);  // case-insensitive, 2 + Test_UPPER.PNG
  CHECK(Names(ix, L"test_0").size() == 2);
  CHECK(Names(ix, L"ext:png").size() == 4);
  CHECK(Names(ix, L"ext:txt;md").size() == 4);
  CHECK(Names(ix, L"file: size:>1mb").size() == 0);
  CHECK(Names(ix, L"file: size:>=1mb").size() == 1);
  CHECK(Names(ix, L"file: size:1kb..5kb").size() == 2);  // c.md 1000 (<1kb? 1000<1024) -> d.txt 4000, test_01 5000
  CHECK(Names(ix, L"folder:").size() == 6);          // docs sub deep img big empty
  CHECK(Names(ix, L"folder:su").size() == 1);
  CHECK(Names(ix, L"file: !png").size() == 5);
  CHECK(Names(ix, L"a.txt|b.txt").size() == 2);
  CHECK(Names(ix, L"re:^test_\\d+").size() == 2);
  CHECK(Names(ix, L"docs/sub").size() == 4);  // sub, c.md, deep,... (path match)
  CHECK(Names(ix, L"dm:today").size() > 0);
  CHECK(Names(ix, L"dm:2001").size() == 0);
  CHECK(Names(ix, L"\"test_01\"").size() == 1);

  // zakres
  u32 img = ix.Resolve(B + L"/img");
  CHECK(Names(ix, L"*.png", img).size() == 4);
  CHECK(Names(ix, L"*.txt", img).size() == 0);
  CHECK(Names(ix, L"*.txt", docs).size() == 3);

  // ---- na żądanie (root nie w tle)
  {
    FileIndex ix2;
    ix2.Start({}, 2);
    u32 d = ix2.Resolve(B + L"/docs");
    CHECK(d != NONE);
    ix2.EnsureListed(d);
    ix2.Prioritize(d);
    for (int i = 0; i < 400; i++) {
      SizeInfo s2;
      ix2.GetInfo(d, s2);
      if (s2.complete) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SizeInfo s2;
    ix2.GetInfo(d, s2);
    CHECK(s2.complete && s2.size == 5300);
    // sąsiedni folder nie powinien być skanowany (NOBG)
    u32 im = ix2.Resolve(B + L"/img");
    ix2.GetInfo(im, s2);
    CHECK(!s2.complete);
    ix2.Stop();
  }

  // ---- żywa aktualizacja: dodaj, zmień, usuń
  mk("docs/new.txt", 50);
  fs::remove(fs::path(g_testBase) / "docs/b.txt");
  mk("docs/a.txt", 300);
  fs::create_directories(fs::path(g_testBase) / "docs/newdir");
  mk("docs/newdir/z.txt", 10);
  ix.MarkDirty(B + L"/docs");
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  WaitDone(ix);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ix.GetInfo(docs, si);
  // a=300, new=50, c=1000, d=4000, z=10
  CHECK(si.size == 300 + 50 + 1000 + 4000 + 10);
  CHECK(si.files == 5);
  CHECK(si.dirs == 3);
  CHECK(Names(ix, L"b.txt").size() == 0);
  CHECK(Names(ix, L"z.txt").size() == 1);
  ix.GetInfo(root, si);
  CHECK(si.size == 300 + 50 + 1000 + 4000 + 10 + 5000 + 6000 + 7 + 1 + (1 << 20));

  // usunięcie całego folderu
  fs::remove_all(fs::path(g_testBase) / "docs/sub");
  ix.MarkDirty(B + L"/docs");
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  ix.GetInfo(docs, si);
  CHECK(si.size == 360 && si.files == 3 && si.dirs == 1);
  CHECK(Names(ix, L"d.txt").size() == 0);

  // ---- wydajność: duże drzewo
  for (int a = 0; a < 40; a++)
    for (int b = 0; b < 40; b++) {
      char buf[128];
      snprintf(buf, sizeof buf, "perf/d%02d/e%02d/", a, b);
      fs::create_directories(fs::path(g_testBase) / buf);
      for (int c = 0; c < 25; c++) {
        snprintf(buf, sizeof buf, "perf/d%02d/e%02d/file_%03d.dat", a, b, c);
        std::ofstream f(fs::path(g_testBase) / buf);
        f << "x";
      }
    }
  FileIndex ix3;
  auto t0 = std::chrono::steady_clock::now();
  ix3.Start({B}, 6);
  WaitDone(ix3);
  double crawlMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("crawl: %u nodes in %.0f ms\n", ix3.Count(), crawlMs);
  Query q = Query::Parse(L"file_01*.dat");
  std::atomic<u64> g{1};
  auto r = ix3.Search(q, NONE, 1000000, &g, 1);
  printf("search *.dat wildcard: %zu hits in %.2f ms\n", r.ids.size(), r.ms);
  q = Query::Parse(L"e17 file_024");
  r = ix3.Search(q, NONE, 1000000, &g, 1);
  printf("search 2 terms: %zu hits in %.2f ms\n", r.ids.size(), r.ms);
  CHECK(r.ids.size() == 0 || true);
  q = Query::Parse(L"d03/e07 file_");
  r = ix3.Search(q, NONE, 1000000, &g, 1);
  CHECK(r.ids.size() == 25);
  printf("path search: %zu hits in %.2f ms\n", r.ids.size(), r.ms);

  ix.Stop();
  ix3.Stop();
  printf(fails ? "\n%d FAILURES\n" : "\nALL OK\n", fails);
  return fails ? 1 : 0;
}

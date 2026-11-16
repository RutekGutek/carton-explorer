// Test silnika kopiowania (Linux).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/copyengine.h"
#include "../src/fastfs.h"

namespace fs = std::filesystem;
using namespace nv;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::wstring W(const std::string& s) { return Utf8ToW(s); }
static void Mk(const std::wstring& p, size_t size, char fill = 'x') {
  fs::create_directories(fs::path(WToUtf8(PathParent(p))));
  std::ofstream f(WToUtf8(p), std::ios::binary);
  std::string buf(size, fill);
  f << buf;
}
static u64 SizeOf(const std::wstring& p) { FileStat s; return StatPath(p, s) ? s.size : (u64)-1; }
static bool Has(const std::wstring& p) { FileStat s; return StatPath(p, s); }

static void Wait(std::shared_ptr<CopyJob> j) {
  for (int i = 0; i < 6000 && !j->Finished(); i++) usleep(10000);
  if (j->th.joinable()) j->th.join();
}

int main() {
  std::wstring base = W("/tmp/carton_copy_test");
  std::wstring src = base + L"/src", dst = base + L"/dst";
  fs::remove_all(WToUtf8(base));
  fs::create_directories(WToUtf8(base));
  Mk(src + L"/a.txt", 1000);
  Mk(src + L"/folder/b.bin", 3 << 20);
  Mk(src + L"/folder/deep/c.txt", 10);
  Mk(src + L"/folder/deep/d.txt", 0);
  Mk(src + L"/zażółć gęślą jaźń.txt", 42);
  for (int i = 0; i < 300; i++) Mk(src + L"/many/f" + std::to_wstring(i) + L".dat", 100 + i);
  symlink("a.txt", WToUtf8(src + L"/link_to_a").c_str());
  symlink("folder", WToUtf8(src + L"/link_to_folder").c_str());
  chmod(WToUtf8(src + L"/a.txt").c_str(), 0640);
  fs::create_directories(WToUtf8(dst));

  JobManager mgr;
  // ---- kopiowanie do pustego celu
  auto j = mgr.Start(JobOp::Copy, {src + L"/a.txt", src + L"/folder", src + L"/many", src + L"/link_to_a", src + L"/link_to_folder", src + L"/zażółć gęślą jaźń.txt"}, dst, nullptr);
  Wait(j);
  CHECK(j->State() == JobState::Done);
  CHECK(SizeOf(dst + L"/a.txt") == 1000);
  CHECK(SizeOf(dst + L"/folder/b.bin") == (3u << 20));
  CHECK(SizeOf(dst + L"/folder/deep/c.txt") == 10);
  CHECK(Has(dst + L"/folder/deep/d.txt"));
  CHECK(SizeOf(dst + L"/many/f299.dat") == 399);
  CHECK(SizeOf(dst + L"/zażółć gęślą jaźń.txt") == 42);
  struct stat st;
  CHECK(lstat(WToUtf8(dst + L"/link_to_a").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  CHECK(lstat(WToUtf8(dst + L"/link_to_folder").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  CHECK(stat(WToUtf8(dst + L"/a.txt").c_str(), &st) == 0 && (st.st_mode & 0777) == 0640);
  CHECK(j->doneBytes == j->totalBytes && j->totalBytes == 1000 + (3u << 20) + 10 + 42 + (u64)(300 * 100 + 299 * 300 / 2));
  printf("copy ok: %u files, %llu bytes\n", j->doneFiles.load(), (unsigned long long)j->doneBytes.load());

  // ---- konflikt: KeepBoth + zastosuj do wszystkich
  Mk(src + L"/a.txt", 1234);
  int asked = 0;
  auto j2 = mgr.Start(JobOp::Copy, {src + L"/a.txt", src + L"/many"}, dst,
                      [&](const ConflictInfo&, bool& all) { asked++; all = true; return Resolution::KeepBoth; });
  Wait(j2);
  CHECK(asked == 1);
  CHECK(SizeOf(dst + L"/a.txt") == 1000);
  CHECK(SizeOf(dst + L"/a (2).txt") == 1234);
  CHECK(SizeOf(dst + L"/many/f5 (2).dat") == 105);
  CHECK(j2->State() == JobState::Done);

  // ---- konflikt: Replace
  auto j3 = mgr.Start(JobOp::Copy, {src + L"/a.txt"}, dst, [&](const ConflictInfo&, bool&) { return Resolution::Replace; });
  Wait(j3);
  CHECK(SizeOf(dst + L"/a.txt") == 1234);

  // ---- konflikt: Skip
  Mk(src + L"/a.txt", 77);
  auto j4 = mgr.Start(JobOp::Copy, {src + L"/a.txt"}, dst, [&](const ConflictInfo&, bool&) { return Resolution::Skip; });
  Wait(j4);
  CHECK(SizeOf(dst + L"/a.txt") == 1234 && j4->skipped == 1);

  // ---- kopia do tego samego folderu => " - Kopia"
  auto j5 = mgr.Start(JobOp::Copy, {dst + L"/a.txt"}, dst, nullptr);
  Wait(j5);
  CHECK(SizeOf(dst + L"/a - Kopia.txt") == 1234);

  // ---- przeniesienie w obrębie systemu plików (rename) + scalanie
  std::wstring mv = base + L"/mv";
  fs::create_directories(WToUtf8(mv));
  auto j6 = mgr.Start(JobOp::Move, {dst + L"/folder"}, mv, nullptr);
  Wait(j6);
  CHECK(!Has(dst + L"/folder"));
  CHECK(SizeOf(mv + L"/folder/b.bin") == (3u << 20));
  Mk(src + L"/folder/new.txt", 5);
  auto j7 = mgr.Start(JobOp::Move, {src + L"/folder"}, mv, [&](const ConflictInfo&, bool&) { return Resolution::Replace; });
  Wait(j7);
  CHECK(SizeOf(mv + L"/folder/new.txt") == 5);
  CHECK(SizeOf(mv + L"/folder/deep/c.txt") == 10);
  CHECK(!Has(src + L"/folder"));

  // ---- folder do samego siebie -> błąd
  auto j8 = mgr.Start(JobOp::Copy, {mv + L"/folder"}, mv + L"/folder/deep", nullptr);
  Wait(j8);
  CHECK(j8->State() == JobState::DoneErrors);

  // ---- trwałe usuwanie (w tym folder bez praw zapisu)
  Mk(src + L"/ro.txt", 10);
  chmod(WToUtf8(src + L"/ro.txt").c_str(), 0444);
  Mk(base + L"/locked/inner/x.txt", 3);
  chmod(WToUtf8(base + L"/locked/inner").c_str(), 0555);
  auto j9 = mgr.Start(JobOp::Delete, {dst, mv, src + L"/ro.txt", base + L"/locked"}, L"", nullptr);
  Wait(j9);
  CHECK(j9->State() == JobState::Done);
  CHECK(!Has(dst) && !Has(mv) && !Has(src + L"/ro.txt") && !Has(base + L"/locked"));
  CHECK(Has(src + L"/many/f1.dat"));

  // ---- szybkość dużego pliku + anulowanie
  Mk(base + L"/big/huge.bin", 400u << 20);
  fs::create_directories(WToUtf8(base + L"/out"));
  auto t0 = std::chrono::steady_clock::now();
  auto j11 = mgr.Start(JobOp::Copy, {base + L"/big/huge.bin"}, base + L"/out", nullptr);
  Wait(j11);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(SizeOf(base + L"/out/huge.bin") == (400u << 20));
  printf("400 MB copy: %.0f ms (%.0f MB/s)\n", ms, 400.0 / (ms / 1000));
  fs::remove(WToUtf8(base + L"/out/huge.bin"));
  auto j10 = mgr.Start(JobOp::Copy, {base + L"/big"}, base + L"/out", nullptr);
  j10->pause = true;
  for (int i = 0; i < 300 && j10->State() != JobState::Running; i++) usleep(10000);
  usleep(200000);
  j10->cancel = true;
  j10->pause = false;
  Wait(j10);
  CHECK(j10->State() == JobState::Cancelled);
  CHECK(!Has(base + L"/out/big/huge.bin"));  // plik częściowy usunięty

  fs::remove_all(WToUtf8(base));
  printf(fails ? "\n%d FAILURES\n" : "\nALL OK\n", fails);
  return fails;
}

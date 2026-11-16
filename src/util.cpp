#include "util.h"

#include <clocale>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <locale.h>

namespace nv {

const uint16_t* FoldTable() {
  static const uint16_t* t = [] {
    auto* p = new uint16_t[65536];
    locale_t loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    if (!loc) loc = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
    for (int i = 0; i < 65536; i++) {
      wint_t c = (wint_t)i;
      if (i >= 0xD800 && i < 0xE000) { p[i] = (uint16_t)i; continue; }
      wint_t l = loc ? towlower_l(c, loc) : towlower(c);
      p[i] = (l < 65536) ? (uint16_t)l : (uint16_t)i;
    }
    if (loc) freelocale(loc);
    return p;
  }();
  return t;
}

std::wstring FoldStr(const std::wstring& s) {
  std::wstring r(s);
  const uint16_t* f = FoldTable();
  for (auto& c : r)
    if ((uint32_t)c < 65536u) c = (wchar_t)f[(uint32_t)c];
  return r;
}

int CompareNoCase(const wchar_t* a, size_t an, const wchar_t* b, size_t bn) {
  const uint16_t* f = FoldTable();
  size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; i++) {
    wchar_t x = (uint32_t)a[i] < 65536u ? (wchar_t)f[(uint32_t)a[i]] : a[i];
    wchar_t y = (uint32_t)b[i] < 65536u ? (wchar_t)f[(uint32_t)b[i]] : b[i];
    if (x != y) return x < y ? -1 : 1;
  }
  if (an == bn) return 0;
  return an < bn ? -1 : 1;
}

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
  return a.size() == b.size() && CompareNoCase(a.data(), a.size(), b.data(), b.size()) == 0;
}

static inline bool IsDig(wchar_t c) { return c >= L'0' && c <= L'9'; }

int NaturalCompare(const wchar_t* a, size_t an, const wchar_t* b, size_t bn) {
  size_t i = 0, j = 0;
  while (i < an && j < bn) {
    if (IsDig(a[i]) && IsDig(b[j])) {
      while (i < an && a[i] == L'0') i++;
      while (j < bn && b[j] == L'0') j++;
      size_t is = i, js = j;
      while (i < an && IsDig(a[i])) i++;
      while (j < bn && IsDig(b[j])) j++;
      size_t la = i - is, lb = j - js;
      if (la != lb) return la < lb ? -1 : 1;
      int c = wmemcmp(a + is, b + js, la);
      if (c) return c < 0 ? -1 : 1;
      continue;
    }
    wchar_t fa = Fold(a[i]), fb = Fold(b[j]);
    if (fa != fb) return fa < fb ? -1 : 1;
    i++;
    j++;
  }
  if (i < an) return 1;
  if (j < bn) return -1;
  return 0;
}

// ---------------------------------------------------------------- formatowanie
static std::wstring Dot2Comma(std::wstring s) {
  for (auto& c : s)
    if (c == L'.') c = L',';
  return s;
}

std::wstring FormatSize(u64 b) {
  if (b < 1024) return std::to_wstring(b) + L" B";
  static const wchar_t* U[] = {L"KB", L"MB", L"GB", L"TB", L"PB"};
  double v = (double)b;
  int k = -1;
  while (v >= 1024.0 && k < 4) {
    v /= 1024.0;
    k++;
  }
  wchar_t buf[64];
  int prec = v >= 100 ? 0 : (v >= 10 ? 1 : 2);
  swprintf(buf, 64, L"%.*f", prec, v);
  return Dot2Comma(buf) + L" " + U[k];
}

std::wstring FormatCount(u64 n) {
  std::wstring s = std::to_wstring(n), r;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; i--) {
    r.insert(r.begin(), s[i]);
    if (++c % 3 == 0 && i > 0) r.insert(r.begin(), L' ');
  }
  return r;
}

std::wstring FormatDateTime(i64 ft) {
  if (ft <= 0) return L"";
  wchar_t buf[48];
  time_t t = (time_t)FileTimeToUnix(ft);
  struct tm tmv;
  if (!localtime_r(&t, &tmv)) return L"";
  swprintf(buf, 48, L"%02d.%02d.%04d %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min);
  return buf;
}

std::wstring FormatDuration(double s) {
  if (s < 0 || s > 99 * 3600) return L"--:--";
  int t = (int)(s + 0.5);
  wchar_t buf[32];
  if (t >= 3600)
    swprintf(buf, 32, L"%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
  else
    swprintf(buf, 32, L"%d:%02d", t / 60, t % 60);
  return buf;
}

std::wstring FormatSpeed(double bps) { return FormatSize((u64)bps) + L"/s"; }

// ---------------------------------------------------------------- UTF-8
std::wstring Utf8ToW(const std::string& s) {
  std::wstring r;
  r.reserve(s.size());
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { r.push_back((wchar_t)c); i++; continue; }
    int len = 0;
    uint32_t cp = 0, minv = 0;
    if ((c >> 5) == 6) { len = 2; cp = c & 0x1F; minv = 0x80; }
    else if ((c >> 4) == 14) { len = 3; cp = c & 0x0F; minv = 0x800; }
    else if ((c >> 3) == 30) { len = 4; cp = c & 0x07; minv = 0x10000; }
    bool ok = len > 0 && i + len <= n;
    if (ok)
      for (int k = 1; k < len; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { ok = false; break; }
        cp = (cp << 6) | (cc & 0x3F);
      }
    if (ok && (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))) ok = false;
    if (!ok) {  // surrogateescape
      r.push_back((wchar_t)(0xDC00 + c));
      i++;
      continue;
    }
    r.push_back((wchar_t)cp);
    i += len;
  }
  return r;
}

std::string WToUtf8(const std::wstring& w) {
  std::string r;
  r.reserve(w.size());
  for (size_t i = 0; i < w.size(); i++) {
    uint32_t cp = (uint32_t)w[i];
    if (cp >= 0xDC80 && cp <= 0xDCFF) { r.push_back((char)(cp - 0xDC00)); continue; }
    if (cp < 0x80) r.push_back((char)cp);
    else if (cp < 0x800) { r.push_back((char)(0xC0 | (cp >> 6))); r.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { r.push_back((char)(0xE0 | (cp >> 12))); r.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); r.push_back((char)(0x80 | (cp & 0x3F))); }
    else { r.push_back((char)(0xF0 | (cp >> 18))); r.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); r.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); r.push_back((char)(0x80 | (cp & 0x3F))); }
  }
  return r;
}

// ---------------------------------------------------------------- ścieżki
size_t RootLen(const std::wstring& p) { return !p.empty() && p[0] == L'/' ? 1 : 0; }

std::wstring PathNormalize(std::wstring p) {
  if (p.empty()) return p;
  bool abs = p[0] == L'/';
  std::wstring out;
  out.reserve(p.size());
  size_t i = 0, n = p.size();
  std::vector<std::wstring> parts;
  while (i < n) {
    while (i < n && p[i] == L'/') i++;
    size_t e = i;
    while (e < n && p[e] != L'/') e++;
    if (e > i) {
      std::wstring c = p.substr(i, e - i);
      if (c == L".") {
      } else if (c == L".." && abs) {
        if (!parts.empty()) parts.pop_back();
      } else
        parts.push_back(c);
    }
    i = e;
  }
  if (abs) out = L"/";
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) out += L'/';
    out += parts[k];
  }
  if (out.empty() && !abs) return L".";
  return out;
}

std::wstring PathJoin(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'/') return a + b;
  return a + L"/" + b;
}

std::wstring PathParent(const std::wstring& p) {
  std::wstring q = p;
  while (q.size() > 1 && q.back() == L'/') q.pop_back();
  if (q.empty() || q == L"/") return L"";
  size_t k = q.rfind(L'/');
  if (k == std::wstring::npos) return L"";
  if (k == 0) return L"/";
  return q.substr(0, k);
}

std::wstring PathName(const std::wstring& p) {
  std::wstring q = p;
  while (q.size() > 1 && q.back() == L'/') q.pop_back();
  if (q == L"/") return q;
  size_t k = q.rfind(L'/');
  if (k == std::wstring::npos) return q;
  return q.substr(k + 1);
}

std::wstring PathExt(const std::wstring& name) {
  size_t k = name.rfind(L'.');
  if (k == std::wstring::npos || k == 0 || k + 1 == name.size()) return L"";
  return FoldStr(name.substr(k));
}

bool PathIsRoot(const std::wstring& p) { return p == L"/"; }

bool PathIsUnder(const std::wstring& child, const std::wstring& parent) {
  std::wstring c = PathNormalize(child), p = PathNormalize(parent);
  if (c.size() < p.size() || c.compare(0, p.size(), p) != 0) return false;
  if (c.size() == p.size()) return true;
  return p.back() == L'/' || c[p.size()] == L'/';
}

std::vector<std::wstring> PathSplit(const std::wstring& p) {
  std::vector<std::wstring> r;
  size_t pos = 0;
  while (pos < p.size()) {
    if (p[pos] == L'/') { pos++; continue; }
    size_t e = p.find(L'/', pos);
    if (e == std::wstring::npos) e = p.size();
    r.push_back(p.substr(pos, e - pos));
    pos = e;
  }
  return r;
}

}  // namespace nv

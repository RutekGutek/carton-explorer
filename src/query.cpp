#include "query.h"

#include <ctime>

namespace nv {

static inline wchar_t F(const uint16_t* f, wchar_t c) { return (uint32_t)c < 65536u ? (wchar_t)f[(uint32_t)c] : c; }

bool WildMatch(const wchar_t* pat, size_t pn, const wchar_t* s, size_t sn) {
  const uint16_t* f = FoldTable();
  size_t p = 0, i = 0, star = (size_t)-1, mark = 0;
  while (i < sn) {
    if (p < pn && (pat[p] == L'?' || pat[p] == F(f, s[i]))) {
      p++;
      i++;
    } else if (p < pn && pat[p] == L'*') {
      star = p++;
      mark = i;
    } else if (star != (size_t)-1) {
      p = star + 1;
      i = ++mark;
    } else
      return false;
  }
  while (p < pn && pat[p] == L'*') p++;
  return p == pn;
}

static bool SubMatch(const wchar_t* n, size_t nn, const wchar_t* s, size_t sn) {
  if (nn == 0) return true;
  if (nn > sn) return false;
  const uint16_t* f = FoldTable();
  wchar_t n0 = n[0];
  size_t last = sn - nn;
  for (size_t i = 0; i <= last; i++) {
    if (F(f, s[i]) != n0) continue;
    size_t k = 1;
    while (k < nn && F(f, s[i + k]) == n[k]) k++;
    if (k == nn) return true;
  }
  return false;
}

// ------------------------------------------------------------------ parsing
struct Tok {
  std::wstring text;
  bool quoted = false;
};

static std::vector<Tok> Tokenize(const std::wstring& s) {
  std::vector<Tok> r;
  size_t i = 0, n = s.size();
  while (i < n) {
    while (i < n && iswspace(s[i])) i++;
    if (i >= n) break;
    Tok t;
    bool inq = false;
    while (i < n && (inq || !iswspace(s[i]))) {
      if (s[i] == L'"') {
        inq = !inq;
        t.quoted = true;
      } else
        t.text.push_back(s[i]);
      i++;
    }
    if (!t.text.empty()) r.push_back(t);
  }
  return r;
}

static bool ParseSizeVal(const std::wstring& s, u64& out) {
  size_t i = 0;
  double v = 0;
  bool any = false, frac = false;
  double scale = 0.1;
  while (i < s.size() && ((s[i] >= L'0' && s[i] <= L'9') || s[i] == L'.' || s[i] == L',')) {
    if (s[i] == L'.' || s[i] == L',') {
      if (frac) return false;
      frac = true;
    } else {
      int d = s[i] - L'0';
      if (!frac) v = v * 10 + d;
      else { v += d * scale; scale /= 10; }
      any = true;
    }
    i++;
  }
  if (!any) return false;
  std::wstring u = FoldStr(s.substr(i));
  double mul = 1;
  if (u.empty() || u == L"b") mul = 1;
  else if (u == L"k" || u == L"kb" || u == L"kib") mul = 1024.0;
  else if (u == L"m" || u == L"mb" || u == L"mib") mul = 1024.0 * 1024;
  else if (u == L"g" || u == L"gb" || u == L"gib") mul = 1024.0 * 1024 * 1024;
  else if (u == L"t" || u == L"tb" || u == L"tib") mul = 1024.0 * 1024 * 1024 * 1024;
  else return false;
  out = (u64)(v * mul);
  return true;
}

static bool ParseSizeRange(std::wstring v, u64& lo, u64& hi) {
  lo = 0;
  hi = ~0ull;
  size_t dd = v.find(L"..");
  if (dd != std::wstring::npos) {
    std::wstring a = v.substr(0, dd), b = v.substr(dd + 2);
    if (!a.empty() && !ParseSizeVal(a, lo)) return false;
    if (!b.empty() && !ParseSizeVal(b, hi)) return false;
    return true;
  }
  u64 x;
  if (v.compare(0, 2, L">=") == 0) return ParseSizeVal(v.substr(2), lo);
  if (v.compare(0, 2, L"<=") == 0) return ParseSizeVal(v.substr(2), hi);
  if (v[0] == L'>') { if (!ParseSizeVal(v.substr(1), x)) return false; lo = x + 1; return true; }
  if (v[0] == L'<') { if (!ParseSizeVal(v.substr(1), x)) return false; hi = x ? x - 1 : 0; return true; }
  if (v[0] == L'=') v = v.substr(1);
  if (!ParseSizeVal(v, x)) return false;
  lo = hi = x;
  return true;
}

static i64 DaysFromCivil(i64 y, unsigned m, unsigned d) {
  y -= m <= 2;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (i64)doe - 719468;
}

static i64 UnixToFt(i64 unixSec) { return (unixSec + 11644473600LL) * 10000000LL; }

static i64 LocalOffsetSec() {
  time_t t = time(nullptr);
  struct tm lt = *localtime(&t), gt = *gmtime(&t);
  int d = (lt.tm_hour - gt.tm_hour) * 3600 + (lt.tm_min - gt.tm_min) * 60;
  if (d > 14 * 3600) d -= 86400;
  if (d < -12 * 3600) d += 86400;
  return d;
}

// "2024", "2024-05", "2024-05-17" => [start, end] jako FILETIME (czas lokalny)
static bool ParseDatePeriod(const std::wstring& s, i64& start, i64& end) {
  int y = 0, m = 0, d = 0;
  int n = swscanf(s.c_str(), L"%d-%d-%d", &y, &m, &d);
  if (n < 1 || y < 1601 || y > 9999) return false;
  i64 off = LocalOffsetSec();
  i64 a, b;
  if (n == 1) { a = DaysFromCivil(y, 1, 1); b = DaysFromCivil(y + 1, 1, 1); }
  else if (n == 2) { if (m < 1 || m > 12) return false; a = DaysFromCivil(y, m, 1); b = m == 12 ? DaysFromCivil(y + 1, 1, 1) : DaysFromCivil(y, m + 1, 1); }
  else { if (m < 1 || m > 12 || d < 1 || d > 31) return false; a = DaysFromCivil(y, m, d); b = a + 1; }
  start = UnixToFt(a * 86400 - off);
  end = UnixToFt(b * 86400 - off) - 1;
  return true;
}

static bool ParseDateRange(const std::wstring& vIn, u64& lo, u64& hi) {
  std::wstring v = FoldStr(vIn);
  lo = 0;
  hi = ~0ull;
  time_t now = time(nullptr);
  struct tm lt = *localtime(&now);
  i64 sodUnix = (i64)now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
  i64 startToday = UnixToFt(sodUnix), endNow = UnixToFt((i64)now);
  const i64 DAY = 86400LL * 10000000LL;
  if (v == L"today" || v == L"dzis" || v == L"dziś") { lo = startToday; hi = ~0ull; return true; }
  if (v == L"yesterday" || v == L"wczoraj") { lo = startToday - DAY; hi = startToday - 1; return true; }
  if (v == L"week" || v == L"tydzien" || v == L"tydzień") { lo = endNow - 7 * DAY; return true; }
  if (v == L"month" || v == L"miesiac" || v == L"miesiąc") { lo = endNow - 30 * DAY; return true; }
  if (v == L"year" || v == L"rok") { lo = endNow - 365 * DAY; return true; }
  i64 a, b, c, d;
  size_t dd = v.find(L"..");
  if (dd != std::wstring::npos) {
    std::wstring l = v.substr(0, dd), r = v.substr(dd + 2);
    if (!l.empty()) { if (!ParseDatePeriod(l, a, b)) return false; lo = (u64)a; }
    if (!r.empty()) { if (!ParseDatePeriod(r, c, d)) return false; hi = (u64)d; }
    return true;
  }
  if (v.compare(0, 2, L">=") == 0) { if (!ParseDatePeriod(v.substr(2), a, b)) return false; lo = (u64)a; return true; }
  if (v.compare(0, 2, L"<=") == 0) { if (!ParseDatePeriod(v.substr(2), a, b)) return false; hi = (u64)b; return true; }
  if (v[0] == L'>') { if (!ParseDatePeriod(v.substr(1), a, b)) return false; lo = (u64)b + 1; return true; }
  if (v[0] == L'<') { if (!ParseDatePeriod(v.substr(1), a, b)) return false; hi = (u64)a - 1; return true; }
  if (v[0] == L'=') v = v.substr(1);
  if (!ParseDatePeriod(v, a, b)) return false;
  lo = (u64)a;
  hi = (u64)b;
  return true;
}

static bool HasWild(const std::wstring& s) { return s.find_first_of(L"*?") != std::wstring::npos; }

static void AddPlainAlts(Query& q, std::vector<Alt>& alts, const std::wstring& text, bool quoted, bool neg) {
  std::vector<std::wstring> parts;
  if (quoted) parts.push_back(text);
  else {
    size_t pos = 0;
    for (;;) {
      size_t k = text.find(L'|', pos);
      parts.push_back(text.substr(pos, k == std::wstring::npos ? k : k - pos));
      if (k == std::wstring::npos) break;
      pos = k + 1;
    }
  }
  for (auto& p : parts) {
    if (p.empty()) continue;
    Alt a;
    a.neg = neg;
    std::wstring fp = FoldStr(p);
    if (fp.find_first_of(L"\\/") != std::wstring::npos) {
      for (auto& c : fp)
        if (c == L'\\') c = L'/';
      a.kind = Alt::Path;
      a.wild = HasWild(fp);
      a.text = a.wild ? L"*" + fp + L"*" : fp;
      q.needPath = true;
    } else if (HasWild(fp)) {
      a.kind = Alt::Wild;
      a.text = fp;
    } else {
      a.kind = Alt::Sub;
      a.text = fp;
    }
    if (!neg && q.primary.empty() && a.kind != Alt::Path) {
      q.primary = a.text;
      q.primary.erase(std::remove_if(q.primary.begin(), q.primary.end(), [](wchar_t c) { return c == L'*' || c == L'?'; }), q.primary.end());
    }
    alts.push_back(std::move(a));
  }
}

Query Query::Parse(const std::wstring& text) {
  Query q;
  for (auto& tk : Tokenize(text)) {
    std::wstring t = tk.text;
    bool neg = false;
    if (t.size() > 1 && (t[0] == L'!' || (t[0] == L'-' && !tk.quoted))) {
      neg = true;
      t = t.substr(1);
    }
    std::vector<Alt> alts;
    size_t colon = t.find(L':');
    std::wstring key, val;
    if (colon != std::wstring::npos && colon > 0 && !tk.quoted) {
      key = FoldStr(t.substr(0, colon));
      val = t.substr(colon + 1);
    }
    bool handled = true;
    if (key == L"ext" || key == L"rozszerzenie") {
      Alt a;
      a.kind = Alt::Ext;
      a.neg = neg;
      size_t pos = 0;
      while (pos <= val.size()) {
        size_t k = val.find_first_of(L";,|", pos);
        std::wstring e = FoldStr(val.substr(pos, k == std::wstring::npos ? k : k - pos));
        if (!e.empty() && e[0] == L'.') e = e.substr(1);
        if (!e.empty()) a.exts.push_back(e);
        if (k == std::wstring::npos) break;
        pos = k + 1;
      }
      if (a.exts.empty()) { q.error = L"ext: wymaga rozszerzenia"; continue; }
      alts.push_back(a);
      q.terms.push_back(alts);
      continue;
    } else if (key == L"size" || key == L"rozmiar") {
      Alt a;
      a.kind = Alt::Size;
      a.neg = neg;
      if (!ParseSizeRange(val, a.lo, a.hi)) { q.error = L"Niepoprawny rozmiar: " + val; continue; }
      q.needAttr = true;
      alts.push_back(a);
      q.terms.push_back(alts);
      continue;
    } else if (key == L"dm" || key == L"modified" || key == L"date" || key == L"data") {
      Alt a;
      a.kind = Alt::Date;
      a.neg = neg;
      if (!ParseDateRange(val, a.lo, a.hi)) { q.error = L"Niepoprawna data: " + val; continue; }
      q.needAttr = true;
      alts.push_back(a);
      q.terms.push_back(alts);
      continue;
    } else if (key == L"folder" || key == L"dir" || key == L"folders" || key == L"file" || key == L"files") {
      Alt a;
      a.kind = (key[0] == L'f' && key.size() >= 4 && key[1] == L'i') ? Alt::IsFile : Alt::IsDir;
      a.neg = neg;
      alts.push_back(a);
      q.terms.push_back(alts);
      alts.clear();
      if (!val.empty()) {
        AddPlainAlts(q, alts, val, false, false);
        if (!alts.empty()) q.terms.push_back(alts);
      }
      continue;
    } else if (key == L"path" || key == L"in" || key == L"sciezka" || key == L"ścieżka") {
      Alt a;
      a.kind = Alt::Path;
      a.neg = neg;
      std::wstring fp = FoldStr(val);
      for (auto& c : fp)
        if (c == L'\\') c = L'/';
      if (fp.empty()) { q.error = L"path: wymaga tekstu"; continue; }
      a.wild = HasWild(fp);
      a.text = a.wild ? L"*" + fp + L"*" : fp;
      q.needPath = true;
      alts.push_back(a);
      q.terms.push_back(alts);
      continue;
    } else if (key == L"re" || key == L"regex") {
      Alt a;
      a.kind = Alt::Regex;
      a.neg = neg;
      try {
        a.re = std::make_shared<std::wregex>(val, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
      } catch (...) {
        q.error = L"Niepoprawne wyrażenie regularne";
        continue;
      }
      alts.push_back(a);
      q.terms.push_back(alts);
      continue;
    } else
      handled = false;
    (void)handled;
    AddPlainAlts(q, alts, t, tk.quoted, neg);
    if (!alts.empty()) q.terms.push_back(alts);
  }
  return q;
}

// ------------------------------------------------------------------ dopasowanie
static bool EvalAlt(const Alt& a, const wchar_t* name, size_t len, bool isDir, u64 size, i64 mtime, Query::PathFn pf, void* pctx) {
  bool r = false;
  switch (a.kind) {
    case Alt::Sub: r = SubMatch(a.text.data(), a.text.size(), name, len); break;
    case Alt::Wild: r = WildMatch(a.text.data(), a.text.size(), name, len); break;
    case Alt::Path: {
      const std::wstring& p = pf(pctx);
      if (a.wild) r = WildMatch(a.text.data(), a.text.size(), p.data(), p.size());
      else r = SubMatch(a.text.data(), a.text.size(), p.data(), p.size());
      break;
    }
    case Alt::Regex: r = std::regex_search(name, name + len, *a.re); break;
    case Alt::Ext: {
      if (isDir) { r = false; break; }
      size_t k = len;
      while (k > 0 && name[k - 1] != L'.') k--;
      if (k == 0 || k == len) { r = false; break; }
      const wchar_t* e = name + k;
      size_t el = len - k;
      for (auto& x : a.exts)
        if (x.size() == el && CompareNoCase(x.data(), x.size(), e, el) == 0) { r = true; break; }
      break;
    }
    case Alt::Size: r = size >= a.lo && size <= a.hi; break;
    case Alt::Date: r = mtime > 0 && (u64)mtime >= a.lo && (u64)mtime <= a.hi; break;
    case Alt::IsDir: r = isDir; break;
    case Alt::IsFile: r = !isDir; break;
  }
  return r != a.neg;
}

bool Query::Match(const wchar_t* name, size_t len, bool isDir, u64 size, i64 mtime, PathFn pf, void* pctx) const {
  for (auto& term : terms) {
    bool any = false;
    for (auto& a : term)
      if (EvalAlt(a, name, len, isDir, size, mtime, pf, pctx)) { any = true; break; }
    if (!any) return false;
  }
  return true;
}

int Query::Score(const wchar_t* name, size_t len) const {
  if (primary.empty()) return 4;
  std::wstring n = FoldStr(std::wstring(name, len));
  if (n == primary) return 0;
  if (n.compare(0, primary.size(), primary) == 0) return 1;
  size_t k = n.find(primary);
  if (k == std::wstring::npos) return 4;
  wchar_t pc = n[k - 1];
  if (pc == L' ' || pc == L'_' || pc == L'-' || pc == L'.' || pc == L'(' || pc == L'[') return 2;
  return 3;
}

}  // namespace nv

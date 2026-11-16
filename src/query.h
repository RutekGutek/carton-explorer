// Parser i silnik dopasowania zapytań:
//   tekst            - podciąg w nazwie (bez rozróżniania wielkości liter)
//   *.txt  test_*.png a?c   - wildcardy dopasowane do całej nazwy
//   a b              - AND;   a|b - OR;   !a lub -a - negacja;   "a b" - fraza
//   ext:txt;md       - rozszerzenia
//   size:>10mb  size:1kb..5mb
//   dm:today|yesterday|week|month|year|2024|2024-05|>2024-01-01|2024-01-01..2024-02-01
//   folder: / file:  - tylko foldery / pliki      (np. folder:photos)
//   path:abc  lub zawierające / (albo \) - dopasowanie do pełnej ścieżki
//   re:^img_\d+      - wyrażenie regularne na nazwie
#pragma once
#include <memory>
#include <regex>

#include "util.h"

namespace nv {

struct Alt {
  enum Kind { Sub, Wild, Path, Regex, Ext, Size, Date, IsDir, IsFile } kind = Sub;
  bool neg = false;
  bool wild = false;  // dla Path
  std::wstring text;  // po Fold()
  std::vector<std::wstring> exts;
  u64 lo = 0, hi = ~0ull;
  std::shared_ptr<std::wregex> re;
};

struct Query {
  std::vector<std::vector<Alt>> terms;  // AND z OR-ów
  bool needPath = false, needAttr = false;
  std::wstring error;
  std::wstring primary;  // do rankingu

  static Query Parse(const std::wstring& text);
  bool Empty() const { return terms.empty(); }

  typedef const std::wstring& (*PathFn)(void*);
  bool Match(const wchar_t* name, size_t len, bool isDir, u64 size, i64 mtime, PathFn pf, void* pctx) const;
  int Score(const wchar_t* name, size_t len) const;  // 0 = najlepsze dopasowanie
};

bool WildMatch(const wchar_t* pat, size_t pn, const wchar_t* s, size_t sn);  // pat musi być po Fold()

}  // namespace nv

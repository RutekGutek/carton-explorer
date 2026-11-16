// Carton Explorer (Linux) - wspólne narzędzia (bez zależności od Qt, żeby rdzeń dało się testować wszędzie)
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace nv {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

constexpr u32 NONE = 0xFFFFFFFFu;

// atrybuty (wartości zgodne z oryginałem: bity FILE_ATTRIBUTE_*)
constexpr u32 A_RO = 0x1, A_HIDDEN = 0x2, A_SYSTEM = 0x4, A_DIR = 0x10, A_REPARSE = 0x400;
constexpr u32 TAG_SYMLINK = 0xA000000C, TAG_MOUNT = 0xA0000003;

// ---- porównywanie znaków bez rozróżniania wielkości liter (tablica dla BMP) ----
const uint16_t* FoldTable();
inline wchar_t Fold(wchar_t c) { return (uint32_t)c < 65536u ? (wchar_t)FoldTable()[(uint32_t)c] : c; }

std::wstring FoldStr(const std::wstring& s);
int CompareNoCase(const wchar_t* a, size_t an, const wchar_t* b, size_t bn);
bool EqualNoCase(const std::wstring& a, const std::wstring& b);
int NaturalCompare(const wchar_t* a, size_t an, const wchar_t* b, size_t bn);

// ---- formatowanie (po polsku) ----
std::wstring FormatSize(u64 bytes);
std::wstring FormatCount(u64 n);
std::wstring FormatDateTime(i64 filetime);  // FILETIME: 100 ns od 1601 (jak w oryginale)
std::wstring FormatDuration(double seconds);
std::wstring FormatSpeed(double bytesPerSec);

// UTF-8 <-> wstring (wchar_t = UTF-32). Niepoprawne bajty nazw plików są zachowywane
// jako U+DC80..U+DCFF (jak "surrogateescape"), więc nazwy zawsze dają się odtworzyć 1:1.
std::wstring Utf8ToW(const std::string& s);
std::string WToUtf8(const std::wstring& s);

inline i64 UnixToFileTime(i64 sec, long nsec = 0) { return (sec + 11644473600LL) * 10000000LL + nsec / 100; }
inline i64 FileTimeToUnix(i64 ft) { return ft / 10000000LL - 11644473600LL; }

// ---- ścieżki POSIX (separator: '/') ----
constexpr wchar_t SEP = L'/';
std::wstring PathNormalize(std::wstring p);  // usuwa podwójne/końcowe '/', "." ; zostawia ".." (bez rozwiązywania)
std::wstring PathJoin(const std::wstring& a, const std::wstring& b);
std::wstring PathParent(const std::wstring& p);  // "/a/b" -> "/a", "/a" -> "/", "/" -> L"" (= "Ten komputer")
std::wstring PathName(const std::wstring& p);    // ostatni element; dla "/" -> "/"
std::wstring PathExt(const std::wstring& name);  // ".txt" (małe litery) lub L""
size_t RootLen(const std::wstring& p);           // 1 dla ścieżek bezwzględnych, inaczej 0
bool PathIsRoot(const std::wstring& p);          // "/"
bool PathIsUnder(const std::wstring& child, const std::wstring& parent);  // child w środku parent (lub równy), z uwzględnieniem wielkości liter
std::vector<std::wstring> PathSplit(const std::wstring& p);  // składniki bez roota

}  // namespace nv

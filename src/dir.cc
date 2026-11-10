// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "dir.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <endian.h>
#include <sys/syscall.h>
#endif

#ifdef __APPLE__
#include <iconv.h>
#endif

#include "bits.h"
#include "check.h"
#include "scope_guard.h"
#include "string_cmp.h"
#include "tribool.h"

namespace gitstatus {

namespace {

template <class Char>
bool Dots(const Char* name) {
  if (name[0] == '.') {
    if (name[1] == 0) return true;
    if (name[1] == '.' && name[2] == 0) return true;
  }
  return false;
}

}  // namespace

#ifdef _WIN32

namespace {

Stat ToStat(DWORD attrs, DWORD reparse_tag, int64_t mtime, int64_t size) {
  // FILETIME counts 100ns intervals since 1601. Git for Windows converts it the same way.
  int64_t t = mtime - 116444736000000000;
  Stat st = {};
  st.st_mtim.tv_sec = t / 10000000;
  st.st_mtim.tv_nsec = t % 10000000 * 100;
  st.st_size = size;
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && reparse_tag == IO_REPARSE_TAG_SYMLINK) {
    st.st_mode = S_IFLNK;
  } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    st.st_mode = S_IFDIR;
  } else {
    st.st_mode = S_IFREG | 0644;
  }
  return st;
}

int64_t Int64(DWORD high, DWORD low) { return static_cast<int64_t>(uint64_t{high} << 32 | low); }

}  // namespace

void AppendWidePath(std::wstring& out, StringView path) {
  if (!path.len) return;
  int n = MultiByteToWideChar(CP_UTF8, 0, path.ptr, static_cast<int>(path.len), nullptr, 0);
  CHECK(n > 0) << GetLastError();
  size_t pos = out.size();
  out.resize(pos + n);
  MultiByteToWideChar(CP_UTF8, 0, path.ptr, static_cast<int>(path.len), &out[pos], n);
  std::replace(out.begin() + pos, out.end(), L'/', L'\\');
  // "\\?\" paths are not normalized, so an empty path component would not resolve.
  auto Seps = [](wchar_t a, wchar_t b) { return a == L'\\' && b == L'\\'; };
  out.erase(std::unique(out.begin() + (pos ? pos - 1 : 0), out.end(), Seps), out.end());
}

std::wstring WidePath(const char* path) {
  std::wstring res = L"\\\\?\\";
  if ((path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\')) {
    // //server/share becomes \\?\UNC\server\share.
    res += L"UNC";
    ++path;
  }
  AppendWidePath(res, StringView(path));
  return res;
}

bool ListDir(const std::wstring& dir, Arena& arena, std::vector<char*>& entries,
             bool case_sensitive, DirId* id) {
  const size_t orig_size = entries.size();
  HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  ON_SCOPE_EXIT(&) { CHECK(CloseHandle(h)) << GetLastError(); };

  BY_HANDLE_FILE_INFORMATION info;
  if (id && !GetFileInformationByHandle(h, &info)) return false;

  alignas(8) static thread_local char buf[64 << 10];
  while (GetFileInformationByHandleEx(h, FileFullDirectoryInfo, buf, sizeof(buf))) {
    for (size_t pos = 0;;) {
      const auto& data = *reinterpret_cast<const FILE_FULL_DIR_INFO*>(buf + pos);
      const int wlen = static_cast<int>(data.FileNameLength / sizeof(wchar_t));
      const wchar_t* wname = data.FileName;
      if (!(wname[0] == L'.' && (wlen == 1 || (wlen == 2 && wname[1] == L'.')))) {
        // Each UTF-16 code unit becomes at most 3 bytes of UTF-8.
        int cap = 3 * wlen;
        char* p = static_cast<char*>(arena.Allocate(sizeof(Stat) + 1 + cap + 1, alignof(Stat)));
        // For reparse points EaSize holds the reparse tag.
        Stat st = ToStat(data.FileAttributes, data.EaSize, data.LastWriteTime.QuadPart,
                         data.EndOfFile.QuadPart);
        std::memcpy(p, &st, sizeof(st));
        char* name = p + sizeof(Stat) + 1;
        int n = WideCharToMultiByte(CP_UTF8, 0, wname, wlen, name, cap, nullptr, nullptr);
        CHECK(n > 0) << GetLastError();
        name[n] = 0;
        name[-1] = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_REG;
        entries.push_back(name);
      }
      if (!data.NextEntryOffset) break;
      pos += data.NextEntryOffset;
    }
  }

  if (GetLastError() != ERROR_NO_MORE_FILES) {
    entries.resize(orig_size);
    return false;
  }
  StrSort(entries.data() + orig_size, entries.data() + entries.size(), case_sensitive);
  if (id) {
    id->volume = info.dwVolumeSerialNumber;
    id->file = static_cast<uint64_t>(Int64(info.nFileIndexHigh, info.nFileIndexLow));
  }
  return true;
}

const Stat& EntryStat(const char* entry) {
  return *reinterpret_cast<const Stat*>(entry - 1 - sizeof(Stat));
}

bool LStat(const char* path, Stat& st) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(WidePath(path).c_str(), GetFileExInfoStandard, &data)) return false;
  st = ToStat(data.dwFileAttributes, 0,
              Int64(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime),
              Int64(data.nFileSizeHigh, data.nFileSizeLow));
  return true;
}

bool FStat(void* handle, Stat& st) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(handle, &info)) return false;
  st = ToStat(info.dwFileAttributes, 0,
              Int64(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
              Int64(info.nFileSizeHigh, info.nFileSizeLow));
  return true;
}

// The linux-specific implementation is about 20% faster than the generic (posix) implementation.
#elif defined(__linux__)

uint64_t Read64(const void* p) {
  uint64_t res;
  std::memcpy(&res, p, 8);
  return res;
}

void Write64(uint64_t x, void* p) { std::memcpy(p, &x, 8); }

void SwapBytes(char** begin, char** end) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  for (; begin != end; ++begin) Write64(__builtin_bswap64(Read64(*begin)), *begin);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "sorry, not implemented"
#endif
}

template <bool kCaseSensitive>
void SortEntries(char** begin, char** end) {
  static_assert(kCaseSensitive, "");
  SwapBytes(begin, end);
  std::sort(begin, end, [](const char* a, const char* b) {
    uint64_t x = Read64(a);
    uint64_t y = Read64(b);
    // Add 5 for good luck.
    return x < y || (x == y && std::memcmp(a + 5, b + 5, 256) < 0);
  });
  SwapBytes(begin, end);
}

template <>
void SortEntries<false>(char** begin, char** end) {
  std::sort(begin, end, StrLt<false>());
}

bool ListDir(int dir_fd, Arena& arena, std::vector<char*>& entries, bool precompose_unicode,
             bool case_sensitive) {
  struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
  };

  constexpr size_t kBufSize = 8 << 10;
  const size_t orig_size = entries.size();

  while (true) {
    char* buf = static_cast<char*>(arena.Allocate(kBufSize, alignof(linux_dirent64)));
    // Save 256 bytes for the rainy day.
    int n = syscall(SYS_getdents64, dir_fd, buf, kBufSize - 256);
    if (n < 0) {
      entries.resize(orig_size);
      return false;
    }
    for (int pos = 0; pos < n;) {
      auto* ent = reinterpret_cast<linux_dirent64*>(buf + pos);
      if (!Dots(ent->d_name)) entries.push_back(ent->d_name);
      pos += ent->d_reclen;
    }
    if (n == 0) break;
    // The following optimization relies on SYS_getdents64 always returning as many
    // entries as would fit. This is not guaranteed by the specification and I don't
    // know if this is true in practice. The optimization has no measurable effect on
    // gitstatus performance, so it's turned off.
    //
    //   if (n + sizeof(linux_dirent64) + 512 <= kBufSize) break;
  }

  if (case_sensitive) {
    SortEntries<true>(entries.data() + orig_size, entries.data() + entries.size());
  } else {
    SortEntries<false>(entries.data() + orig_size, entries.data() + entries.size());
  }

  return true;
}

#else  // __linux__

namespace {

char* DirentDup(Arena& arena, const struct dirent& ent, size_t len) {
  char* p = arena.Allocate<char>(len + 2);
  *p++ = ent.d_type;
  std::memcpy(p, ent.d_name, len + 1);
  return p;
}

#ifdef __APPLE__

std::atomic<bool> g_iconv_error(true);

Tribool IConvTry(char* inp, size_t ins, char* outp, size_t outs) {
  if (outs == 0) return Tribool::kUnknown;
  iconv_t ic = iconv_open("UTF-8", "UTF-8-MAC");
  if (ic == (iconv_t)-1) {
    if (g_iconv_error.load(std::memory_order_relaxed) &&
        g_iconv_error.exchange(false, std::memory_order_relaxed)) {
      LOG(ERROR) << "iconv_open(\"UTF-8\", \"UTF-8-MAC\") failed";
    }
    return Tribool::kFalse;
  }
  ON_SCOPE_EXIT(&) { CHECK(iconv_close(ic) == 0) << Errno(); };
  --outs;
  if (iconv(ic, &inp, &ins, &outp, &outs) >= 0) {
    *outp = 0;
    return Tribool::kTrue;
  }
  return errno == E2BIG ? Tribool::kUnknown : Tribool::kFalse;
}

char* DirenvConvert(Arena& arena, struct dirent& ent, bool do_convert) {
  if (!do_convert) return DirentDup(arena, ent, std::strlen(ent.d_name));

  size_t len = 0;
  do_convert = false;
  for (unsigned char c; (c = ent.d_name[len]); ++len) {
    if (c & 0x80) do_convert = true;
  }
  if (!do_convert) return DirentDup(arena, ent, len);

  size_t n = NextPow2(len + 2);
  while (true) {
    char* p = arena.Allocate<char>(n);
    switch (IConvTry(ent.d_name, len, p + 1, n - 1)) {
      case Tribool::kFalse:
        return DirentDup(arena, ent, len);
      case Tribool::kTrue:
        *p = ent.d_type;
        return p + 1;
      case Tribool::kUnknown:
        break;
    }
    n *= 2;
  }
}

#else  // __APPLE__

char* DirenvConvert(Arena& arena, struct dirent& ent, bool do_convert) {
  return DirentDup(arena, ent, std::strlen(ent.d_name));
}

#endif  // __APPLE__

}  // namespace

bool ListDir(int dir_fd, Arena& arena, std::vector<char*>& entries, bool precompose_unicode,
             bool case_sensitive) {
  const size_t orig_size = entries.size();
  dir_fd = dup(dir_fd);
  if (dir_fd < 0) return false;
  DIR* dir = fdopendir(dir_fd);
  if (!dir) {
    CHECK(!close(dir_fd)) << Errno();
    return false;
  }
  ON_SCOPE_EXIT(&) { CHECK(!closedir(dir)) << Errno(); };
  while (struct dirent* ent = (errno = 0, readdir(dir))) {
    if (Dots(ent->d_name)) continue;
    entries.push_back(DirenvConvert(arena, *ent, precompose_unicode));
  }
  if (errno) {
    entries.resize(orig_size);
    return false;
  }
  StrSort(entries.data() + orig_size, entries.data() + entries.size(), case_sensitive);
  return true;
}

#endif  // __linux__

}  // namespace gitstatus

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

#ifdef _WIN32

#include "platform.h"

#include <windows.h>
#include <winternl.h>

#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <string>

#include "dir.h"
#include "string_cmp.h"

#pragma comment(lib, "ntdll.lib")

namespace gitstatus {

namespace {

// Not in the SDK's user-mode headers (they live in the DDK's ntifs.h).
const FILE_INFORMATION_CLASS kFileFullDirectoryInformation =
    static_cast<FILE_INFORMATION_CLASS>(2);

struct FileFullDirInformation {
  ULONG NextEntryOffset;
  ULONG FileIndex;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER EndOfFile;
  LARGE_INTEGER AllocationSize;
  ULONG FileAttributes;
  ULONG FileNameLength;
  // For reparse points this holds the reparse tag.
  ULONG EaSize;
  WCHAR FileName[1];
};

extern "C" NTSYSAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event,
                                                        PIO_APC_ROUTINE ApcRoutine,
                                                        PVOID ApcContext,
                                                        PIO_STATUS_BLOCK IoStatusBlock,
                                                        PVOID FileInformation, ULONG Length,
                                                        FILE_INFORMATION_CLASS FileInformationClass,
                                                        BOOLEAN ReturnSingleEntry,
                                                        PUNICODE_STRING FileName,
                                                        BOOLEAN RestartScan);

constexpr NTSTATUS kStatusNoMoreFiles = static_cast<NTSTATUS>(0x80000006L);

// UTF-8 to UTF-16. Separators become '\', which NT relative names require.
bool Widen(const char* s, std::wstring& out) {
  out.clear();
  if (!*s) return true;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
  if (n <= 0) return false;
  out.resize(n);
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, &out[0], n) != n) return false;
  out.pop_back();
  for (wchar_t& c : out) {
    if (c == L'/') c = L'\\';
  }
  return true;
}

int ErrnoFromWin32(DWORD err) {
  switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_DIRECTORY:
      return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY:
      return ENOMEM;
    default:
      return EIO;
  }
}

constexpr ULONG kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// The equivalent of openat(). An empty name reopens parent itself.
NTSTATUS OpenAt(HANDLE parent, const std::wstring& name, ACCESS_MASK access, ULONG options,
                HANDLE* out) {
  UNICODE_STRING uname;
  uname.Buffer = const_cast<PWSTR>(name.c_str());
  uname.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  uname.MaximumLength = uname.Length;
  OBJECT_ATTRIBUTES attrs;
  InitializeObjectAttributes(&attrs, &uname, OBJ_CASE_INSENSITIVE, parent, nullptr);
  IO_STATUS_BLOCK iosb;
  return NtCreateFile(out, access | SYNCHRONIZE, &attrs, &iosb, nullptr, 0, kShareAll, FILE_OPEN,
                      options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT,
                      nullptr, 0);
}

constexpr ACCESS_MASK kDirAccess = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES;

// Unix time from FILETIME, as in libgit2's git_win32__filetime_to_timespec().
struct timespec ToTimespec(LARGE_INTEGER ft) {
  constexpr std::int64_t kEpoch = 116444736000000000LL;  // 1970-01-01 in FILETIME units
  std::int64_t t = ft.QuadPart - kEpoch;
  struct timespec res;
  res.tv_sec = static_cast<time_t>(t / 10000000);
  res.tv_nsec = static_cast<long>((t % 10000000) * 100);
  return res;
}

bool IsSymlinkTag(ULONG tag) {
  // Git for Windows reports both symbolic links and junctions as symlinks.
  return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

mode_t Mode(DWORD attrs, ULONG reparse_tag) {
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && IsSymlinkTag(reparse_tag)) return S_IFLNK | 0777;
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) return S_IFDIR | 0755;
  return S_IFREG | 0644;
}

bool StatHandle(HANDLE h, Stat* st) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) return false;
  ULONG tag = 0;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    FILE_ATTRIBUTE_TAG_INFO tag_info;
    if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
      tag = tag_info.ReparseTag;
    }
  }
  LARGE_INTEGER mtime;
  mtime.LowPart = info.ftLastWriteTime.dwLowDateTime;
  mtime.HighPart = static_cast<LONG>(info.ftLastWriteTime.dwHighDateTime);
  st->st_mode = Mode(info.dwFileAttributes, tag);
  st->st_size = (std::int64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
  st->st_ino = (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
  st->st_mtim = ToTimespec(mtime);
  return true;
}

HANDLE OpenPath(const char* path, DWORD access, DWORD flags) {
  std::wstring wpath;
  if (!Widen(path, wpath)) return INVALID_HANDLE_VALUE;
  return CreateFileW(wpath.c_str(), access, kShareAll, nullptr, OPEN_EXISTING,
                     flags | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool StatPathImpl(const char* path, Stat* st, DWORD flags) {
  HANDLE h = OpenPath(path, FILE_READ_ATTRIBUTES, flags);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool res = StatHandle(h, st);
  CHECK(CloseHandle(h));
  return res;
}

}  // namespace

DirFd OpenDir(const char* path) {
  HANDLE h = OpenPath(path, kDirAccess, 0);
  if (h == INVALID_HANDLE_VALUE) return kNoDir;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    CHECK(CloseHandle(h));
    return kNoDir;
  }
  return h;
}

DirFd OpenDirAt(DirFd parent, const char* name) {
  // OpenTail() passes "." for the root itself.
  if (name[0] == '.' && name[1] == 0) name = "";
  std::wstring wname;
  if (!Widen(name, wname)) return kNoDir;
  HANDLE h;
  if (!NT_SUCCESS(OpenAt(parent, wname, kDirAccess, FILE_DIRECTORY_FILE, &h))) return kNoDir;
  return h;
}

// A fresh handle rather than DuplicateHandle(): duplicates share the directory enumeration
// position, which would make concurrent ListDir() calls interfere.
DirFd DupDir(DirFd dir) {
  HANDLE h;
  if (!NT_SUCCESS(OpenAt(dir, std::wstring(), kDirAccess, FILE_DIRECTORY_FILE, &h))) {
    return kNoDir;
  }
  return h;
}

void CloseDir(DirFd dir) { CHECK(CloseHandle(dir)) << GetLastError(); }

int LStatAt(DirFd dir, const char* name, Stat* st) {
  std::wstring wname;
  if (!Widen(name, wname)) return ENOENT;
  HANDLE h;
  NTSTATUS status = OpenAt(dir, wname, FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT, &h);
  if (!NT_SUCCESS(status)) return ErrnoFromWin32(RtlNtStatusToDosError(status));
  bool ok = StatHandle(h, st);
  CHECK(CloseHandle(h));
  return ok ? 0 : EIO;
}

bool StatDir(DirFd dir, Stat* st) { return StatHandle(dir, st); }

bool StatPath(const char* path, Stat* st) { return StatPathImpl(path, st, 0); }

bool LStatPath(const char* path, Stat* st) {
  return StatPathImpl(path, st, FILE_FLAG_OPEN_REPARSE_POINT);
}

bool StatFile(int fd, Stat* st) {
  intptr_t h = _get_osfhandle(fd);
  return h != -1 && StatHandle(reinterpret_cast<HANDLE>(h), st);
}

int OpenFile(const char* path) {
  HANDLE h = OpenPath(path, GENERIC_READ, 0);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
  if (fd < 0) CHECK(CloseHandle(h));
  return fd;
}

bool ListDir(DirFd dir, Arena& arena, std::vector<char*>& entries, bool precompose_unicode,
             bool case_sensitive) {
  (void)precompose_unicode;  // macOS-only
  const size_t orig_size = entries.size();
  alignas(8) static thread_local char buf[64 << 10];

  for (BOOLEAN restart = TRUE;; restart = FALSE) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = NtQueryDirectoryFile(dir, nullptr, nullptr, nullptr, &iosb, buf, sizeof(buf),
                                           kFileFullDirectoryInformation, FALSE, nullptr, restart);
    if (status == kStatusNoMoreFiles) break;
    if (!NT_SUCCESS(status)) {
      entries.resize(orig_size);
      return false;
    }
    for (const char* p = buf;;) {
      auto* ent = reinterpret_cast<const FileFullDirInformation*>(p);
      const int wlen = static_cast<int>(ent->FileNameLength / sizeof(WCHAR));
      const WCHAR* wname = ent->FileName;
      bool dots = wname[0] == L'.' && (wlen == 1 || (wlen == 2 && wname[1] == L'.'));
      if (!dots) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wname, wlen, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
          entries.resize(orig_size);
          return false;
        }
        // Layout expected by callers: d_type at -1, then the NUL-terminated name.
        char* e = arena.Allocate<char>(len + 2);
        bool is_link = (ent->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                       IsSymlinkTag(ent->EaSize);
        *e++ = is_link                                          ? DT_LNK
               : (ent->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR
                                                                  : DT_REG;
        WideCharToMultiByte(CP_UTF8, 0, wname, wlen, e, len, nullptr, nullptr);
        e[len] = 0;
        entries.push_back(e);
      }
      if (!ent->NextEntryOffset) break;
      p += ent->NextEntryOffset;
    }
  }

  StrSort(entries.data() + orig_size, entries.data() + entries.size(), case_sensitive);
  return true;
}

}  // namespace gitstatus

#endif  // _WIN32

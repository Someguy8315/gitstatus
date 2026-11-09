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

#ifndef ROMKATV_GITSTATUS_PLATFORM_H_
#define ROMKATV_GITSTATUS_PLATFORM_H_

// The filesystem primitives that differ between POSIX and native Windows.
//
// On POSIX these are inline wrappers around the syscalls gitstatusd always used. On Windows
// (clang-cl, MSVC ABI) directories are NT handles and relative opens go through NtCreateFile
// with a root directory handle, which is the Windows equivalent of openat(). Paths are UTF-8
// and may use '/' as the separator on both.

#include <cstddef>
#include <cstdint>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32

#include <io.h>  // POSIX-named read() and close() on CRT file descriptors
#include <string.h>

using ssize_t = std::ptrdiff_t;
using mode_t = unsigned int;

#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

// Values of the d_type byte that ListDir() stores before every entry.
enum : unsigned char { DT_UNKNOWN = 0, DT_DIR = 4, DT_REG = 8, DT_LNK = 10 };

// ASCII-only, like strcasecmp() in the C locale and like libgit2's index ordering.
inline int strcasecmp(const char* x, const char* y) { return _stricmp(x, y); }
inline int strncasecmp(const char* x, const char* y, size_t n) { return _strnicmp(x, y, n); }

#else  // _WIN32

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#endif  // _WIN32

#include "check.h"

namespace gitstatus {

#ifdef _WIN32

// The subset of struct stat that gitstatusd uses. Filled from BY_HANDLE_FILE_INFORMATION the
// same way libgit2 and Git for Windows fill struct stat, so that mtimes match index entries.
struct Stat {
  mode_t st_mode;
  std::int64_t st_size;
  std::uint64_t st_ino;
  struct timespec st_mtim;
};

// An NT directory handle (HANDLE), or kNoDir.
using DirFd = void*;
constexpr DirFd kNoDir = nullptr;

DirFd OpenDir(const char* path);
DirFd OpenDirAt(DirFd parent, const char* name);
DirFd DupDir(DirFd dir);
void CloseDir(DirFd dir);

// Returns 0 on success or an errno value (ENOENT if the file doesn't exist). Doesn't follow
// symlinks.
int LStatAt(DirFd dir, const char* name, Stat* st);
bool StatDir(DirFd dir, Stat* st);
bool StatPath(const char* path, Stat* st);
bool LStatPath(const char* path, Stat* st);
// fd is a CRT file descriptor.
bool StatFile(int fd, Stat* st);
int OpenFile(const char* path);

#else  // _WIN32

using Stat = struct stat;

using DirFd = int;
constexpr DirFd kNoDir = -1;

inline DirFd OpenDir(const char* path) { return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); }

inline DirFd OpenDirAt(DirFd parent, const char* name) {
  return openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

inline DirFd DupDir(DirFd dir) { return dup(dir); }

inline void CloseDir(DirFd dir) { CHECK(!close(dir)) << Errno(); }

inline int LStatAt(DirFd dir, const char* name, Stat* st) {
  return fstatat(dir, name, st, AT_SYMLINK_NOFOLLOW) ? errno : 0;
}

inline bool StatDir(DirFd dir, Stat* st) { return !fstat(dir, st); }
inline bool StatPath(const char* path, Stat* st) { return !stat(path, st); }
inline bool LStatPath(const char* path, Stat* st) { return !lstat(path, st); }
inline bool StatFile(int fd, Stat* st) { return !fstat(fd, st); }
inline int OpenFile(const char* path) { return open(path, O_RDONLY | O_CLOEXEC); }

#endif  // _WIN32

// True for "/abs" and, on Windows, also for "C:/abs" and "//server/share".
inline bool IsAbsPath(const char* path) {
#ifdef _WIN32
  if (((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':' &&
      (path[2] == '/' || path[2] == '\\')) {
    return true;
  }
#endif
  return path[0] == '/';
}

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_PLATFORM_H_

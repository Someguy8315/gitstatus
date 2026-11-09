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

#include "check_dir_mtime.h"

#ifdef _WIN32
#include <windows.h>

#include <direct.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "check.h"
#include "dir.h"
#include "logging.h"
#include "print.h"
#include "scope_guard.h"
#include "stat.h"

namespace gitstatus {

namespace {

constexpr char kDirPrefix[] = ".gitstatus.";

#ifdef _WIN32
// Read-only files cannot be deleted on Windows.
constexpr int kTouchMode = _S_IREAD | _S_IWRITE;
int MkDir(const char* path) { return _mkdir(path); }
char* MkdTemp(char* tmpl) {
  return !_mktemp_s(tmpl, std::strlen(tmpl) + 1) && !_mkdir(tmpl) ? tmpl : nullptr;
}
void SleepOneSecond() { Sleep(1000); }
#else
constexpr int kTouchMode = 0444;
int MkDir(const char* path) { return mkdir(path, 0755); }
char* MkdTemp(char* tmpl) { return mkdtemp(tmpl); }
void SleepOneSecond() {
  while (sleep(1)) {
    // zzzz
  }
}
#endif

void Touch(const char* path) {
  int fd = creat(path, kTouchMode);
  VERIFY(fd >= 0) << Errno();
  CHECK(!close(fd)) << Errno();
}

bool StatChanged(const char* path, const Stat& prev) {
  Stat cur;
  VERIFY(LStatPath(path, &cur)) << Errno();
  return !StatEq(prev, cur);
}

void RemoveStaleDirs(const char* root_dir) {
  DirFd dir_fd = OpenDir(root_dir);
  if (dir_fd == kNoDir) return;
  ON_SCOPE_EXIT(&) { CloseDir(dir_fd); };

  Arena arena;
  std::vector<char*> entries;
  const std::time_t now = std::time(nullptr);
  if (!ListDir(dir_fd, arena, entries,
               /* precompose_unicode = */ false,
               /* case_sensitive = */ true)) {
    return;
  }

  std::string path = root_dir;
  const size_t root_dir_len = path.size();

  for (const char* entry : entries) {
    if (std::strlen(entry) < std::strlen(kDirPrefix)) continue;
    if (std::memcmp(entry, kDirPrefix, std::strlen(kDirPrefix))) continue;

    Stat st;
    if (LStatAt(dir_fd, entry, &st)) {
      LOG(WARN) << "Cannot stat " << Print(entry) << " in " << Print(root_dir) << ": " << Errno();
      continue;
    }
    if (MTim(st).tv_sec + 10 > now) continue;

    path.resize(root_dir_len);
    path += entry;
    size_t dir_len = path.size();

    path += "/b/1";
    if (unlink(path.c_str()) && errno != ENOENT) {
      LOG(WARN) << "Cannot unlink " << Print(path) << ": " << Errno();
      continue;
    }

    for (const char* d : {"/a/1", "/a", "/b", ""}) {
      path.resize(dir_len);
      path += d;
      if (rmdir(path.c_str()) && errno != ENOENT) {
        LOG(WARN) << "Cannot remove " << Print(path) << ": " << Errno();
        break;
      }
    }
  }
}

}  // namespace

bool CheckDirMtime(const char* root_dir) {
  try {
    RemoveStaleDirs(root_dir);

    std::string tmp = std::string() + root_dir + kDirPrefix + "XXXXXX";
    VERIFY(MkdTemp(&tmp[0])) << Errno();
    ON_SCOPE_EXIT(&) { rmdir(tmp.c_str()); };

    std::string a_dir = tmp + "/a";
    VERIFY(!MkDir(a_dir.c_str())) << Errno();
    ON_SCOPE_EXIT(&) { rmdir(a_dir.c_str()); };
    Stat a_st;
    VERIFY(LStatPath(a_dir.c_str(), &a_st)) << Errno();

    std::string b_dir = tmp + "/b";
    VERIFY(!MkDir(b_dir.c_str())) << Errno();
    ON_SCOPE_EXIT(&) { rmdir(b_dir.c_str()); };
    Stat b_st;
    VERIFY(LStatPath(b_dir.c_str(), &b_st)) << Errno();

    SleepOneSecond();

    std::string a1 = a_dir + "/1";
    VERIFY(!MkDir(a1.c_str())) << Errno();
    ON_SCOPE_EXIT(&) { rmdir(a1.c_str()); };
    if (!StatChanged(a_dir.c_str(), a_st)) {
      LOG(WARN) << "Creating a directory doesn't change mtime of the parent: " << Print(root_dir);
      return false;
    }

    std::string b1 = b_dir + "/1";
    Touch(b1.c_str());
    ON_SCOPE_EXIT(&) { unlink(b1.c_str()); };
    if (!StatChanged(b_dir.c_str(), b_st)) {
      LOG(WARN) << "Creating a file doesn't change mtime of the parent: " << Print(root_dir);
      return false;
    }

    LOG(INFO) << "All mtime checks have passes. Enabling untracked cache: " << Print(root_dir);
    return true;
  } catch (const Exception&) {
    LOG(WARN) << "Error while testing for mtime capability: " << Print(root_dir);
    return false;
  }
}

}  // namespace gitstatus

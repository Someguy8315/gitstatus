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

#ifndef ROMKATV_GITSTATUS_DIR_H_
#define ROMKATV_GITSTATUS_DIR_H_

#include <cstddef>
#include <vector>

#ifdef _WIN32
#include <string>

#include "stat.h"
#include "string_view.h"
#endif

#include "arena.h"

namespace gitstatus {

#ifdef _WIN32

// Appends the UTF-8 path to out as UTF-16, converting '/' to '\'.
void AppendWidePath(std::wstring& out, StringView path);

// Returns the absolute UTF-8 path (drive or UNC; either separator) as an extended-length
// "\\?\" path, which is not limited to MAX_PATH.
std::wstring WidePath(const char* path);

// The Windows counterpart of the POSIX ListDir below. dir must be a WidePath() ending with '\'.
// Every entry additionally carries its Stat (see EntryStat()) because a single directory query
// returns names and metadata together, which spares a separate stat for each file.
bool ListDir(const std::wstring& dir, Arena& arena, std::vector<char*>& entries,
             bool case_sensitive);

// Returns the Stat of an entry produced by ListDir().
const Stat& EntryStat(const char* entry);

// Like lstat(), except that a symlink reports as the regular file or directory it is marked as.
bool LStat(const char* path, Stat& st);

#else

// On error, leaves entries unchanged and returns false. Does not throw.
//
// On success, appends names of files from the specified directory to entries and returns true.
// Every appended entry is a null-terminated string. At -1 offset is its d_type. All elements
// point into the arena. They are sorted either by strcmp or strcasecmp depending on case_sensitive.
//
// Does not close dir_fd.
//
// There are two distinct implementations of ListDir -- one for Linux and another for everything
// else. The linux-specific implementation is 20% faster.
//
// The reason sorting is bundled with directory listing is performance on Linux. The API of
// getdents64 allows for much faster sorting than what can be done with a plain vector<char*>.
// For the POSIX implementation there is no need to bundle sorting in this way. In fact, it's
// done at the end with a generic StrSort() call.
//
// For best results, reuse the arena and vector for multiple calls to avoid heap allocations.
bool ListDir(int dir_fd, Arena& arena, std::vector<char*>& entries, bool precompose_unicode,
             bool case_sensitive);

#endif  // _WIN32

}  // namespace gitstatus

#endif  // ROMKATV_GITSTATUS_DIR_H_

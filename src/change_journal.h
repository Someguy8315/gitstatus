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

#ifndef ROMKATV_GITSTATUS_CHANGE_JOURNAL_H_
#define ROMKATV_GITSTATUS_CHANGE_JOURNAL_H_

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>

namespace gitstatus {

// Reads the changes on an NTFS volume from its change journal. Unlike a directory watch, this
// needs no privileges and holds no handle that would prevent renaming the watched directories.
class ChangeJournal {
 public:
  // Calls `changed(file_id, parent_id, moved)` for every change since the previous call on the
  // volume of `dir`, a "\\?\" path. `moved` is set if the file may have left its path: deleted,
  // renamed, linked or retargeted. Returns false if the changes are unknown: on the first call,
  // if the journal is unavailable, was reset or overwritten, or if there are too many changes.
  // Changes made after the start of this call are reported by the next call.
  bool ReadChanges(const std::wstring& dir,
                   const std::function<void(uint64_t, uint64_t, bool)>& changed);

  // The serial number of the volume read by the last ReadChanges() call.
  uint32_t volume() const { return volume_; }

 private:
  uint64_t journal_id_ = 0;
  int64_t next_usn_ = -1;
  uint32_t volume_ = 0;
};

}  // namespace gitstatus

#endif  // _WIN32

#endif  // ROMKATV_GITSTATUS_CHANGE_JOURNAL_H_

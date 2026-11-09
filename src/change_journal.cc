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

#include "change_journal.h"

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>

#include "check.h"
#include "scope_guard.h"

namespace gitstatus {

bool ChangeJournal::ReadChanges(const std::wstring& dir,
                                const std::function<void(uint64_t, uint64_t, bool)>& changed) {
  // Reading more changes than this costs about as much as listing a large repository.
  constexpr int kMaxReads = 64;
  constexpr DWORD kMoved = USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME |
                           USN_REASON_RENAME_NEW_NAME | USN_REASON_HARD_LINK_CHANGE |
                           USN_REASON_REPARSE_POINT_CHANGE;

  const int64_t start = next_usn_;
  next_usn_ = -1;

  HANDLE h = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  ON_SCOPE_EXIT(&) { CHECK(CloseHandle(h)) << GetLastError(); };

  // File IDs from other file systems may not match the journal or the directory listings.
  wchar_t fs[MAX_PATH + 1];
  DWORD volume;
  if (!GetVolumeInformationByHandleW(h, nullptr, 0, &volume, nullptr, nullptr, fs, MAX_PATH + 1) ||
      std::wcscmp(fs, L"NTFS")) {
    return false;
  }

  USN_JOURNAL_DATA_V0 journal;
  DWORD n;
  if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &n,
                       nullptr)) {
    return false;
  }

  const bool known = start >= 0 && journal_id_ == journal.UsnJournalID && volume_ == volume;
  journal_id_ = journal.UsnJournalID;
  volume_ = volume;
  next_usn_ = journal.NextUsn;
  if (!known) return false;

  READ_USN_JOURNAL_DATA_V1 read = {};
  read.StartUsn = start;
  read.ReasonMask = 0xFFFFFFFF;
  read.UsnJournalID = journal.UsnJournalID;
  read.MinMajorVersion = 2;
  read.MaxMajorVersion = 2;

  alignas(8) static thread_local char buf[64 << 10];
  for (int i = 0; read.StartUsn < journal.NextUsn; ++i) {
    if (i == kMaxReads) return false;
    if (!DeviceIoControl(h, FSCTL_READ_UNPRIVILEGED_USN_JOURNAL, &read, sizeof(read), buf,
                         sizeof(buf), &n, nullptr) ||
        n < sizeof(USN)) {
      return false;
    }
    USN next;
    std::memcpy(&next, buf, sizeof(next));
    // Unprivileged reads omit file names, so records can be shorter than USN_RECORD_V2.
    constexpr DWORD kMinRecord = offsetof(USN_RECORD_V2, FileName);
    for (DWORD pos = sizeof(USN); pos + kMinRecord <= n;) {
      const auto& record = *reinterpret_cast<const USN_RECORD_V2*>(buf + pos);
      if (record.MajorVersion != 2 || record.RecordLength < kMinRecord) return false;
      changed(record.FileReferenceNumber, record.ParentFileReferenceNumber,
              (record.Reason & kMoved) != 0);
      pos += record.RecordLength;
    }
    if (next <= read.StartUsn) break;
    read.StartUsn = next;
  }
  return true;
}

}  // namespace gitstatus

#endif  // _WIN32
